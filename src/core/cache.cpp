// core/cache.cpp — see cache.h.

#include "cache.h"
#include "cache_detail.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#endif
#if !defined(__linux__)
#include <sys/stat.h>
#endif

#include "platform.h"

namespace cache {
namespace {

constexpr int kRetryAttempts = 3;
constexpr int kRetrySleepMs = 50;

bool is_retryable(int err) {
  return err == EINTR || err == EAGAIN || err == EBUSY;
}

const std::function<void()> pause_retry = [] {
  std::this_thread::sleep_for(std::chrono::milliseconds(kRetrySleepMs));
};

int64_t bytes2pages(int64_t bytes) {
  const auto page_size = platform::page_size();
  return bytes / page_size + (bytes % page_size != 0);
}

// Runs one syscall with the retry policy. `call` returns 0 on success and
// nonzero on failure. out_err receives the error code (errno, or the call's
// own return value routed through errno for errno-less calls such as
// posix_fadvise). Never throws: retryable-after-N is reported as a failure.
template <typename Call>
bool retry_op(Call &&call, int &out_err, const std::function<void()> &pause = pause_retry) {
  int err = 0;
  for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
    errno = 0; // each attempt classifies its own errno
    if (call() == 0) {
      out_err = 0;
      return true;
    }
    err = errno;
    if (!is_retryable(err))
      break;
    if (err != EINTR && attempt + 1 < kRetryAttempts)
      pause();
  }
  out_err = err;
  return false;
}

// Working state shared by the prelude and the verbs.
struct Mapping {
  MappedRegion region;
  int64_t len_of_file = 0;
  int64_t len_of_range = 0;
  int64_t pages_in_range = 0;
};

// Common steps before a verb: fstat, block-device size, empty-file and
// max-size gates, range selection, mmap and alignment check. Returns Ok
// with the mapping established, or Error for a per-file failure.
// skipped_ok is set for an empty file (silent Ok, no record).
vmt::Result<void> prelude(int fd, const Params &p, Mapping &m, bool &skipped_ok) {
  skipped_ok = false;
  int err = 0;

  if (p.range.offset < 0 || p.range.max_len < 0 || p.range.offset % platform::page_size() != 0)
    return vmt::Result<void>::Error(Failure{Failure::Kind::Map, EINVAL, "", p.range.offset});

  struct stat sb;
  if (!retry_op([&] { return fstat(fd, &sb); }, err))
    return vmt::Result<void>::Error(Failure{Failure::Kind::FstatFailed, err, "", 0});
  if (S_ISBLK(sb.st_mode)) {
#if defined(__linux__)
    if (!retry_op([&] { return ioctl(fd, BLKGETSIZE64, &m.len_of_file); }, err))
      return vmt::Result<void>::Error(Failure{Failure::Kind::IoctlSizeFailed, err, "", 0});
#else
    throw FatalError("discovering size of block devices not (yet?) supported on this platform");
#endif
  } else {
    m.len_of_file = sb.st_size;
  }

  if (m.len_of_file == 0) {
    skipped_ok = true; // empty: silent Ok, no record
    return vmt::Result<void>::Ok();
  }

  if (static_cast<uint64_t>(m.len_of_file) > p.max_file_size)
    return vmt::Result<void>::Error(Failure{Failure::Kind::TooLarge, 0, "", 0});

  // ---- range selection --------------------------------------------------

  // Unsigned arithmetic for the range end: offset and max_len are
  // non-negative user input capped at INT64_MAX, so their sum can exceed
  // INT64_MAX and would be UB as signed addition.
  const uint64_t range_end =
      static_cast<uint64_t>(p.range.offset) + static_cast<uint64_t>(p.range.max_len);

  if (p.range.max_len > 0 && range_end < static_cast<uint64_t>(m.len_of_file)) {
    m.len_of_range = p.range.max_len;
  } else if (p.range.offset >= m.len_of_file) {
    return vmt::Result<void>::Error(Failure{Failure::Kind::BeyondEof, 0, "", p.range.offset});
  } else {
    m.len_of_range = m.len_of_file - p.range.offset;
  }

  // ---- map --------------------------------------------------------------

  void *mem = MAP_FAILED;
  int map_err = 0;
  if (!retry_op(
          [&] {
            mem = mmap(nullptr, m.len_of_range, PROT_READ, MAP_SHARED, fd, p.range.offset);
            return mem == MAP_FAILED ? -1 : 0;
          },
          map_err))
    return vmt::Result<void>::Error(Failure{Failure::Kind::Map, map_err, "", 0});

  if (!platform::aligned_p(mem)) {     // infrastructure failure
    (void)munmap(mem, m.len_of_range); // best effort; the Fatal is the report
    throw FatalError("mmap wasn't page aligned");
  }

  m.region = MappedRegion(mem, m.len_of_range);
  m.pages_in_range = bytes2pages(m.len_of_range);
  return vmt::Result<void>::Ok();
}

// Counts resident pages in the mapped range. The retry_op result is
// checked: on failure the caller gets Failed{MincoreFailed} rather than a
// count derived from an unwritten buffer.
vmt::Result<int64_t> mincore_count(const Mapping &m, const detail::Syscalls &calls) {
  std::vector<unsigned char> mincore_array(m.pages_in_range);

  int err = 0;
  if (!retry_op([&] { return calls.mincore(m.region.mem(), m.len_of_range, mincore_array.data()); },
                err, calls.pause_retry))
    return vmt::Result<int64_t>::Error(Failure{Failure::Kind::MincoreFailed, err, "", 0});

  int64_t pages_resident = 0;
  for (int64_t i = 0; i < m.pages_in_range; i++) {
    if (platform::is_mincore_page_resident(mincore_array[i]))
      pages_resident++;
  }

  return vmt::Result<int64_t>::Ok(pages_resident);
}

// A volatile read faults in the first byte of every page and cannot be elided.
void touch_pages(const Mapping &m) {
  const auto page_size = platform::page_size();
  const auto *bytes = static_cast<const volatile char *>(m.region.mem());
  for (int64_t i = 0; i < m.pages_in_range; i++)
    static_cast<void>(bytes[i * page_size]);
}

// Unmaps a region inside a verb. unmap() clears the handle only on
// success, so the retry genuinely re-attempts; on failure the still-live
// region is released by the destructor. Error ownership: unmap() reports
// the raw errno through its out-param; retry_op owns the retry policy and
// its out_err receives the same value on the first (non-retryable) failure.
bool unmap_or_fail(MappedRegion &region, int &err,
                   const detail::Syscalls &calls = detail::real_syscalls()) {
  return retry_op(
      [&] {
        int unmap_err = 0;
        if (calls.unmap(region, unmap_err))
          return 0;
        errno = unmap_err;
        return -1;
      },
      err, calls.pause_retry);
}

} // namespace

const detail::Syscalls &detail::real_syscalls() {
  static const Syscalls calls{
      .mincore = [](void *mem, int64_t len,
                    unsigned char *vec) { return ::mincore(mem, len, vec); },
      .evict = [](int fd, int64_t offset, const MappedRegion &region) -> int {
#if defined(__linux__) || defined(__hpux)
        return ::posix_fadvise(fd, offset, region.len(), POSIX_FADV_DONTNEED);
#elif defined(__FreeBSD__) || defined(__sun__) || defined(__APPLE__)
        (void)fd;
        (void)offset;
        return ::msync(region.mem(), region.len(), MS_INVALIDATE) == 0 ? 0 : errno;
#else
        (void)fd;
        (void)offset;
        (void)region;
        throw FatalError("cache eviction not (yet?) supported on this platform");
#endif
      },
      .unmap = [](MappedRegion &region, int &err) { return region.unmap(err); },
      .pause_retry = pause_retry,
  };
  return calls;
}

// ---------------------------------------------------------------------------
// The four verbs.
// ---------------------------------------------------------------------------

vmt::Result<VerbOutcome> inspect(int fd, const Params &p) {
  return detail::inspect(fd, p, detail::real_syscalls());
}

vmt::Result<VerbOutcome> detail::inspect(int fd, const Params &p, const Syscalls &calls) {
  Mapping m;
  bool empty = false;
  if (auto pr = prelude(fd, p, m, empty); pr.is_error())
    return vmt::Result<VerbOutcome>::Error(pr.error());
  if (empty)
    return vmt::Result<VerbOutcome>::Ok(VerbOutcome{}); // silent Ok, zero stats

  auto resident = mincore_count(m, calls);
  if (resident.is_error())
    return vmt::Result<VerbOutcome>::Error(resident.error());

  VerbOutcome out;
  out.stats.total_pages = m.pages_in_range;
  out.stats.total_pages_in_core = resident.value();
  int munmap_err = 0;
  if (!unmap_or_fail(m.region, munmap_err, calls))
    return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MunmapFailed, munmap_err, "", 0});
  return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

vmt::Result<VerbOutcome> warm(int fd, const Params &p) {
  Mapping m;
  bool empty = false;
  if (auto pr = prelude(fd, p, m, empty); pr.is_error())
    return vmt::Result<VerbOutcome>::Error(pr.error());
  if (empty)
    return vmt::Result<VerbOutcome>::Ok(VerbOutcome{});

  touch_pages(m);

  VerbOutcome out;
  out.stats.total_pages = m.pages_in_range;
  out.stats.total_pages_in_core = m.pages_in_range; // all pages touched
  if (p.retain_warm_mapping) {
    out.region = std::move(m.region);
    return vmt::Result<VerbOutcome>::Ok(std::move(out));
  }
  int munmap_err = 0;
  if (!unmap_or_fail(m.region, munmap_err))
    return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MunmapFailed, munmap_err, "", 0});
  return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

vmt::Result<VerbOutcome> evict(int fd, const Params &p) {
  return detail::evict(fd, p, detail::real_syscalls());
}

vmt::Result<VerbOutcome> detail::evict(int fd, const Params &p, const Syscalls &calls) {
  Mapping m;
  bool empty = false;
  if (auto pr = prelude(fd, p, m, empty); pr.is_error())
    return vmt::Result<VerbOutcome>::Error(pr.error());
  if (empty)
    return vmt::Result<VerbOutcome>::Ok(VerbOutcome{});

#if defined(__linux__) || defined(__hpux)
  constexpr auto failure_kind = Failure::Kind::EvictAdviseFailed;
#elif defined(__FreeBSD__) || defined(__sun__) || defined(__APPLE__)
  constexpr auto failure_kind = Failure::Kind::EvictInvalidateFailed;
#else
  constexpr auto failure_kind = Failure::Kind::EvictAdviseFailed; // real adapter throws
#endif
  int err = 0;
  // posix_fadvise returns its error, rather than setting errno.
  if (!retry_op(
          [&] {
            const int rc = calls.evict(fd, p.range.offset, m.region);
            if (rc != 0)
              errno = rc;
            return rc;
          },
          err, calls.pause_retry))
    return vmt::Result<VerbOutcome>::Error(Failure{failure_kind, err, "", 0});

  VerbOutcome out;
  out.stats.total_pages = m.pages_in_range;
  int munmap_err = 0;
  if (!unmap_or_fail(m.region, munmap_err, calls))
    return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MunmapFailed, munmap_err, "", 0});
  return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

vmt::Result<VerbOutcome> lock(int fd, const Params &p) {
  Mapping m;
  bool empty = false;
  if (auto pr = prelude(fd, p, m, empty); pr.is_error())
    return vmt::Result<VerbOutcome>::Error(pr.error());
  if (empty)
    return vmt::Result<VerbOutcome>::Ok(VerbOutcome{});

  touch_pages(m); // -l implies touch

  int err = 0;
  if (!retry_op([&] { return mlock(m.region.mem(), m.len_of_range); }, err))
    return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MlockFailed, err, "", 0});

  // No munmap: the region is handed to the caller and stays mapped.
  VerbOutcome out;
  out.stats.total_pages = m.pages_in_range;
  out.stats.total_pages_in_core = m.pages_in_range;
  out.region = std::move(m.region);
  return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

} // namespace cache
