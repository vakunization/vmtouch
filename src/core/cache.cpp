// core/cache.cpp — see cache.h.

#include "cache.h"

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

int64_t bytes2pages(int64_t bytes) {
    return (bytes + pagesize - 1) / pagesize;
}

// Runs one syscall with the retry policy. `call` returns 0 on success and
// nonzero on failure. out_err receives the error code (errno, or the call's
// own return value routed through errno for errno-less calls such as
// posix_fadvise). Never throws: retryable-after-N is reported as a failure.
template <typename Call>
bool retry_op(const char* /*label*/, Call&& call, int& out_err) {
    int err = 0;
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        errno = 0;   // each attempt classifies its own errno
        if (call() == 0) {
            out_err = 0;
            return true;
        }
        err = errno;
        if (!is_retryable(err))
            break;
        if (attempt + 1 < kRetryAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetrySleepMs));
    }
    out_err = err;
    return false;
}

// Working state shared by the prelude and the verbs.
struct Mapping {
    MappedRegion region;
    struct stat st = {};        // fstat result (permission heuristic input)
    int64_t len_of_file = 0;
    int64_t len_of_range = 0;
    int64_t pages_in_range = 0;
};

// Common steps before a verb: fstat, block-device size, empty-file and
// max-size gates, range selection, mmap and alignment check. Returns Ok
// with the mapping established, or Error for a per-file failure.
// skipped_ok is set for an empty file (silent Ok, no record).
vmt::Result<void> prelude(int fd, const Params& p, Mapping& m, bool& skipped_ok) {
    skipped_ok = false;
    int err = 0;

    struct stat sb;
    if (!retry_op("fstat", [&] { return fstat(fd, &sb); }, err))
        return vmt::Result<void>::Error(Failure{Failure::Kind::FstatFailed, err, "", 0});
    m.st = sb;

    if (S_ISBLK(sb.st_mode)) {
#if defined(__linux__)
        if (!retry_op("ioctl", [&] { return ioctl(fd, BLKGETSIZE64, &m.len_of_file); }, err))
            return vmt::Result<void>::Error(Failure{Failure::Kind::IoctlSizeFailed, err, "", 0});
#else
        throw FatalError("discovering size of block devices not (yet?) supported on this platform");
#endif
    } else {
        m.len_of_file = sb.st_size;
    }

    if (m.len_of_file == 0) {
        skipped_ok = true;      // empty: silent Ok, no record
        return vmt::Result<void>::Ok();
    }

    if (static_cast<uint64_t>(m.len_of_file) > p.max_file_size)
        return vmt::Result<void>::Error(Failure{Failure::Kind::TooLarge, 0, "", 0});

    // ---- range selection --------------------------------------------------

    if (p.range.max_len > 0 && (p.range.offset + p.range.max_len) < m.len_of_file) {
        m.len_of_range = p.range.max_len;
    } else if (p.range.offset >= m.len_of_file) {
        return vmt::Result<void>::Error(Failure{Failure::Kind::BeyondEof, 0, "", p.range.offset});
    } else {
        m.len_of_range = m.len_of_file - p.range.offset;
    }

    // ---- map --------------------------------------------------------------

    void* mem = MAP_FAILED;
    int map_err = 0;
    if (!retry_op("mmap", [&] {
            mem = mmap(NULL, m.len_of_range, PROT_READ, MAP_SHARED, fd, p.range.offset);
            return mem == MAP_FAILED ? -1 : 0;
        }, map_err))
        return vmt::Result<void>::Error(Failure{Failure::Kind::Map, map_err, "", 0});

    if (!platform::aligned_p(mem)) {   // infrastructure failure
        munmap(mem, m.len_of_range);
        throw FatalError("mmap wasn't page aligned");
    }

    m.region = MappedRegion(mem, m.len_of_range);
    m.pages_in_range = bytes2pages(m.len_of_range);
    return vmt::Result<void>::Ok();
}

// Counts resident pages in the mapped range. The retry_op result is
// checked: on failure the caller gets Failed{MincoreFailed} rather than a
// count derived from an unwritten buffer.
vmt::Result<int64_t> mincore_count(const Mapping& m) {
    std::vector<unsigned char> mincore_array(m.pages_in_range);

    int err = 0;
    if (!retry_op("mincore", [&] {
            return mincore(m.region.mem(), m.len_of_range, mincore_array.data());
        }, err))
        return vmt::Result<int64_t>::Error(Failure{Failure::Kind::MincoreFailed, err, "", 0});

    int64_t pages_resident = 0;
    for (int64_t i = 0; i < m.pages_in_range; i++) {
        if (platform::is_mincore_page_resident(mincore_array[i]))
            pages_resident++;
    }

    return vmt::Result<int64_t>::Ok(pages_resident);
}

// Reads the first byte of every page; this access IS the feature (it faults
// pages in). The accumulator is read back so the loop is not optimized away.
void touch_pages(const Mapping& m) {
    volatile uint64_t j = 0;
    for (int64_t i = 0; i < m.pages_in_range; i++)
        j += (static_cast<char*>(m.region.mem()))[i * pagesize];
    uint64_t touched = j;
    static_cast<void>(touched);
}

// Unmaps a region inside a verb. unmap() clears the handle only on
// success, so the retry genuinely re-attempts; on failure the still-live
// region is released by the destructor.
bool unmap_or_fail(MappedRegion& region, int& err) {
    return retry_op("munmap", [&] { return region.unmap(err) ? 0 : -1; }, err);
}

} // namespace

// ---------------------------------------------------------------------------
// The four verbs.
// ---------------------------------------------------------------------------

vmt::Result<VerbOutcome> inspect(int fd, const Params& p) {
    Mapping m;
    bool empty = false;
    if (auto pr = prelude(fd, p, m, empty); pr.is_error())
        return vmt::Result<VerbOutcome>::Error(pr.error());
    if (empty)
        return vmt::Result<VerbOutcome>::Ok(VerbOutcome{});   // silent Ok, zero stats

    auto resident = mincore_count(m);
    if (resident.is_error())
        return vmt::Result<VerbOutcome>::Error(resident.error());

    VerbOutcome out;
    out.stats.total_pages = m.pages_in_range;
    out.stats.total_pages_in_core = resident.value();
    int munmap_err = 0;
    if (!unmap_or_fail(m.region, munmap_err))
        return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MunmapFailed, munmap_err, "", 0});
    return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

vmt::Result<VerbOutcome> warm(int fd, const Params& p) {
    Mapping m;
    bool empty = false;
    if (auto pr = prelude(fd, p, m, empty); pr.is_error())
        return vmt::Result<VerbOutcome>::Error(pr.error());
    if (empty)
        return vmt::Result<VerbOutcome>::Ok(VerbOutcome{});

    touch_pages(m);

    VerbOutcome out;
    out.stats.total_pages = m.pages_in_range;
    out.stats.total_pages_in_core = m.pages_in_range;   // all pages touched
    int munmap_err = 0;
    if (!unmap_or_fail(m.region, munmap_err))
        return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MunmapFailed, munmap_err, "", 0});
    return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

vmt::Result<VerbOutcome> evict(int fd, const Params& p) {
    Mapping m;
    bool empty = false;
    if (auto pr = prelude(fd, p, m, empty); pr.is_error())
        return vmt::Result<VerbOutcome>::Error(pr.error());
    if (empty)
        return vmt::Result<VerbOutcome>::Ok(VerbOutcome{});

    int err = 0;
#if defined(__linux__) || defined(__hpux)
    // posix_fadvise returns the error code and does not set errno, so the
    // lambda routes its return value through errno for classification.
    const bool ok = retry_op("posix_fadvise", [&] {
        const int rc = posix_fadvise(fd, p.range.offset, m.len_of_range, POSIX_FADV_DONTNEED);
        if (rc != 0)
            errno = rc;
        return rc;
    }, err);
    if (!ok)
        return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::EvictAdviseFailed, err, "", 0});
#elif defined(__FreeBSD__) || defined(__sun__) || defined(__APPLE__)
    const bool ok = retry_op("msync", [&] {
        return msync(m.region.mem(), m.len_of_range, MS_INVALIDATE);
    }, err);
    if (!ok)
        return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::EvictInvalidateFailed, err, "", 0});
#else
    throw FatalError("cache eviction not (yet?) supported on this platform");
#endif

    VerbOutcome out;
    out.stats.total_pages = m.pages_in_range;
    int munmap_err = 0;
    if (!unmap_or_fail(m.region, munmap_err))
        return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MunmapFailed, munmap_err, "", 0});
    return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

vmt::Result<VerbOutcome> lock(int fd, const Params& p) {
    Mapping m;
    bool empty = false;
    if (auto pr = prelude(fd, p, m, empty); pr.is_error())
        return vmt::Result<VerbOutcome>::Error(pr.error());
    if (empty)
        return vmt::Result<VerbOutcome>::Ok(VerbOutcome{});

    touch_pages(m);   // -l implies touch

    int err = 0;
    if (!retry_op("mlock", [&] {
            return mlock(m.region.mem(), m.len_of_range);
        }, err))
        return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MlockFailed, err, "", 0});

    // No munmap: the region is handed to the caller and stays mapped.
    VerbOutcome out;
    out.stats.total_pages = m.pages_in_range;
    out.stats.total_pages_in_core = m.pages_in_range;
    out.region = std::move(m.region);
    return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

} // namespace cache
