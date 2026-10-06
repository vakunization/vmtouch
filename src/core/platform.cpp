// core/platform.cpp — see platform.h.

#include "platform.h"

#include <cstdlib>
#include <ctime>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "common.h"

// Definition of the global declared in core/common.h. Lives in the library
// so the core links standalone. Set by platform::init().
long pagesize = 4096;

namespace platform {

void init() {
    pagesize = sysconf(_SC_PAGESIZE);
}

bool aligned_p(const void* p) {
    return 0 == (reinterpret_cast<uintptr_t>(p) & (static_cast<uintptr_t>(pagesize) - 1));
}

bool is_mincore_page_resident(unsigned char mincore_byte) {
    return mincore_byte & 0x1;
}

double now_seconds() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + (tv.tv_usec / 1000000.0);
}

KernelVersion parse_kernel_version(const char* release) {
    KernelVersion v;
    if (!release)
        return v;
    const char* p = release;
    v.major = static_cast<int>(strtoul(p, const_cast<char**>(&p), 10));
    if (*p == '.') {
        ++p;
        v.minor = static_cast<int>(strtoul(p, const_cast<char**>(&p), 10));
    }
    return v;
}

#ifdef __linux__
bool can_do_mincore(const struct stat& st) {
    constexpr KernelVersion kMincoreAccuracyKernel{5, 2};

    struct utsname utsinfo;
    if (uname(&utsinfo) == 0) {
        if (parse_kernel_version(utsinfo.release) < kMincoreAccuracyKernel)
            return true;   // kernel < 5.2: mincore is always accurate
    }

    uid_t uid = getuid();
    return st.st_uid == uid ||
           (st.st_gid == getgid() && (st.st_mode & S_IWGRP)) ||
           (st.st_mode & S_IWOTH) ||
           uid == 0;
}
#endif

RaiseNofileResult raise_nofile_rlimit() {
    struct rlimit r;

    if (getrlimit(RLIMIT_NOFILE, &r))
        return {RaiseNofileResult::Outcome::GetrlimitFailed, errno};

    r.rlim_cur = r.rlim_max + 1;
    r.rlim_max = r.rlim_max + 1;

    if (setrlimit(RLIMIT_NOFILE, &r)) {
        if (errno == EPERM)
            return {RaiseNofileResult::Outcome::PermissionDenied, errno};
        return {RaiseNofileResult::Outcome::SetrlimitFailed, errno};
    }

    return {RaiseNofileResult::Outcome::Raised, 0};
}

} // namespace platform
