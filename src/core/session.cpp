// core/session.cpp — see session.h.
//
// A3: the public surface is process()/process_fd()/report(). The per-file
// pipeline (open with O_NOATIME + EPERM fallback, EMFILE/ENFILE rlimit
// retry loop lives HERE, as designed) is shared by process_file() and
// process_fd() through run_verb_on_fd():
//   * Ok        -> stats absorbed, region kept iff lock/lockall, else dropped;
//   * Skipped   -> Failure record (unless silent), Status::Skipped;
//   * Fatal     -> the session re-adds the path and throws FatalError with
//                  the historical message text;
//   * mincore_inaccurate / evict_failed -> Failure records (Ok still).
// process_file() owns the fd it opens and closes it on every exit path.
// process_fd() never closes (the fd stays the caller's, S3 contract).

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

// A2: bridge between the engine's path-less cache::EngineEvents and the
// caller-facing cache::ProgressSink — binds the path of the file currently
// being processed (the A1 FailureSink adapter used the same pattern).
struct Session::PathBoundEvents final : public cache::EngineEvents {
    Session* self;
    const std::string& path;
    cache::ProgressSink* sink;

    PathBoundEvents(Session* s, const std::string& p, cache::ProgressSink* sk)
        : self(s), path(p), sink(sk) {}

    void on_file_start() override { sink->on_file_start(path); }
    void on_evicting() override { sink->on_evicting(path); }
    void on_chart(std::span<const unsigned char> pages, int64_t n) override {
        sink->on_chart(pages, n);
    }
    void on_file_done(int64_t pages, int64_t resident) override {
        sink->on_file_done(path, pages, resident);
    }
};

Session::Session(Options opts, std::unique_ptr<cache::ProgressSink> sink)
    : opts_(std::move(opts)), sink_(std::move(sink)),
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

    // fd is owned from here: closed on every exit path below.
    Status st;
    try {
        st = run_verb_on_fd(opened.value().fd(), path, range_);
    } catch (...) {
        throw;
    }
    return st;
}

Status Session::run_verb_on_fd(int fd, const std::string& path, const FileInfoRange& range) {
    // Shared verb dispatch + classification (A3): used by process_file()
    // (fd opened above) and process_fd() (caller-owned fd, S3).
    cache::Params params;
    params.range = range;
    params.max_file_size = opts_.max_file_size;

    // Verbose gating, exactly the old FileInfo.verbose:
    // events fire iff -v AND a sink is installed.
    PathBoundEvents adapter(this, path, sink_.get());
    cache::EngineEvents* ev = (opts_.verbose > 0 && sink_)
                                  ? static_cast<cache::EngineEvents*>(&adapter)
                                  : nullptr;

    const cache::Verb verb = verb_from_options(opts_);

    Status st;
    try {
        cache::FileResult r;
        switch (verb) {
            case cache::Verb::Inspect: r = cache::inspect(fd, params, ev); break;
            case cache::Verb::Warm:    r = cache::warm(fd, params, ev);    break;
            case cache::Verb::Evict:   r = cache::evict(fd, params, ev);   break;
            case cache::Verb::Lock:    r = cache::lock(fd, params, ev);    break;
        }

        if (r.code == cache::FileResult::Code::Fatal) {
            // The verb lost the path (A2); the session re-adds it with the
            // historical message text.
            throw_fatal(r, path);
        }

        // Facts: the session decides what to collect.
        stats_.total_pages += r.pages_total;
        stats_.total_pages_in_core += r.pages_resident;

        if (r.mincore_inaccurate)
            fail(Failure::Kind::MincoreInaccurate, 0, path);
        if (r.evict_failed)
            fail(r.evict_kind, r.err, path);

        // Region keep-policy, exactly upstream's bail-out guard
        // (!o_lock && !o_lockall && mem -> munmap):
        if (opts_.lock || opts_.lockall) {
            kept_regions_.push_back(std::move(r.region));   // stays mapped
        } else {
            // The session munmaps here (not the region's destructor) so a
            // munmap failure becomes a Failure record WITH path context —
            // upstream: "unable to munmap file %s (%s)".
            int munmap_err = 0;
            if (!r.region.unmap(munmap_err))
                fail(Failure::Kind::MunmapFailed, munmap_err, path);
        }

        if (r.code == cache::FileResult::Code::Ok) {
            st = Status::Ok;
        } else if (r.silent) {
            st = Status::Skipped;     // silent skip: no record (empty file)
        } else {
            fail(r.skip, r.err, path, r.offset);
            st = Status::Skipped;
        }
    } catch (...) {
        throw;   // fd ownership stays with the caller of run_verb_on_fd
    }

    return st;
}

void Session::throw_fatal(const cache::FileResult& r, const std::string& path) {
    switch (r.fatal) {
        case cache::FileResult::FatalWhat::MlockFailed:
            throw FatalError("mlock: " + path + " (" + strerror(r.err) + ")");
        case cache::FileResult::FatalWhat::MincoreFailed:
            throw FatalError("mincore " + path + " (" + strerror(r.err) + ")");
        case cache::FileResult::FatalWhat::MisalignedMap:
            throw FatalError("mmap(" + path + ") wasn't page aligned");
        case cache::FileResult::FatalWhat::None:
            break;
    }
    throw FatalError("unexpected fatal fact");
}
