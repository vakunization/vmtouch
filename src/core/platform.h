// core/platform.h — portability helpers for the vmtouch core library.
//
// No printing, no exit(), no global CLI state: hopeless situations are
// reported to the caller, which decides whether they are fatal.

#pragma once

#include <string>

namespace platform {

// Returns the system page size. Cached on first use; throws FatalError if
// the operating system cannot provide it.
long page_size();

// True if p is page-aligned.
bool aligned_p(const void* p);

// mincore(2) vector bit 0 marks a resident page.
bool is_mincore_page_resident(unsigned char mincore_byte);

// Outcome of raise_nofile_rlimit(); each gets its own user-facing message.
struct RaiseNofileResult {
    enum class Outcome {
        Raised,            // soft limit successfully increased
        AlreadyAtLimit,    // soft limit is already at the hard limit
        GetrlimitFailed,   // getrlimit(2) failed (err = errno)
        SetrlimitFailed,   // setrlimit(2) failed, not permission (err = errno)
        PermissionDenied,  // setrlimit(2) failed with EPERM
    };
    Outcome outcome;
    int err = 0;           // errno for a failed getrlimit or setrlimit call
};

// Bumps RLIMIT_NOFILE soft limit to the hard limit (the hard limit is never
// touched). See platform.cpp for the reasoning.
RaiseNofileResult raise_nofile_rlimit();

// The user-facing fatal message for a failed raise (the historical upstream
// texts). Callers throw FatalError with it.
std::string nofile_fatal_message(const RaiseNofileResult& res);

} // namespace platform
