// core/platform.h — portability helpers for the vmtouch core library.
//
// No printing, no exit(), no global CLI state: hopeless situations are
// reported to the caller, which decides whether they are fatal.

#pragma once

#include <cstdint>

#include <sys/stat.h>
#include <sys/types.h>

namespace platform {

// Reads the page size once into ::pagesize. Must run before other core code.
void init();

// True if p is page-aligned.
bool aligned_p(const void* p);

// mincore(2) vector bit 0 marks a resident page.
bool is_mincore_page_resident(unsigned char mincore_byte);

// gettimeofday(2) as fractional seconds.
double now_seconds();

#ifdef __linux__
// Heuristic for whether mincore(2) reports accurate residency: since
// kernel 5.2 it only does so when the process has write permission on
// the file (side-channel mitigation).
bool can_do_mincore(const struct stat& st);
#endif

// Outcome of raise_nofile_rlimit(); each gets its own user-facing message.
struct RaiseNofileResult {
    enum class Outcome {
        Raised,            // limit successfully increased
        GetrlimitFailed,   // getrlimit(2) failed (err = errno)
        SetrlimitFailed,   // setrlimit(2) failed, not permission (err = errno)
        PermissionDenied,  // setrlimit(2) failed with EPERM
    };
    Outcome outcome;
    int err = 0;           // errno, valid when outcome != Raised
};

// Bumps RLIMIT_NOFILE cur/max by one.
RaiseNofileResult raise_nofile_rlimit();

} // namespace platform
