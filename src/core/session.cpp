// core/session.cpp — see session.h.
//
// process_file() opens the path (O_NOATIME + EPERM fallback, EMFILE/ENFILE
// rlimit retry loop lives HERE, as designed), then dispatches to one of the
// four cache verbs and CLASSIFIES the returned data:
//   * Ok        -> stats absorbed, region kept iff lock/lockall, else dropped;
//   * Skipped   -> Failure record (unless silent), Status::Skipped;
//   * Fatal     -> the session re-adds the path and throws FatalError with
//                  the historical message text;
//   * mincore_inaccurate / evict_failed -> Failure records (Ok still).
// The fd is owned by this function and closed on every exit path.

#include "session.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "platform.h"

namespace {

// A2: the verb choice is a session-local detail (the engine's public
// surface is the four verbs themselves).
enum class Verb { Query, Touch, Evict, Lock };

Verb verb_from_options(const Options& o) {
    if (o.evict) return Verb::Evict;
    if (o.lock)  return Verb::Lock;
    if (o.touch) return Verb::Touch;
    return Verb::Query;
}

// Translates the rlimit outcome into the monolith's exact fatal messages.
[[noreturn]] void throw_nofile_fatal(const platform::RaiseNofileResult& res) {
    switch (res.outcome) {
        case platform::RaiseNofileResult::Outcome::GetrlimitFailed:
            throw FatalError(std::string("increment_nofile_rlimit: getrlimit (") +
                             strerror(res.err) + ")");
        case platform::RaiseNofileResult::Outcome::PermissionDenied:
            if (getuid() == 0 || geteuid() == 0)
                throw FatalError("system open file limit reached");
            throw FatalError("open file limit reached and unable to increase limit. retry as root");
        case platform::RaiseNofileResult::Outcome::SetrlimitFailed:
            throw FatalError(std::string("increment_nofile_rlimit: setrlimit (") +
                             strerror(res.err) + ")");
        case platform::RaiseNofileResult::Outcome::Raised:
            break;   // not an error; unreachable via this function
    }
    throw FatalError("increment_nofile_rlimit: unexpected state");
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

Session::Session(Options opts, FileFilter filter,
                 std::unique_ptr<cache::ProgressSink> sink)
    : opts_(opts), sink_(std::move(sink)), filter_(std::move(filter)) {}

Session::~Session() = default;

Status Session::process_file(const std::string& path) {
    int open_flags = O_RDONLY;
#if defined(O_NOATIME)
    open_flags |= O_NOATIME;
#endif

    // ---- open with EMFILE/ENFILE retry (retry loop lives HERE) ----------

    int fd = -1;
    for (;;) {
        fd = open(path.c_str(), open_flags, 0);
        if (fd != -1)
            break;

#if defined(O_NOATIME)
        if (errno == EPERM && (open_flags & O_NOATIME)) {
            open_flags &= ~O_NOATIME;   // owned by someone else: retry without O_NOATIME
            continue;
        }
#endif

        if (errno != ENFILE && errno != EMFILE)
            break;

        const auto res = platform::raise_nofile_rlimit();
        if (res.outcome != platform::RaiseNofileResult::Outcome::Raised)
            throw_nofile_fatal(res);
        // limit raised: retry the open immediately
    }

    if (fd == -1) {
        fail(Failure::Kind::Open, errno, path);
        return Status::Skipped;
    }

    // ---- dispatch to the verb and classify the returned data -------------

    cache::Params params;
    params.range = range_;
    params.max_file_size = opts_.max_file_size;

    // Verbose gating, exactly the old FileInfo.verbose:
    // events fire iff -v AND a sink is installed.
    PathBoundEvents adapter(this, path, sink_.get());
    cache::EngineEvents* ev = (opts_.verbose > 0 && sink_)
                                  ? static_cast<cache::EngineEvents*>(&adapter)
                                  : nullptr;

    const Verb verb = verb_from_options(opts_);

    Status st;
    try {
        cache::Result r;
        switch (verb) {
            case Verb::Query: r = cache::inspect(fd, params, ev); break;
            case Verb::Touch: r = cache::warm(fd, params, ev);    break;
            case Verb::Evict: r = cache::evict(fd, params, ev);   break;
            case Verb::Lock:  r = cache::lock(fd, params, ev);    break;
        }

        if (r.code == cache::Result::Code::Fatal) {
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

        if (r.code == cache::Result::Code::Ok) {
            st = Status::Ok;
        } else if (r.silent) {
            st = Status::Skipped;     // silent skip: no record (empty file)
        } else {
            fail(r.skip, r.err, path, r.offset);
            st = Status::Skipped;
        }
    } catch (...) {
        close(fd);
        throw;
    }

    close(fd);
    return st;
}

void Session::throw_fatal(const cache::Result& r, const std::string& path) {
    switch (r.fatal) {
        case cache::Result::FatalWhat::MlockFailed:
            throw FatalError("mlock: " + path + " (" + strerror(r.err) + ")");
        case cache::Result::FatalWhat::MincoreFailed:
            throw FatalError("mincore " + path + " (" + strerror(r.err) + ")");
        case cache::Result::FatalWhat::MisalignedMap:
            throw FatalError("mmap(" + path + ") wasn't page aligned");
        case cache::Result::FatalWhat::None:
            break;
    }
    throw FatalError("unexpected fatal fact");
}
