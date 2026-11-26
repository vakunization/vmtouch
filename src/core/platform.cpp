// core/platform.cpp — see platform.h.

#include "platform.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <limits.h>   // RLIM_INFINITY
#include <sys/resource.h>
#include <unistd.h>

#include "common.h"

// Definition of the global declared in core/common.h. Lives in the library
// so the core links standalone. Set by platform::init().
long pagesize = 4096;

namespace platform {

void init() {
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)   // -1 (error) or 0 (never true, but poison either way)
        throw FatalError("sysconf(_SC_PAGESIZE) failed");
    pagesize = ps;
}

bool aligned_p(const void* p) {
    return 0 == (reinterpret_cast<uintptr_t>(p) & (static_cast<uintptr_t>(pagesize) - 1));
}

bool is_mincore_page_resident(unsigned char mincore_byte) {
    return mincore_byte & 0x1;
}

RaiseNofileResult raise_nofile_rlimit() {
    struct rlimit r;

    if (getrlimit(RLIMIT_NOFILE, &r))
        return {RaiseNofileResult::Outcome::GetrlimitFailed, errno};

    // Raise the SOFT limit to the hard limit and leave the hard limit alone:
    // raising rlim_max requires CAP_SYS_RESOURCE (unprivileged setrlimit would
    // always fail with EPERM), and rlim_max + 1 wraps RLIM_INFINITY to 0.
    if (r.rlim_cur >= r.rlim_max)   // already at the hard limit (incl. RLIM_INFINITY)
        return {RaiseNofileResult::Outcome::AlreadyAtLimit, 0};
    r.rlim_cur = r.rlim_max;

    if (setrlimit(RLIMIT_NOFILE, &r)) {
        if (errno == EPERM)
            return {RaiseNofileResult::Outcome::PermissionDenied, errno};
        return {RaiseNofileResult::Outcome::SetrlimitFailed, errno};
    }

    return {RaiseNofileResult::Outcome::Raised, 0};
}

std::string nofile_fatal_message(const RaiseNofileResult& res) {
    switch (res.outcome) {
        case RaiseNofileResult::Outcome::Raised:
            break;   // not an error; not reachable via this function
        case RaiseNofileResult::Outcome::AlreadyAtLimit:
            return "open file limit reached (RLIMIT_NOFILE is already at its hard limit)";
        case RaiseNofileResult::Outcome::GetrlimitFailed:
            return std::string("increment_nofile_rlimit: getrlimit (") + strerror(res.err) + ")";
        case RaiseNofileResult::Outcome::PermissionDenied:
            if (getuid() == 0 || geteuid() == 0)
                return "system open file limit reached";
            return "open file limit reached and unable to increase limit. retry as root";
        case RaiseNofileResult::Outcome::SetrlimitFailed:
            return std::string("increment_nofile_rlimit: setrlimit (") +
                   strerror(res.err) + ")";
    }
    return "increment_nofile_rlimit: unexpected state";
}

} // namespace platform
