// Internal cache syscall seam. Public cache verbs always use the real calls.
#pragma once

#include <functional>

#include "cache.h"

namespace cache::detail {

struct Syscalls {
    // mincore follows the POSIX return/errno convention; eviction returns an
    // error number directly. unmap reports its error through the out-param.
    std::function<int(void*, int64_t, unsigned char*)> mincore;
    std::function<int(int, int64_t, const MappedRegion&)> evict;
    std::function<bool(MappedRegion&, int&)> unmap;
    std::function<void()> pause_retry;
};

const Syscalls& real_syscalls();
vmt::Result<VerbOutcome> inspect(int fd, const Params& p, const Syscalls& calls);
vmt::Result<VerbOutcome> evict(int fd, const Params& p, const Syscalls& calls);

} // namespace cache::detail
