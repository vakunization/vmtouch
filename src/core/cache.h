// core/cache.h — page-cache verbs on an open file descriptor.
//
// A2: the engine sheds `path` and `verbose` (they were FileInfo members).
// It now exposes four internal verbs — inspect / warm / evict / lock —
// working purely on the fd, and RETURNS DATA: the session decides what to
// collect (stats, failures) and owns the mapping lifetime. Names here are
// provisional; A5 formalizes the Verb/FileResult/Report vocabulary.
//
// Rules that survive from the Increment-1 engine:
//   * no printing, no exit(), no CLI state, and now no paths;
//   * hopeless situations either come back as fatal facts in the Result
//     (when the historical message contained the path — the session
//     re-adds it) or throw FatalError (already path-less text);
//   * recoverable skips come back as data (Kind + errno), never strings.

#pragma once

#include <cstdint>
#include <span>
#include <stddef.h>

#include <sys/mman.h>    // munmap

#include "common.h"

namespace cache {

// Caller policy per call (was FileInfo minus path/verbose).
struct Params {
    FileInfoRange range;                    // -p: offset / max_len (0 = to EOF)
    size_t max_file_size = SIZE_MAX;        // -m (upstream default: unlimited)
};

// ---------------------------------------------------------------------------
// MappedRegion: owning handle for a live mmap region. The verbs return it
// instead of munmapping — the session decides keep-vs-drop (matches -l/-L
// and the evict+lockall combination, which upstream kept mapped too).
// Move-only; destructor munmaps. (The P-phase "vector+RAII MappedRegion"
// modernization builds on this name.)
// ---------------------------------------------------------------------------
class MappedRegion {
public:
    MappedRegion() = default;
    MappedRegion(void* mem, int64_t len) : mem_(mem), len_(len) {}
    ~MappedRegion() {
        if (mem_)
            munmap(mem_, len_);
    }

    MappedRegion(MappedRegion&& other) noexcept : mem_(other.mem_), len_(other.len_) {
        other.mem_ = nullptr;
        other.len_ = 0;
    }
    MappedRegion& operator=(MappedRegion&& other) noexcept {
        if (this != &other) {
            if (mem_)
                munmap(mem_, len_);
            mem_ = other.mem_;
            len_ = other.len_;
            other.mem_ = nullptr;
            other.len_ = 0;
        }
        return *this;
    }

    MappedRegion(const MappedRegion&) = delete;
    MappedRegion& operator=(const MappedRegion&) = delete;

    void* mem() const { return mem_; }
    int64_t len() const { return len_; }
    explicit operator bool() const { return mem_ != nullptr; }

private:
    void* mem_ = nullptr;
    int64_t len_ = 0;
};

// Verb result: PLAIN DATA. Internal shape until A5 formalizes FileResult.
struct Result {
    enum class Code : int { Ok = 0, Skipped = 1, Fatal = 2 };
    Code code = Code::Ok;

    // --- Skipped: the session turns this into a Failure record ----------
    bool silent = false;                        // empty file: skip, NO record
    Failure::Kind skip = Failure::Kind::Stat;   // which record (when !silent)
    int err = 0;                                // errno
    int64_t offset = 0;                         // BeyondEof

    // --- Fatal: the session classifies and re-adds path context ---------
    enum class FatalWhat : int {
        None = 0,
        MlockFailed,        // err = errno; msg "mlock: PATH (ERR)"
        MincoreFailed,      // err = errno; msg "mincore PATH (ERR)"
        MisalignedMap,      // msg "mmap(PATH) wasn't page aligned"
    };
    FatalWhat fatal = FatalWhat::None;

    // --- Facts collected: the session decides what to keep --------------
    int64_t pages_total = 0;                    // was stats.total_pages +=
    int64_t pages_resident = 0;                 // was stats.total_pages_in_core +=
    bool evict_failed = false;                  // fadvise/msync failed (still Ok)
    bool mincore_inaccurate = false;            // kernel>=5.2 heuristic

    MappedRegion region;                        // live mapping (may be empty)
};

// ---------------------------------------------------------------------------
// EngineEvents: live-progress events for the verbs, PATH-LESS (the engine
// no longer knows a path). The session binds the current path via an
// adapter and forwards to the caller-facing ProgressSink below. Interim
// interface — A6 (final-state-only -v) removes it together with the chart
// throttling.
//
// Contract (unchanged from ProgressSink):
//   * calls happen synchronously on the thread processing the file;
//   * implementations must not throw;
//   * the CHART_UPDATE_INTERVAL (0.1 s) throttle stays INSIDE the engine,
//     so events fire at exactly the old print points (run.sh parses them).
//   * nullptr means non-verbose: no events AND no throttle clock reads.
// ---------------------------------------------------------------------------
class EngineEvents {
public:
    virtual ~EngineEvents() = default;

    virtual void on_file_start() = 0;                       // was printf("%s\n", path)
    virtual void on_evicting() = 0;                         // was printf("Evicting %s\n", path)
    virtual void on_chart(std::span<const unsigned char> mincore_pages,
                          int64_t pages_in_file) = 0;       // was print_page_residency_chart(...)
    virtual void on_file_done(int64_t pages_in_file,
                              int64_t pages_resident) = 0;  // ended the verbose block
};

// ---------------------------------------------------------------------------
// Caller-facing progress sink (UNCHANGED from Increment 1 — the CLI's
// ChartSink and the embedder docs keep this signature). The engine never
// sees it directly anymore; the session's adapter bridges EngineEvents to
// it, attaching the path of the file being processed.
// ---------------------------------------------------------------------------
class ProgressSink {
public:
    virtual ~ProgressSink() = default;

    virtual void on_file_start(std::string_view path) = 0;
    virtual void on_evicting(std::string_view path) = 0;
    virtual void on_chart(std::span<const unsigned char> mincore_pages,
                          int64_t pages_in_file) = 0;
    virtual void on_file_done(std::string_view path,
                              int64_t pages_in_file,
                              int64_t pages_resident) = 0;
};

// The four internal verbs. fd is the only I/O identity. All share the same
// prelude (fstat -> block-device size -> empty -> -m -> range -> mmap ->
// alignment). Hopeless platform situations still throw path-less
// FatalError (allocation failure, block-device size on non-Linux,
// unsupported eviction platform).
Result inspect(int fd, const Params& p, EngineEvents* ev);   // query residency
Result warm(int fd, const Params& p, EngineEvents* ev);      // -t: touch pages
Result evict(int fd, const Params& p, EngineEvents* ev);     // -e
Result lock(int fd, const Params& p, EngineEvents* ev);      // -l: mlock + region

} // namespace cache
