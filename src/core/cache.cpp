// core/cache.cpp — see cache.h.
//
// The bodies are transplanted verbatim from the monolith's vmtouch_file()
// (commit 5). Deliberate deviations, each required by the seam:
//   * printf sites -> ProgressSink calls (byte-identical when rendered by
//     the CLI's ChartSink);
//   * warning() -> the WarnFn callback (quiet handling stays with the
//     installer of the callback);
//   * fatal() -> throw FatalError;
//   * the msync eviction branch passes `path` correctly to the warning —
//     upstream passed the whole Options struct (compile error on the
//     FreeBSD/Sun/Apple branch); flagged in the commit message.
// The session (commit 6) owns open/fstat/checks; this engine works purely
// on the fd and never closes it.

#include "cache.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
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

} // namespace

Status process_file(const FileInfo& fi, Mode mode, Stats& stats,
                    const WarnFn& warn, ProgressSink& sink) {
    void* mem = NULL;
    int64_t len_of_file = 0;
    int64_t len_of_range = 0;
    int64_t pages_in_range = 0;
    const int fd = fi.fd;
    const std::string_view path = fi.path;

    auto do_warn = [&](const std::string& msg) {
        if (warn) warn(msg);
    };

    // ---- size of the object (fd is the identity; fstat here mirrors what
    //      the monolith did right before mmap) ----------------------------
    struct stat sb;
    if (fstat(fd, &sb)) {
        do_warn("unable to fstat " + std::string(path) + " (" + strerror(errno) + "), skipping");
        return Status::Skipped;
    }

    if (S_ISBLK(sb.st_mode)) {
#if defined(__linux__)
        if (ioctl(fd, BLKGETSIZE64, &len_of_file)) {
            do_warn("unable to ioctl " + std::string(path) + " (" + strerror(errno) + "), skipping");
            return Status::Skipped;
        }
#else
        throw FatalError("discovering size of block devices not (yet?) supported on this platform");
#endif
    } else {
        len_of_file = sb.st_size;
    }

    if (len_of_file == 0) {
        return Status::Skipped;   // empty: silent skip, as upstream
    }

    if (static_cast<uint64_t>(len_of_file) > fi.max_file_size) {
        do_warn("file " + std::string(path) + " too large, skipping");
        return Status::Skipped;
    }

    // ---- range selection (verbatim) --------------------------------------

    if (fi.max_len > 0 && (fi.offset + fi.max_len) < len_of_file) {
        len_of_range = fi.max_len;
    } else if (fi.offset >= len_of_file) {
        do_warn("file " + std::string(path) + " smaller than offset, skipping");
        return Status::Skipped;
    } else {
        len_of_range = len_of_file - fi.offset;
    }

    // ---- map --------------------------------------------------------------

    mem = mmap(NULL, len_of_range, PROT_READ, MAP_SHARED, fd, fi.offset);

    if (mem == MAP_FAILED) {
        do_warn("unable to mmap file " + std::string(path) + " (" + strerror(errno) + "), skipping");
        return Status::Skipped;
    }

    if (!platform::aligned_p(mem)) {
        throw FatalError("mmap(" + std::string(path) + ") wasn't page aligned");
    }

    pages_in_range = bytes2pages(len_of_range);

    stats.total_pages += pages_in_range;

    // ---- act (verbatim, prints -> sink) -----------------------------------

    if (mode == Mode::Evict) {
        if (fi.verbose) sink.on_evicting(path);

#if defined(__linux__) || defined(__hpux)
        if (posix_fadvise(fd, fi.offset, len_of_range, POSIX_FADV_DONTNEED))
            do_warn("unable to posix_fadvise file " + std::string(path) + " (" + strerror(errno) + ")");
#elif defined(__FreeBSD__) || defined(__sun__) || defined(__APPLE__)
        if (msync(mem, len_of_range, MS_INVALIDATE))
            do_warn("unable to msync invalidate file " + std::string(path) + " (" + strerror(errno) + ")");
#else
        throw FatalError("cache eviction not (yet?) supported on this platform");
#endif
    } else {
        double last_chart_print_time = 0.0, temp_time;
        auto* mincore_array = static_cast<unsigned char*>(malloc(pages_in_range));
        if (mincore_array == NULL)
            throw FatalError(std::string("Failed to allocate memory for mincore array (") + strerror(errno) + ")");

        // 3rd arg to mincore is char* on BSD and unsigned char* on linux
        if (mincore(mem, len_of_range, mincore_array))
            throw FatalError("mincore " + std::string(path) + " (" + strerror(errno) + ")");

        int64_t pages_resident = 0;
        for (int64_t i = 0; i < pages_in_range; i++) {
            if (platform::is_mincore_page_resident(mincore_array[i])) {
                stats.total_pages_in_core++;
                pages_resident++;
            }
        }

        if (fi.verbose) {
            sink.on_file_start(path);
#ifdef __linux__
            if (!platform::can_do_mincore(sb)) {
                do_warn("Process does not have write permission, residency chart will not be accurate");
            }
#endif
            last_chart_print_time = platform::now_seconds();
            sink.on_chart(std::span<const unsigned char>(mincore_array, pages_in_range), pages_in_range);
        }

        if (mode == Mode::Touch || mode == Mode::Lock) {
            volatile uint64_t j = 0;
            for (int64_t i = 0; i < pages_in_range; i++) {
                j += (static_cast<char*>(mem))[i * pagesize];
                mincore_array[i] = 1;

                if (fi.verbose) {
                    temp_time = platform::now_seconds();

                    if (temp_time > (last_chart_print_time + kChartUpdateInterval)) {
                        last_chart_print_time = temp_time;
                        sink.on_chart(std::span<const unsigned char>(mincore_array, pages_in_range), pages_in_range);
                    }
                }
            }

            pages_resident = pages_in_range;
        }

        if (fi.verbose) {
            // was: final print_page_residency_chart(stdout, ...) + printf("\\n")
            // ending the verbose block. on_file_done replaces the trailing
            // newline; the CLI's ChartSink emits "\\n" inside on_file_done.
            sink.on_chart(std::span<const unsigned char>(mincore_array, pages_in_range), pages_in_range);
            sink.on_file_done(path, pages_in_range, pages_resident);
        }

        free(mincore_array);
    }

    if (mode == Mode::Lock) {
        if (mlock(mem, len_of_range))
            throw FatalError("mlock: " + std::string(path) + " (" + strerror(errno) + ")");
    }

    // ---- bail (verbatim; Lock/lockall keep the mapping alive) -------------

    if (mode != Mode::Lock && !fi.lockall && mem) {
        if (munmap(mem, len_of_range))
            do_warn("unable to munmap file " + std::string(path) + " (" + strerror(errno) + ")");
    }

    return Status::Ok;
}

} // namespace cache
