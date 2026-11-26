// core/common.h — shared types for the vmtouch core library and CLI.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

// Per-file selection. Both values are non-negative; offset is page-aligned.
// max_len == 0 means to EOF, so {0,0} selects the whole file.
struct FileInfoRange {
  int64_t offset = 0;
  int64_t max_len = 0;
};

// Library-level call policy. The CLI keeps all its own state separately.
struct Options {
  enum class Verb { Inspect, Warm, Evict, Lock };
  Verb verb = Verb::Inspect;
  FileInfoRange range{};             // {0,0} = whole file
  size_t max_file_size = SIZE_MAX;   // files larger are policy-skipped
  bool collect_failures = false;     // gate Report.failures (except Fatal)
  bool retain_warm_mappings = false; // Warm only; keep mappings until Session destruction
};

// Result codes returned by Session entry points.
enum class Status : int32_t {
  Ok = 0,    // call completed; per-file problems are in the Report
  Fatal = 1, // traversal interrupted; Report holds everything + the Fatal record
};

// Infrastructure failure that stops a run (e.g. misaligned mmap, unsupported
// platform, or traversal depth limit). Session converts this to Status::Fatal.
// Other exceptions, including std::bad_alloc, can propagate to C++ callers.
class FatalError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// One recorded failure. The library reports facts; the CLI formats text.
// Kind values are frozen — the Phase-3 C enum maps 1:1. Additions are
// allowed; renumbering is not. The gaps left by removed kinds are intentional.
struct Failure {
  enum class Kind : int32_t {
    Open = 1,       // unable to open
    Stat = 2,       // unable to stat (crawl lstat/stat)
    Map = 3,        // invalid mapping range or unable to mmap
    TooLarge = 4,   // file exceeds -m
    BeyondEof = 5,  // -p offset beyond EOF
    NotRegular = 6, // skipping non-regular file
    Symlink = 7,    // not following symbolic link
    // 8 was SeparateFs (reserved, no producer)   — removed
    CrawlLoop = 9, // detected a repeated directory on the active walk path
    // 10 was PathTooLong (reserved, no producer) — removed
    // 11 was MincoreInaccurate (no producer)     — removed
    // 12 was EvictFailed (superseded by 18/19)   — removed
    Closedir = 13,              // unable to closedir
    FstatFailed = 14,           // unable to fstat PATH (ERR), skipping
    IoctlSizeFailed = 15,       // unable to ioctl PATH (ERR), skipping
    OpendirFailed = 16,         // unable to opendir PATH (ERR), skipping
    MunmapFailed = 17,          // unable to munmap file PATH (ERR)
    EvictAdviseFailed = 18,     // unable to posix_fadvise file PATH (ERR)
    EvictInvalidateFailed = 19, // unable to msync invalidate file PATH (ERR)
    Fatal = 20,                 // traversal stopped; the message rides in `path`
    MlockFailed = 21,           // mlock failed (per-file Failed, traversal continues)
    MincoreFailed = 22,         // mincore failed (per-file Failed)
  };
  Kind kind;
  int32_t err = 0;    // errno, 0 if not applicable
  std::string path;   // empty for fd-based ops; the message text for Fatal
  int64_t offset = 0; // for range problems (BeyondEof)
};

struct Stats {
  int64_t total_pages = 0;
  int64_t total_pages_in_core = 0;
};
