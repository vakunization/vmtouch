// core/file_handle.h — RAII owner of an open fd; the ONLY place where the
// open logic lives (B1, per spec2 §6).
//
// Open algorithm (verbatim semantics from the Session code this extracts):
//   1. O_RDONLY | O_NOATIME (when available);
//   2. EPERM with O_NOATIME -> retry once without the flag;
//   3. ENFILE/EMFILE -> platform::raise_nofile_rlimit + retry; a failed
//      raise becomes the corresponding FatalError (historical text);
//   4. anything else -> Result::Error(Failure{Kind::Open, errno, path}).
//
// Knows: path, fd, open logic.  Does not know: the verb, Session, Report,
// cache::* (spec2 knowledge map).

#pragma once

#include <string>

#include <unistd.h>   // ::close

#include "common.h"
#include "result.h"

namespace vmt {

class FileHandle {
public:
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
