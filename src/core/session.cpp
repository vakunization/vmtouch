// core/session.cpp — see session.h.
//
// B3: the Session is the Crawler's VISITOR. process() creates a Crawler
// per call and walks the given root; the visitor body opens files via
// FileHandle and dispatches verbs; traversal failures arrive as
// CrawlItem.error and become Failure records here.
//
// Fatal handling: FatalError propagates for now; B4 converts it to
// Status::Fatal + a Failure{Kind::Fatal} record at the public boundary.

#include "session.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "file_handle.h"
#include "platform.h"

namespace {

// A5: the verb vocabulary is cache::Verb (docs/vocabulary.md); the
// Options->Verb mapping lives here (session policy).
cache::Verb verb_from_options(const Options& o) {
    if (o.evict) return cache::Verb::Evict;
    if (o.lock)  return cache::Verb::Lock;
    if (o.touch) return cache::Verb::Warm;
    return cache::Verb::Inspect;
}

} // namespace

Session::Session(Options opts) : opts_(std::move(opts)) {}

Session::~Session() = default;

// ---- public surface ---------------------------------------------------------

Status Session::process(const std::string& path) {
    // B3 interim: ONE Crawler for the Session's lifetime so the CLI's
    // per-argv process() loop keeps cross-root dedup (run.sh pins "hl hl2
    // -> Files: 1"). B4 restores per-call state when the span-taking
    // process(paths) lands (one call, all roots, one Report).
    if (!crawler_)
        crawler_ = std::make_unique<vmt::Crawler>();

    const size_t failures_before = failures_.size();

    auto visitor = [&](const vmt::CrawlItem& item) {
        if (item.error != nullptr) {
            // Traversal-level failure (stat/symlink/not-regular/loop/...):
            // recorded, never thrown (spec2 §5.10).
            fail(item.error->kind, item.error->err, std::string(item.path), item.error->offset);
            return;
        }

        ++total_files_;
        (void)process_file(std::string(item.path));
    };

    crawler_->walk(path, visitor);
    total_dirs_ += crawler_->dirs_seen();

    return failures_.size() > failures_before ? Status::Skipped : Status::Ok;
}

Status Session::process_fd(int fd, int64_t offset, int64_t max_len) {
    // S3: the fd is the caller's — never closed here, never re-opened by
    // path. Range policy comes from the caller (offset/max_len); size
    // policy (-m) and the verb follow the session's options.
    if (fd < 0) {
        fail(Failure::Kind::Open, EBADF, "", offset);
        return Status::Skipped;
    }
    FileInfoRange range;
    range.offset = offset;
    range.max_len = max_len;
    return run_verb_on_fd(fd, "", range);   // no path context: "" for records
}

Session::Report Session::report() const {
    Report r;
    r.total_files = total_files_;
    r.total_dirs = total_dirs_;
    r.stats = stats_;
    return r;
}

// ---- per-file pipeline -------------------------------------------------------

Status Session::process_file(const std::string& path) {
    // B1: the open/retry logic lives in FileHandle; the fd is owned by
    // the handle and closed on every exit path below.
    auto opened = vmt::FileHandle::open(path);
    if (opened.is_error()) {
        fail(opened.error().kind, opened.error().err, path);
        return Status::Skipped;
    }

    return run_verb_on_fd(opened.value().fd(), path, range_);
}

Status Session::run_verb_on_fd(int fd, const std::string& path, const FileInfoRange& range) {
    // Shared verb dispatch + classification: used by process_file() (fd
    // opened above) and process_fd() (caller-owned fd, S3).
    //
    // B2 classification of cache::* outcomes:
    //   Error(Failure) -> the SAME record with the path attached, then
    //                     Status::Skipped;
    //   Ok(VerbOutcome)-> stats absorbed; region kept iff lock/lockall,
    //                     else the session munmaps it here (MunmapFailed
    //                     record with path context on failure);
    //   FatalError     -> propagates (B4 converts at the public boundary).

    cache::Params params;
    params.range = range;
    params.max_file_size = opts_.max_file_size;

    const cache::Verb verb = verb_from_options(opts_);

    vmt::Result<cache::VerbOutcome> r = [&] {
        switch (verb) {
            case cache::Verb::Inspect: return cache::inspect(fd, params);
            case cache::Verb::Warm:    return cache::warm(fd, params);
            case cache::Verb::Evict:   return cache::evict(fd, params);
            case cache::Verb::Lock:    return cache::lock(fd, params);
        }
        throw FatalError("unreachable verb");
    }();

    if (r.is_error()) {
        // Attach the path (the verb is path-free) and record.
        Failure f = r.error();
        f.path = path;
        fail(f.kind, f.err, f.path, f.offset);
        return Status::Skipped;
    }

    // Ok: absorb the stats; keep or drop the region.
    stats_.total_pages += r.value().stats.total_pages;
    stats_.total_pages_in_core += r.value().stats.total_pages_in_core;

    if (opts_.lock || opts_.lockall) {
        kept_regions_.push_back(std::move(r.value().region));   // stays mapped
    } else if (r.value().region) {
        // The verb hands inspect/warm/evict mappings back; the session
        // munmaps (path context available for the failure record).
        int munmap_err = 0;
        if (!r.value().region.unmap(munmap_err))
            fail(Failure::Kind::MunmapFailed, munmap_err, path);
    }

    return Status::Ok;
}
