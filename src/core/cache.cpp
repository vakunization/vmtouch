// core/cache.cpp — see cache.h (spec2 §7).
//
// Structure: one shared prelude (fstat -> block-device size -> empty ->
// -m -> range -> mmap -> alignment), then the per-verb body. Syscalls are
// wrapped by retry_op(): 3 attempts, 50 ms, only EINTR/EAGAIN/EBUSY;
// retryable-after-N on mmap/munmap/mincore/mlock/fadvise/msync is FATAL
// (spec2 §8.3) — non-retryable errors become Result::Error(Failure).
//
// The control flow (ranges, page math, the volatile touch-read loop that
// IS the feature, the -l keep-mapping rule) is unchanged from the
// pre-migration engine; what changed is the reporting surface: no paths,
// no sinks, no progress events (spec §7.3: cache knows fd + Params only).

#include "cache.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

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

constexpr int kRetryAttempts = 3;                  // spec2 §7.11
constexpr int kRetrySleepMs = 50;

bool is_retryable(int err) {
    return err == EINTR || err == EAGAIN || err == EBUSY;   // spec2 §8.3
}

int64_t bytes2pages(int64_t bytes) {
    return (bytes + pagesize - 1) / pagesize;
}

// Runs one syscall with the retry policy. `call` returns 0 on success,
// nonzero on failure; out_err receives the error code (errno, or the
// call's own return value for errno-less calls like posix_fadvise —
// MISS-2). R2 adjudication (2026-10-05): retryable-after-N is a per-file
// Failed, NOT a fatal — this helper NEVER throws. cache::* throws
// FatalError only for infrastructural causes (allocation, misaligned
// mmap, unsupported platform).
template <typename Call>
bool retry_op(const char* /*fatal_what*/, Call&& call, int& out_err) {
    int err = 0;
    for (int attempt = 0; attempt < kRetryAttempts; ++attempt) {
        errno = 0;   // each attempt classifies its own errno (MISS-2)
        if (call() == 0) {
            out_err = 0;
            return true;
        }
        err = errno;   // errno-less calls must return their code via errno
        if (!is_retryable(err))
            break;
        if (attempt + 1 < kRetryAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetrySleepMs));
    }
    out_err = err;
    return false;
}

// Internal working state shared by the prelude and the verbs.
struct Mapping {
    MappedRegion region;
    struct stat st = {};        // prelude fstat: permission heuristic site
    int64_t len_of_file = 0;
    int64_t len_of_range = 0;
    int64_t pages_in_range = 0;
};

// Prelude (spec2 §7.6). Outcomes:
//   * empty file -> skipped_ok = true, Ok (caller returns silent Ok);
//   * Failure    -> Error record (path empty);
//   * fatal      -> throws (platform).
// Returns Ok (mapping established, m filled) or Error.
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
        skipped_ok = true;      // empty: silent Ok, no record (spec2 §7.13)
        return vmt::Result<void>::Ok();
    }

    if (static_cast<uint64_t>(m.len_of_file) > p.max_file_size)
        return vmt::Result<void>::Error(Failure{Failure::Kind::TooLarge, 0, "", 0});

    // ---- range selection (verbatim) --------------------------------------

    if (p.range.max_len > 0 && (p.range.offset + p.range.max_len) < m.len_of_file) {
        m.len_of_range = p.range.max_len;
    } else if (p.range.offset >= m.len_of_file) {
        return vmt::Result<void>::Error(Failure{Failure::Kind::BeyondEof, 0, "", p.range.offset});
    } else {
        m.len_of_range = m.len_of_file - p.range.offset;
    }

    // ---- map --------------------------------------------------------------

    // DEV-3 (R2): mmap goes through retry_op like every other syscall.
    // Non-retryable AND retryable-after-N -> Failed{Map}; no throw.
    void* mem = MAP_FAILED;
    int map_err = 0;
    if (!retry_op("mmap", [&] {
            mem = mmap(NULL, m.len_of_range, PROT_READ, MAP_SHARED, fd, p.range.offset);
            return mem == MAP_FAILED ? -1 : 0;
        }, map_err))
        return vmt::Result<void>::Error(Failure{Failure::Kind::Map, map_err, "", 0});

    if (!platform::aligned_p(mem)) {   // infrastructure: fatal (spec2 §8.3)
        munmap(mem, m.len_of_range);
        throw FatalError("mmap wasn't page aligned");
    }

    m.region = MappedRegion(mem, m.len_of_range);
    m.pages_in_range = bytes2pages(m.len_of_range);
    return vmt::Result<void>::Ok();
}

// mincore pass: counts residency. BUG-1 fix (2026-10-05): the retry_op
// result IS checked — on failure (non-retryable AND retryable-after-N per
// the R2 adjudication) the verb returns Failed{MincoreFailed} instead of
// counting residency from an uninitialized array. FatalError is gone from
// this path entirely (DEV-2 adjudication). Scope: inspect only — DEC-4(a)
// removed the mincore call from warm/lock (landing in the cleanup commit).
vmt::Result<int64_t> mincore_count(const Mapping& m) {
    auto* mincore_array = static_cast<unsigned char*>(malloc(m.pages_in_range));
    if (mincore_array == NULL)
        throw FatalError(std::string("Failed to allocate memory for mincore array (") + strerror(errno) + ")");

    int err = 0;
    if (!retry_op("mincore", [&] {
            return mincore(m.region.mem(), m.len_of_range, mincore_array);
        }, err))
        return vmt::Result<int64_t>::Error(Failure{Failure::Kind::MincoreFailed, err, "", 0});

    int64_t pages_resident = 0;
    for (int64_t i = 0; i < m.pages_in_range; i++) {
        if (platform::is_mincore_page_resident(mincore_array[i]))
            pages_resident++;
    }

    free(mincore_array);
    return vmt::Result<int64_t>::Ok(pages_resident);
}

// The volatile touch-read loop — reading the first byte of every page IS
// the feature (kept by decision; see the pre-migration history for why the
// accumulator is explicitly read back).
void touch_pages(const Mapping& m) {
    volatile uint64_t j = 0;
    for (int64_t i = 0; i < m.pages_in_range; i++)
        j += (static_cast<char*>(m.region.mem()))[i * pagesize];
    uint64_t touched = j;
    static_cast<void>(touched);
}

// munmap inside the verb for inspect/warm/evict (spec2 §7.12). MISS-1 fix
// (unmap option (a), 2026-10-05): unmap() clears the handle only on
// success, so the retry below genuinely re-attempts munmap (previously the
// first failure cleared the handle and every "retry" trivially succeeded).
// R2: failure after retry is a per-file Failed{MunmapFailed}, not a fatal;
// the (still live) region is then released by MappedRegion's destructor —
// no leak, no double-unmap.
bool unmap_or_fail(MappedRegion& region, int& err) {
    return retry_op("munmap", [&] { return region.unmap(err) ? 0 : -1; }, err);
}

} // namespace

// ---------------------------------------------------------------------------
// The four verbs (spec2 §7.7 - §7.10).
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

    if (auto resident = mincore_count(m); resident.is_error())
        return vmt::Result<VerbOutcome>::Error(resident.error());

    touch_pages(m);

    VerbOutcome out;
    out.stats.total_pages = m.pages_in_range;
    out.stats.total_pages_in_core = m.pages_in_range;   // all touched (spec2 §7.8)
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
    // MISS-2: posix_fadvise RETURNS the error code and does not set errno.
    // The lambda routes its return value through errno (zeroed by retry_op
    // before each attempt) so classification and Failure.err are correct.
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

    if (auto resident = mincore_count(m); resident.is_error())
        return vmt::Result<VerbOutcome>::Error(resident.error());

    touch_pages(m);   // -l implies touch

    // DEV-1 adjudication (2026-10-05): the FATAL freeze is overturned.
    // mlock failure -> Failed{MlockFailed}; traversal continues. R2 makes
    // this true for the retryable-after-N path as well (retry_op no longer
    // throws).
    int err = 0;
    if (!retry_op("mlock", [&] {
            return mlock(m.region.mem(), m.len_of_range);
        }, err))
        return vmt::Result<VerbOutcome>::Error(Failure{Failure::Kind::MlockFailed, err, "", 0});

    // NO munmap: the region is handed to the caller (spec2 §7.10).
    VerbOutcome out;
    out.stats.total_pages = m.pages_in_range;
    out.stats.total_pages_in_core = m.pages_in_range;
    out.region = std::move(m.region);
    return vmt::Result<VerbOutcome>::Ok(std::move(out));
}

} // namespace cache
