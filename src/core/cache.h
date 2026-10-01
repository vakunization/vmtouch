// core/cache.h — page-cache operations on an open file descriptor.
//
// The pure engine of vmtouch: mmap -> mincore -> query/touch/evict/mlock.
// No printing, no exit(), no CLI state. All observable output goes through
// ProgressSink; hopeless situations throw FatalError; recoverable ones are
// reported via the warning callback and returned as Status::Skipped.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "common.h"

namespace cache {

// What to do with the mapped pages. Maps the CLI flags:
//   Query = neither -t nor -e nor -l (just report residency)
//   Touch = -t (and -l, which touches then locks)
//   Evict = -e
//   Lock  = -l (implies touch; mapping is intentionally NOT unmapped)
enum class Mode { Query, Touch, Evict, Lock };

// Per-file work item, assembled by the session (commit 6). Until then the
// CLI shim builds it directly. `path` is informational only (messages and
// sink calls): the fd is the only I/O identity, so there is no TOCTOU
// between the name and the object. Precondition: path outlives the call.
struct FileInfo {
    std::string_view path;
    int fd;
    int64_t offset;          // page-aligned start (from -p)
    int64_t max_len;         // 0 = to end of file
    size_t max_file_size;    // from -m; SIZE_MAX = unlimited (upstream default)
    bool verbose;            // gates the print-equivalent sink calls, as upstream gated on o.verbose
    bool lockall;            // from -L: suppress munmap exactly like upstream
};

// Every stdout side effect the monolith's vmtouch_file() had, as sink calls.
// Contract:
//  * calls happen synchronously on the thread processing the file;
//  * implementations must not throw;
//  * the CHART_UPDATE_INTERVAL (0.1 s) throttle stays INSIDE the engine, so
//    sinks fire at exactly the old print points;
//  * the CLI's sink renders on_chart via print_page_residency_chart, keeping
//    -v output byte-identical (tests/run.sh depends on it).
class ProgressSink {
public:
    virtual ~ProgressSink() = default;

    virtual void on_file_start(std::string_view path) = 0;   // was: printf("%s\n", path)
    virtual void on_evicting(std::string_view path) = 0;     // was: printf("Evicting %s\n", path)
    virtual void on_chart(std::span<const unsigned char> mincore_pages,
                          int64_t pages_in_file) = 0;        // was: print_page_residency_chart(...)
    // End of one verbose file block (was: the trailing printf("\\n")). The
    // CLI's ChartSink prints the newline here so -v output stays
    // byte-identical; embedders instead receive a per-file summary and
    // decide what to do with it.
    virtual void on_file_done(std::string_view path,
                              int64_t pages_in_file,
                              int64_t pages_resident) = 0;
};

// Warning seam until Session (commit 6) owns it: message text is passed
// verbatim; the CLI installs a printer honoring -q.
using WarnFn = std::function<void(std::string_view msg)>;

// Result of processing one file region.
enum class Status { Ok = 0, Skipped = 1 };   // Skipped: warning already emitted

// Process one file region according to mode.
//  * Lock mode intentionally skips munmap (upstream mlockall semantics);
//  * throws FatalError where continuing is pointless (mlock failure,
//    mincore failure, allocation failure, misaligned mmap);
//  * returns Skipped for the old warn+goto-bail cases (mmap failure etc.).
Status process_file(const FileInfo& fi, Mode mode, Stats& stats,
                    const WarnFn& warn, ProgressSink& sink);

} // namespace cache
