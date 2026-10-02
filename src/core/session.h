// core/session.h — the façade that owns per-run state and drives the
// per-file pipeline: open -> checks -> cache verb dispatch, plus the
// internal filesystem traversal (A3: Session absorbs Crawler).
//
// Public surface (A3, per the approved plan):
//   process(path)            — one path; directories are traversed
//   process_fd(fd, off, len) — caller-owned fd (S3: no re-open by path)
//   report()                 — totals (files/dirs) + stats snapshot
//   failures()               — structured failure records (A1)
//
// Ownership: the application owns the Session; the Session owns Stats,
// the ProgressSink, the kept regions (-l/-L mappings stay alive for
// process lifetime) and the traversal state (A3). Filter policy lives on
// Options (A4) — the Session holds no filter state.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "cache.h"
#include "crawl.h"

class Session {
public:
    // sink may be null (no verbose output).
    // A4: filter policy travels inside Options — no separate filter arg.
    Session(Options opts, std::unique_ptr<cache::ProgressSink> sink);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- public pipeline (A3 names) --------------------------------------

    // Processes one path: regular file or block device via the per-file
    // pipeline; directories via the internal crawler. Symlinks/filters/
    // dedup semantics are the crawler's (unchanged, pinned by tests).
    Status process(const std::string& path);

    // S3 entry: the caller's ALREADY-OPEN fd. No open-by-path, no close —
    // the fd stays the caller's. offset/max_len are the -p range.
    Status process_fd(int fd, int64_t offset, int64_t max_len);

    // Totals snapshot: crawler counters + page stats, plain data (A5 will
    // formalize the Report vocabulary; today it is a small aggregate).
    struct Report {
        int64_t total_files = 0;
        int64_t total_dirs = 0;
        Stats stats;
    };
    Report report() const;

    // ---- structured failures (A1, unchanged) ------------------------------

    void fail(Failure::Kind kind, int err, const std::string& path, int64_t offset = 0) {
        failures_.push_back(Failure{kind, err, path, offset});
    }
    const std::vector<Failure>& failures() const { return failures_; }

    // ---- wiring surface (CLI/tests until A4/A5 move it) -------------------

    const Stats& stats() const { return stats_; }
    Stats& stats() { return stats_; }
    const Options& options() const { return opts_; }
    Options& options() { return opts_; }   // A4: tests/CLI mutate filter lists pre-walk
    cache::ProgressSink* sink() const { return sink_.get(); }
    const FileInfoRange& range() const { return range_; }
    void set_range(const FileInfoRange& r) { range_ = r; }   // CLI applies -p here

private:
    Options opts_;
    std::unique_ptr<cache::ProgressSink> sink_;
    Stats stats_;
    FileInfoRange range_;
    std::vector<Failure> failures_;
    std::vector<cache::MappedRegion> kept_regions_;   // -l/-L: alive for process lifetime

    // A3: the crawler is INTERNAL — owned here, never exposed. It reports
    // per-file work back through this session (walk() below) and its
    // totals flow into report().
    std::unique_ptr<Crawler> crawler_;

    // Per-file pipeline (A2 shape, renamed from the public process_file):
    // open with O_NOATIME + EPERM fallback, EMFILE/ENFILE retry, verb
    // dispatch, result classification. Called by process() for regular
    // files and (before A3) directly by tests; internal from now on.
    Status process_file(const std::string& path);

    // A2: bridge between the engine's path-less EngineEvents and the
    // caller-facing ProgressSink — binds the path of the file currently
    // being processed. Defined in session.cpp.
    struct PathBoundEvents;

    // A2: translate a verb's fatal fact back into the historical message
    // (the verb lost the path; the session re-adds it).
    [[noreturn]] void throw_fatal(const cache::Result& r, const std::string& path);

    // A3: shared verb dispatch + classification for an ALREADY-OPEN fd.
    // Does not close the fd — the open-path owner (process_file) or the
    // external caller (process_fd) keeps ownership.
    Status run_verb_on_fd(int fd, const std::string& path, const FileInfoRange& range);

    // Crawler access to the per-file pipeline (friendship keeps process_file
    // private; the crawler is the only caller besides process() itself).
    friend class Crawler;
};
