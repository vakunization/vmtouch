// core/session.h — the façade that owns per-run state and drives the
// pipeline: crawl (producer) -> open -> verb dispatch.
//
// B3 interim state (the full process/Report rewrite is B4): the public
// surface is still the A3 trio — process(path)/process_fd/report() — but
// the Session is now the Crawler's VISITOR: it opens files via FileHandle
// and dispatches verbs; traversal failures arrive as CrawlItem.error.
//
// Ownership: the application owns the Session; the Session owns Stats,
// the kept regions (-l/-L mappings stay alive for process lifetime) and
// a Crawler per process() call (dedup works across roots within one call).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "cache.h"
#include "crawl.h"

class Session {
public:
    explicit Session(Options opts);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- public pipeline (A3 names; B4 replaces with process(span)/get_report) ----

    // Processes one path: files are opened + dispatched; directories are
    // crawled (fixed upstream policy — no -f/-F/-h; filters are gone).
    // Returns Skipped iff this call recorded new failures, Ok otherwise;
    // FatalError propagates (B4 converts at the boundary).
    Status process(const std::string& path);

    // S3 entry: the caller's ALREADY-OPEN fd. No open-by-path, no close —
    // the fd stays the caller's. offset/max_len are the -p range.
    Status process_fd(int fd, int64_t offset, int64_t max_len);

    // Totals snapshot: crawler counters + page stats, plain data.
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

    // ---- wiring surface (CLI/tests until B4/B5 move it) -------------------

    const Stats& stats() const { return stats_; }
    Stats& stats() { return stats_; }
    const Options& options() const { return opts_; }
    const FileInfoRange& range() const { return range_; }
    void set_range(const FileInfoRange& r) { range_ = r; }   // CLI applies -p here

private:
    Options opts_;
    Stats stats_;
    FileInfoRange range_;
    std::vector<Failure> failures_;
    std::vector<cache::MappedRegion> kept_regions_;   // -l/-L: alive for process lifetime
    int64_t total_files_ = 0;   // B3 interim counters (B4: per-call Report)
    int64_t total_dirs_ = 0;
    std::unique_ptr<vmt::Crawler> crawler_;   // B3 interim: session-lifetime (B4: per-call)

    // Per-file pipeline (the visitor body): open via FileHandle, verb
    // dispatch, result classification. Called by the crawler callback.
    Status process_file(const std::string& path);

    // Shared verb dispatch + classification for an ALREADY-OPEN fd.
    // Does not close the fd — the open-path owner (process_file) or the
    // external caller (process_fd) keeps ownership.
    Status run_verb_on_fd(int fd, const std::string& path, const FileInfoRange& range);
};
