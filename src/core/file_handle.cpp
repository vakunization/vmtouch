// core/file_handle.cpp — see file_handle.h.
//
// The open/retry loop is transplanted verbatim from Session::process_file
// (Increment-1 semantics preserved): same flag fallback, same rlimit
// retry, same fatal texts for the rlimit outcomes.

#include "file_handle.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "platform.h"

namespace vmt {
namespace {

// Translates the rlimit outcome into the monolith's exact fatal messages.
// (Same text the session used before the extraction — B1 is behavior-
// preserving by construction.)
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

Result<FileHandle> FileHandle::open(const std::string& path) {
    int open_flags = O_RDONLY;
#if defined(O_NOATIME)
    open_flags |= O_NOATIME;
#endif

    for (;;) {
        const int fd = ::open(path.c_str(), open_flags, 0);
        if (fd != -1)
            return Result<FileHandle>::Ok(FileHandle(fd));

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

    return Result<FileHandle>::Error(Failure{Failure::Kind::Open, errno, path, 0});
}

} // namespace vmt
