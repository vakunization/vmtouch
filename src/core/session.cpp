// core/session.cpp — see session.h.
//
// B2 interim state (full rewrite lands in B4): the pipeline still runs
// through process()/process_file()/run_verb_on_fd(), but the verb layer
// now speaks the spec language — cache::* returns Result<VerbOutcome> and
// throws FatalError; there are NO sinks anymore (the -v sink wiring is
// removed; totals-only output arrives with the CLI rewrite in B5).
//
// Fatal handling (owner rulings): FatalError propagates for now; B4
// converts it to Status::Fatal + a Failure{Kind::Fatal} record at the
// public boundary.

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

Session::Session(Options opts)
    : opts_(std::move(opts)),
      crawler_(std::make_unique<Crawler>(*this)) {}

Session::~Session() = default;

// ---- A3 public surface ------------------------------------------------------

Status Session::process(const std::string& path) {
    // Contract (preserves the old process_file semantics at the A3 API):
    // Skipped iff this call recorded new failures (the path or something in
    // its subtree was skipped), Ok if everything processed. FatalError
    // propagates; the CLI exit code is unaffected either way (run.sh pins
    // "missing path exits 0").
    const size_t before = failures_.size();
    crawler_->walk(path);
    return failures_.size() > before ? Status::Skipped : Status::Ok;
}

Status Session::process_fd(int fd, int64_t offset, int64_t max_len) {
    // S3: the fd is the caller's — never closed here, never re-opened by
    // path. Range policy comes from the caller (offset/max_len); size
    // policy (-m) and the verb follow the session's options, exactly like
    // process_file().
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
    if (crawler_) {
        r.total_files = crawler_->total_files();
        r.total_dirs = crawler_->total_dirs();
    }
    r.stats = stats_;
    return r;
}

// ---- per-file pipeline -------------------------------------------------------

Status Session::process_file(const std::string& path) {
    // B1: the open/retry logic lives in FileHandle now (verbatim move);
    // the fd is owned by the handle and closed on every exit path below.
    auto opened = vmt::FileHandle::open(path);
    if (opened.is_error()) {
        fail(opened.error().kind, opened.error().err, path);
        return Status::Skipped;
    }

    Status st;
    try {
        st = run_verb_on_fd(opened.value().fd(), path, range_);
    } catch (...) {
        throw;
    }
    return st;
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
    //                     record with path context on failure — the
    //                     upstream warning keeps its text);
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

