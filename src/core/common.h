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

// Warning seam (the "Diagnostics folded into Session" decision): the
// library never prints to stderr; the installer decides where warnings go.
// Quiet (-q) handling lives with the owner of the options — Session::warning
// checks it before invoking the callback.
using WarnFn = std::function<void(std::string_view msg)>;

struct Stats {
    int64_t total_pages = 0;
    int64_t total_pages_in_core = 0;
};
