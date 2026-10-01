// core/session.h — the façade that owns per-run state and drives the
// per-file pipeline: open -> checks -> cache engine.
//
// Ownership (agreed): the application owns the Session; the Session owns
// Stats, FileFilter, the ProgressSink and the warning seam. Crawler (commit
// 7) holds only references and must not outlive the Session.

#pragma once

#include <memory>
#include <string>

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

    // True warning seam: quiet check lives here (owner of the Options).
    void warning(std::string_view msg) const {
        if (!opts_.quiet && warn_) warn_(msg);
    }

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
};
