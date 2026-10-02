// core/session.cpp — see session.h.
//
// process_file() absorbs the front matter that used to live in the CLI's
// vmtouch_file() shim: open with O_NOATIME + EPERM fallback, the
// EMFILE/ENFILE rlimit retry (retries live here, per the design), then the
// open fd is handed to the engine which performs the remaining checks.
// The fd is owned by this function and closed on every exit path.

#include "session.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "platform.h"

namespace {

cache::Mode mode_from_options(const Options& o) {
    if (o.evict) return cache::Mode::Evict;
    if (o.lock)  return cache::Mode::Lock;
    if (o.touch) return cache::Mode::Touch;
    return cache::Mode::Query;
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

Session::Session(Options opts, std::unique_ptr<cache::ProgressSink> sink, WarnFn warn)
    : opts_(opts), sink_(std::move(sink)), warn_(std::move(warn)) {}

Session::Session(Options opts, FileFilter filter,
                 std::unique_ptr<cache::ProgressSink> sink, WarnFn warn)
    : opts_(opts), sink_(std::move(sink)), warn_(std::move(warn)),
      filter_(std::move(filter)) {}

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

    // ---- hand the open fd to the engine (owns the remaining checks) -----

    cache::FileInfo fi;
    fi.path = path;
    fi.fd = fd;
    fi.offset = range_.offset;
    fi.max_len = range_.max_len;
    fi.max_file_size = opts_.max_file_size;
    fi.verbose = opts_.verbose > 0 && sink_ != nullptr;
    fi.lockall = opts_.lockall;

    static cache::NullSink g_null_sink;
    cache::ProgressSink& sink = sink_ ? *sink_ : static_cast<cache::ProgressSink&>(g_null_sink);

    // cache::FailureSink adapter: failures carry the path being processed.
    struct PathSink final : public cache::FailureSink {
        Session* self;
        const std::string* path;
        PathSink(Session* s, const std::string* p) : self(s), path(p) {}
        void add(Failure::Kind kind, int err = 0, int64_t offset = 0) override {
            self->fail(kind, err, *path, offset);
        }
    } path_sink{this, &path};

    Status st;
    try {
        st = cache::process_file(fi, mode_from_options(opts_), stats_,
                                 path_sink, sink);
    } catch (...) {
        close(fd);
        throw;
    }

    close(fd);
    return st;
}
