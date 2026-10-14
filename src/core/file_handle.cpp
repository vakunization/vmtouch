// core/file_handle.cpp — see file_handle.h.

#include "file_handle.h"

#include <cerrno>

#include <fcntl.h>

#include "platform.h"

namespace vmt {
namespace {

// Translates the failed rlimit raise into the historical fatal messages
// (the shared texts live in platform::nofile_fatal_message).
[[noreturn]] void throw_nofile_fatal(const platform::RaiseNofileResult& res) {
    throw FatalError(platform::nofile_fatal_message(res));
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
            open_flags &= ~O_NOATIME;   // owned by someone else: drop O_NOATIME
            continue;
        }
#endif

        if (errno != ENFILE && errno != EMFILE)
            break;

        const auto res = platform::raise_nofile_rlimit();
        if (res.outcome != platform::RaiseNofileResult::Outcome::Raised)
            throw_nofile_fatal(res);
        // limit raised: retry the open
    }

    return Result<FileHandle>::Error(Failure{Failure::Kind::Open, errno, path, 0});
}

} // namespace vmt
