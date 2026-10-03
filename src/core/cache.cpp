// core/cache.cpp — see cache.h.
//
// A2: process_file() dissolved into four verbs (inspect/warm/evict/lock)
// sharing one prelude. Control flow, syscall sequence and print points are
// transplanted verbatim from the Increment-1 engine (itself verbatim from
// the monolith's vmtouch_file()). Deliberate deviations, each required by
// the A2 seam:
//   * no path: fatal messages that historically embedded the path now come
//     back as FileResult::fatal facts; the session re-adds the path;
//   * no verbose flag: progress events fire iff the EngineEvents pointer
//     is non-null (the session's adapter binds the sink + path);
//   * stats are returned as data (FileResult fields), not mutated in place;
//   * the mapping is RETURNED (MappedRegion), never munmapped here — the
//     session drops or keeps it exactly where upstream's bail-out did.
// A1's coarse Failure kinds are kept as-is (known, user-parked question:
// five rare warning texts drift from upstream — out of A2's scope).

#include "cache.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

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

int64_t bytes2pages(int64_t bytes) {
    return (bytes + pagesize - 1) / pagesize;
}

constexpr double kChartUpdateInterval = 0.1;

// Internal working state shared by the prelude and the verbs.
struct Mapping {
    MappedRegion region;
    struct stat st = {};        // from the prelude's fstat: can_do_mincore reads it
    int64_t len_of_file = 0;
    int64_t len_of_range = 0;
    int64_t pages_in_range = 0;
};

// Shared prelude: fstat -> block-device size -> empty -> -m -> range ->
// mmap. Returns Ok (mapping established) or the skip/fatal fact.
FileResult prelude(int fd, const Params& p, Mapping& m) {
    struct stat sb;
    if (fstat(fd, &sb)) {
        FileResult r;
        r.code = FileResult::Code::Skipped;
        r.skip = Failure::Kind::FstatFailed;   // upstream: "unable to fstat ... skipping"
        r.err = errno;
        return r;
    }
    m.st = sb;

    if (S_ISBLK(sb.st_mode)) {
#if defined(__linux__)
        if (ioctl(fd, BLKGETSIZE64, &m.len_of_file)) {
            FileResult r;
            r.code = FileResult::Code::Skipped;
            r.skip = Failure::Kind::IoctlSizeFailed;   // upstream: "unable to ioctl"
            r.err = errno;
            return r;
        }
#else
        throw FatalError("discovering size of block devices not (yet?) supported on this platform");
#endif
    } else {
        m.len_of_file = sb.st_size;
    }

    if (m.len_of_file == 0) {
        FileResult r;                                   // empty: silent skip, as upstream
        r.code = FileResult::Code::Skipped;
        r.silent = true;
        return r;
    }

    if (static_cast<uint64_t>(m.len_of_file) > p.max_file_size) {
        FileResult r;
        r.code = FileResult::Code::Skipped;
        r.skip = Failure::Kind::TooLarge;
        return r;
    }

    // ---- range selection (verbatim) --------------------------------------

    if (p.range.max_len > 0 && (p.range.offset + p.range.max_len) < m.len_of_file) {
        m.len_of_range = p.range.max_len;
    } else if (p.range.offset >= m.len_of_file) {
        FileResult r;
        r.code = FileResult::Code::Skipped;
        r.skip = Failure::Kind::BeyondEof;
        r.offset = p.range.offset;
        return r;
    } else {
        m.len_of_range = m.len_of_file - p.range.offset;
    }

    // ---- map --------------------------------------------------------------

    void* mem = mmap(NULL, m.len_of_range, PROT_READ, MAP_SHARED, fd, p.range.offset);

    if (mem == MAP_FAILED) {
        FileResult r;
        r.code = FileResult::Code::Skipped;
        r.skip = Failure::Kind::Map;
        r.err = errno;
        return r;
    }

    if (!platform::aligned_p(mem)) {
        FileResult r;                                   // session re-adds the path
        r.code = FileResult::Code::Fatal;
        r.fatal = FileResult::FatalWhat::MisalignedMap;
        return r;
    }

    m.region = MappedRegion(mem, m.len_of_range);
    m.pages_in_range = bytes2pages(m.len_of_range);
    return FileResult{};
}

// mincore pass shared by inspect/warm/lock: counts residency, fires the
// verbose events at the old print points. `touch` performs the volatile
// touch-read loop (the feature itself) with the throttled live chart.
// Returns the Fatal fact for mincore failure (or throws for allocation
// failure, as before).
FileResult mincore_pass(const Mapping& m, bool touch, EngineEvents* ev,
                    int64_t& pages_resident_out) {
    FileResult r;
    double last_chart_print_time = 0.0;

    auto* mincore_array = static_cast<unsigned char*>(malloc(m.pages_in_range));
    if (mincore_array == NULL)
        throw FatalError(std::string("Failed to allocate memory for mincore array (") + strerror(errno) + ")");

    // 3rd arg to mincore is char* on BSD and unsigned char* on linux
    if (mincore(m.region.mem(), m.len_of_range, mincore_array)) {
        free(mincore_array);
        r.code = FileResult::Code::Fatal;
        r.fatal = FileResult::FatalWhat::MincoreFailed;   // session re-adds the path
        r.err = errno;
        return r;
    }

    int64_t pages_resident = 0;
    for (int64_t i = 0; i < m.pages_in_range; i++) {
        if (platform::is_mincore_page_resident(mincore_array[i]))
            pages_resident++;
    }

    if (ev) {
        ev->on_file_start();
#ifdef __linux__
        if (!platform::can_do_mincore(m.st)) {
            r.mincore_inaccurate = true;
        }
#endif
        last_chart_print_time = platform::now_seconds();
        ev->on_chart(std::span<const unsigned char>(mincore_array, m.pages_in_range), m.pages_in_range);
    }

    if (touch) {
        // The volatile accumulator forces the per-page load — that load IS
        // the -t/-l feature (the volatile read loop from upstream, kept by
        // decision). Newer gcc flags the accumulator as "set but not used"
        // under -Werror (only the volatile object, never its value, was
        // observable). One explicit read-back of `j` after the loop
        // satisfies the warning without altering the volatile semantics.
        volatile uint64_t j = 0;
        for (int64_t i = 0; i < m.pages_in_range; i++) {
            j += (static_cast<char*>(m.region.mem()))[i * pagesize];
            mincore_array[i] = 1;

            if (ev) {
                double temp_time = platform::now_seconds();
                if (temp_time > (last_chart_print_time + kChartUpdateInterval)) {
                    last_chart_print_time = temp_time;
                    ev->on_chart(std::span<const unsigned char>(mincore_array, m.pages_in_range), m.pages_in_range);
                }
            }
        }
        uint64_t touched = j;   // explicit volatile read-back (also -Werror fix)
        static_cast<void>(touched);
        pages_resident = m.pages_in_range;
    }

    if (ev) {
        // was: final print_page_residency_chart(...) + printf("\n") ending
        // the verbose block. on_file_done replaces the trailing newline;
        // the CLI's ChartSink emits "\n" inside on_file_done.
        ev->on_chart(std::span<const unsigned char>(mincore_array, m.pages_in_range), m.pages_in_range);
        ev->on_file_done(m.pages_in_range, pages_resident);
    }

    free(mincore_array);
    pages_resident_out = pages_resident;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// The four verbs.
// ---------------------------------------------------------------------------

FileResult inspect(int fd, const Params& p, EngineEvents* ev) {
    Mapping m;
    if (FileResult r = prelude(fd, p, m); r.code != FileResult::Code::Ok)
        return r;

    int64_t pages_resident = 0;
    FileResult r = mincore_pass(m, /*touch=*/false, ev, pages_resident);
    if (r.code == FileResult::Code::Fatal)
        return r;

    r.pages_total = m.pages_in_range;
    r.pages_resident = pages_resident;
    r.region = std::move(m.region);
    return r;
}

FileResult warm(int fd, const Params& p, EngineEvents* ev) {
    Mapping m;
    if (FileResult r = prelude(fd, p, m); r.code != FileResult::Code::Ok)
        return r;

    int64_t pages_resident = 0;
    FileResult r = mincore_pass(m, /*touch=*/true, ev, pages_resident);
    if (r.code == FileResult::Code::Fatal)
        return r;

    r.pages_total = m.pages_in_range;
    r.pages_resident = pages_resident;
    r.region = std::move(m.region);
    return r;
}

FileResult evict(int fd, const Params& p, EngineEvents* ev) {
    Mapping m;
    if (FileResult r = prelude(fd, p, m); r.code != FileResult::Code::Ok)
        return r;

    if (ev) ev->on_evicting();

    FileResult r;
#if defined(__linux__) || defined(__hpux)
    if (posix_fadvise(fd, p.range.offset, m.len_of_range, POSIX_FADV_DONTNEED)) {
        r.evict_failed = true;
        r.evict_kind = Failure::Kind::EvictAdviseFailed;   // upstream: "unable to posix_fadvise file"
        r.err = errno;
    }
#elif defined(__FreeBSD__) || defined(__sun__) || defined(__APPLE__)
    if (msync(m.region.mem(), m.len_of_range, MS_INVALIDATE)) {
        r.evict_failed = true;
        r.evict_kind = Failure::Kind::EvictInvalidateFailed;   // upstream: "unable to msync invalidate file"
        r.err = errno;
    }
#else
    throw FatalError("cache eviction not (yet?) supported on this platform");
#endif

    r.pages_total = m.pages_in_range;
    r.region = std::move(m.region);
    return r;
}

FileResult lock(int fd, const Params& p, EngineEvents* ev) {
    Mapping m;
    if (FileResult r = prelude(fd, p, m); r.code != FileResult::Code::Ok)
        return r;

    int64_t pages_resident = 0;
    FileResult r = mincore_pass(m, /*touch=*/true, ev, pages_resident);   // -l implies touch
    if (r.code == FileResult::Code::Fatal)
        return r;

    if (mlock(m.region.mem(), m.len_of_range)) {
        r.code = FileResult::Code::Fatal;
        r.fatal = FileResult::FatalWhat::MlockFailed;     // session re-adds the path
        r.err = errno;
        return r;
    }

    r.pages_total = m.pages_in_range;
    r.pages_resident = pages_resident;
    r.region = std::move(m.region);   // caller keeps the locked mapping alive
    return r;
}

} // namespace cache
