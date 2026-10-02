// core/session.h — the façade that owns per-run state and drives the
// per-file pipeline: open -> checks -> cache verb dispatch.
//
// Ownership (agreed): the application owns the Session; the Session owns
// Stats, FileFilter, the ProgressSink and the kept regions (A2: lock/lockall
// mappings stay alive for process lifetime). Crawler (commit 7) holds only
// references and must not outlive the Session.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "cache.h"
#include "filters.h"

class Session {
public:
    // sink may be null (no verbose output).
    // A2: the legacy WarnFn parameter is gone — failures are recorded as
    // structured data and read via failures().
    Session(Options opts, FileFilter filter,
            std::unique_ptr<cache::ProgressSink> sink);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Processes one path (regular file or block device; directories are the
    // crawler's job): open with O_NOATIME + EPERM fallback, EMFILE/ENFILE
    // retry via platform::raise_nofile_rlimit, then the cache verb. The
    // verb's returned data is classified HERE (A2): stats absorbed, failures
    // recorded, fatal facts re-thrown with the path, kept regions stored.
    Status process_file(const std::string& path);

    // Structured failure recording (A1): crawl/cache report facts; Session
    // appends them to its failure list. The quiet policy is applied by the
    // CLI at print time — the library keeps ALL facts regardless of -q, so
    // embedders get complete reports even in quiet runs.
    void fail(Failure::Kind kind, int err, const std::string& path, int64_t offset = 0) {
        failures_.push_back(Failure{kind, err, path, offset});
    }
    const std::vector<Failure>& failures() const { return failures_; }

    const Stats& stats() const { return stats_; }
    Stats& stats() { return stats_; }
    const Options& options() const { return opts_; }
    FileFilter& filter() { return filter_; }
    cache::ProgressSink* sink() const { return sink_.get(); }
    const FileInfoRange& range() const { return range_; }
    void set_range(const FileInfoRange& r) { range_ = r; }   // CLI applies -p here

private:
    Options opts_;
    std::unique_ptr<cache::ProgressSink> sink_;
    FileFilter filter_;
    Stats stats_;
    FileInfoRange range_;
    std::vector<Failure> failures_;
    std::vector<cache::MappedRegion> kept_regions_;   // -l/-L: alive for process lifetime

    // A2: bridge between the engine's path-less EngineEvents and the
    // caller-facing ProgressSink — binds the path of the file currently
    // being processed. Defined in session.cpp.
    struct PathBoundEvents;

    // A2: translate a verb's fatal fact back into the historical message
    // (the verb lost the path; the session re-adds it).
    [[noreturn]] void throw_fatal(const cache::Result& r, const std::string& path);
};
