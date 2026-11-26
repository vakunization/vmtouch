// core/file_handle.h — RAII owner of an open fd; the only place where
// the open logic lives.

#pragma once

#include <string>

#include <unistd.h>   // ::close

#include "common.h"
#include "result.h"

namespace vmt {

class FileHandle {
public:
    // Opens the path with O_RDONLY | O_NOATIME. On EPERM retries without
    // O_NOATIME; on ENFILE/EMFILE retries only if RLIMIT_NOFILE was actually
    // increased (otherwise throws FatalError). Any other error is
    // Failure{Kind::Open, errno, path}.
    static Result<FileHandle> open(const std::string& path);

    FileHandle(FileHandle&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            close_fd();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    ~FileHandle() { close_fd(); }

    int fd() const { return fd_; }

private:
    explicit FileHandle(int fd) : fd_(fd) {}

    void close_fd() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
};

} // namespace vmt
