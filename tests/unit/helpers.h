// tests/unit/helpers.h — shared temporary file fixture.

#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "test.h"
#include "core/common.h"

namespace unit {

// ---------------------------------------------------------------------------
// TempFile: RAII temp file of n pages, fd kept open for the test's lifetime.
// ---------------------------------------------------------------------------
struct TempFile {
    std::string path;
    int fd = -1;
    int64_t size;   // bytes

    explicit TempFile(int n_pages)
        : size(static_cast<int64_t>(n_pages) * pagesize) {
        char tmpl[] = "/tmp/vmtouch-unit-XXXXXX";
        fd = mkstemp(tmpl);
        if (fd == -1) { std::perror("mkstemp"); std::abort(); }
        path = tmpl;
        if (ftruncate(fd, size) == -1) { std::perror("ftruncate"); std::abort(); }
    }
    ~TempFile() {
        if (fd != -1) ::close(fd);
        ::unlink(path.c_str());
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

} // namespace unit
