// core/common.h — shared types for the vmtouch core library and CLI.
//
// Introduced by commit 2 ("common-types") as a pure move from the monolith:
// the structs are byte-for-byte the ones that used to live at the top of
// src/vmtouch.cpp. Status and FatalError are defined here from the start so
// commit 3 (diagnostics) can use them; nothing in the C-transplanted code
// references them yet.

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string_view>

#include <sys/types.h>   // dev_t, ino_t

inline constexpr const char* VMTOUCH_VERSION = "1.3.1";

// Set once at startup by the CLI (platform::init() in commit 3).
extern long pagesize;

/*
 * To find out if the stat results from a single file correspond to a file we
 * have already seen, we need to compare both the device and the inode
 */
struct dev_and_inode {
    dev_t dev;
    ino_t ino;
};

struct Options {
    bool touch = 0;
    bool evict = 0;
    bool quiet = 0;
    int verbose = 0;
    bool lock = 0;
    bool lockall = 0;
    bool daemon = 0;
    bool followsymlinks = 0;
    bool singlefilesystem = 0;
    bool ignorehardlinkeduplictes = 0;   // spelling preserved from upstream
    size_t max_file_size = SIZE_MAX;
    bool wait = 0;
    const char* batch = nullptr;
    const char* pidfile = nullptr;
    const char* output = nullptr;
    int zero_delim = 0;
};

// Per-file selection range (from -p). Was "struct FileInfo" in the monolith;
// its `char* path` member was dead (never read or written) and is dropped.
struct FileInfoRange {
    int64_t offset = 0;
    int64_t max_len = 0;
};

// Result codes returned by core entry points (cache/session).
enum class Status {
    Ok = 0,        // file/directory processed
    Skipped = 1,   // not processed; a warning was already emitted (or silence is correct)
};

// Fatal operation error: thrown where continuing is pointless (mlock
// failure, allocation failure, ...). The CLI's single top-level handler
// prints "vmtouch: FATAL: <msg>" and exits 1 — the library itself never
// exits (defined in commit 3, used by core from then on).
class FatalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// LEGACY (removed in A2): the pre-A1 string-warning type.
// A1 kept it only so the Session constructor signature compiled during the
// transition; A2 removed the constructor parameter and this alias.

// ---------------------------------------------------------------------------
// Structured failures (A1): the library reports FACTS, not strings. The CLI
// formats them at print time; embedders (S1: "error codes I can log") read
// them directly. C-shape note: Kind values are explicit and frozen — the
// Phase-3 C enum maps 1:1.
// ---------------------------------------------------------------------------
struct Failure {
    enum class Kind : int {
        Open = 1,             // unable to open
        Stat = 2,             // unable to stat / fstat / ioctl size
        Map = 3,              // unable to mmap / munmap
        TooLarge = 4,         // file exceeds -m
        BeyondEof = 5,        // -p offset beyond EOF
        NotRegular = 6,       // skipping non-regular file
        Symlink = 7,          // not following symbolic link
        SeparateFs = 8,       // -F: not recursing into other filesystem
        CrawlLoop = 9,        // symbolic link loop detected
        PathTooLong = 10,     // path exceeds PATH_MAX
        MincoreInaccurate = 11, // kernel >= 5.2 write-permission heuristic
        EvictFailed = 12,     // fadvise/msync eviction failure
        Closedir = 13,        // unable to closedir
    };
    Kind        kind;
    int         err = 0;    // errno, 0 if not applicable
    std::string path;       // empty for fd-based ops
    int64_t     offset = 0; // for range problems (BeyondEof)
};

struct Stats {
    int64_t total_pages = 0;
    int64_t total_pages_in_core = 0;
};
