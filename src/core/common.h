// core/common.h — shared types for the vmtouch core library and CLI.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <string_view>

#include <sys/types.h>   // dev_t, ino_t

inline constexpr const char* VMTOUCH_VERSION = "1.3.1";

// Set once at startup by the CLI (platform::init()).
extern long pagesize;

// Per-file selection range (from -p). {0,0} = whole file.
struct FileInfoRange {
    int64_t offset = 0;
    int64_t max_len = 0;
};

// Identity of an already-seen file (hardlink dedup).
struct dev_and_inode {
    dev_t dev;
    ino_t ino;
};

// Library-level call policy. The CLI keeps all its own state separately.
struct Options {
    enum class Verb { Inspect, Warm, Evict, Lock };
    Verb verb = Verb::Inspect;
    FileInfoRange range{};               // {0,0} = whole file
    size_t max_file_size = SIZE_MAX;     // files larger are policy-skipped
    bool collect_failures = false;       // gate Report.failures (except Fatal)
};

// Result codes returned by core entry points (cache/session).
enum class Status : int32_t {
    Ok = 0,        // call completed; per-file problems are in the Report
    Skipped = 1,   // reserved (not produced)
    Fatal = 2,     // traversal interrupted; Report holds everything + the Fatal record
};

// Error that stops the whole traversal (allocation failure, misaligned
// mmap, unsupported platform). Caught at the Session boundary; the CLI
// prints "vmtouch: FATAL: <msg>" and exits 1. The library never exits.
class FatalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One recorded failure. The library reports facts; the CLI formats text.
// Kind values are frozen — the Phase-3 C enum maps 1:1. Additions are
// allowed; renumbering is not.
struct Failure {
    enum class Kind : int32_t {
        Open = 1,             // unable to open
        Stat = 2,             // unable to stat (crawl lstat/stat)
        Map = 3,              // unable to mmap
        TooLarge = 4,         // file exceeds -m
        BeyondEof = 5,        // -p offset beyond EOF
        NotRegular = 6,       // skipping non-regular file
        Symlink = 7,          // not following symbolic link
        SeparateFs = 8,       // -F: not recursing into other filesystem
        CrawlLoop = 9,        // symbolic link loop detected
        PathTooLong = 10,     // path exceeds PATH_MAX
        MincoreInaccurate = 11, // RESERVED (kernel >= 5.2 heuristic) — not produced
        EvictFailed = 12,     // legacy umbrella — not produced (18/19 supersede)
        Closedir = 13,        // unable to closedir
        FstatFailed = 14,     // unable to fstat PATH (ERR), skipping
        IoctlSizeFailed = 15, // unable to ioctl PATH (ERR), skipping
        OpendirFailed = 16,   // unable to opendir PATH (ERR), skipping
        MunmapFailed = 17,    // unable to munmap file PATH (ERR)
        EvictAdviseFailed = 18,     // unable to posix_fadvise file PATH (ERR)
        EvictInvalidateFailed = 19, // unable to msync invalidate file PATH (ERR)
        Fatal = 20,           // traversal stopped; the message rides in `path`
        MlockFailed = 21,     // mlock failed (per-file Failed, traversal continues)
        MincoreFailed = 22,   // mincore failed (per-file Failed)
    };
    Kind        kind;
    int32_t     err = 0;    // errno, 0 if not applicable
    std::string path;       // empty for fd-based ops; the message text for Fatal
    int64_t     offset = 0; // for range problems (BeyondEof)
};

struct Stats {
    int64_t total_pages = 0;
    int64_t total_pages_in_core = 0;
};
