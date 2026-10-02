// core/session.h — the façade that owns per-run state and drives the
// per-file pipeline: open -> checks -> cache engine.
//
// Ownership (agreed): the application owns the Session; the Session owns
// Stats, FileFilter, the ProgressSink and the warning seam. Crawler (commit
// 7) holds only references and must not outlive the Session.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "cache.h"
#include "filters.h"

class Session {
public:
    // sink may be null (no verbose output); warn may be null (warnings
    // dropped — the CLI installs a printer honoring -q instead).
    Session(Options opts, std::unique_ptr<cache::ProgressSink> sink, WarnFn warn);
    // Same, with a pre-populated FileFilter (ownership moves into Session).
    Session(Options opts, FileFilter filter, std::unique_ptr<cache::ProgressSink> sink, WarnFn warn);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Processes one path (regular file or block device; directories are the
    // crawler's job): open with O_NOATIME + EPERM fallback, EMFILE/ENFILE
    // retry via platform::raise_nofile_rlimit, fstat, size checks, then
    // cache::process_file. Returns the engine's Status.
    Status process_file(const std::string& path);

    // Structured failure recording (A1): crawl/cache report facts; Session
    // appends them to its failure list. The quiet policy is applied by the
    // CLI at print time — the library keeps ALL facts regardless of -q, so
    // embedders get complete reports even in quiet runs.
    void fail(Failure::Kind kind, int err, const std::string& path, int64_t offset = 0) {
        failures_.push_back(Failure{kind, err, path, offset});
    }
    // cache::FailureSink adapter: engine reports kinds without paths (the
    // path travels in FileInfo; session attaches the current file's path).
    void fail_for(const std::string& path, Failure::Kind kind, int err, int64_t offset = 0) {
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
    WarnFn warn_;
    FileFilter filter_;
    Stats stats_;
    FileInfoRange range_;
    std::vector<Failure> failures_;

    // cache::FailureSink adapter bound to the path being processed.
    struct CacheFailureSink final : public cache::FailureSink {
        Session* self;
        std::string path;
        void add(Failure::Kind kind, int err, int64_t offset = 0) override {
            self->fail(kind, err, path, offset);
        }
    } cache_sink_adapter_;
};
