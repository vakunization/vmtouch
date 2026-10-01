// core/platform.cpp — see platform.h.

#include "platform.h"

#include <cstdlib>
#include <ctime>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <ctype.h>

#include "common.h"

// Definition of the global declared in core/common.h. Lives in the library
// (not the CLI) so the core links standalone. Set by platform::init().
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

#ifdef __linux__
// Verbatim port of upstream can_do_mincore(): parse the kernel release and
// check write permission for kernel >= 5.2.
bool can_do_mincore(const struct stat& st) {
    struct utsname utsinfo;
    if (uname(&utsinfo) == 0) {
        unsigned long ver[16] = {};
        int i = 0;
        const char* p = utsinfo.release;
        while (*p) {
            if (isdigit(static_cast<unsigned char>(*p))) {
                ver[i] = strtoul(p, const_cast<char**>(&p), 10);
                i++;
            } else {
                p++;
            }
        }
        // kernel < 5.2
        if (ver[0] < 5 || ver[1] < 2)
            return true;
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
