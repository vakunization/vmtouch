// core/platform.h — portability helpers for the vmtouch core library.
//
// Extracted verbatim from the monolith (commit 3). Rules for this module:
//   - no printing, no exit(), no global CLI state;
//   - hopeless situations are reported to the caller, which decides whether
//     they are fatal (the CLI translates them into FatalError with the
//     original message text).

#pragma once

#include <cstdint>

#include <sys/stat.h>
#include <sys/types.h>

namespace platform {

// Reads the page size once and stores it in ::pagesize (declared in
// core/common.h). Must be called before any other core function.
void init();

// True if p is page-aligned. (mmap(2) already guarantees page alignment of
// the mapping start; kept because the CLI pins the fatal on misalignment.)
bool aligned_p(const void* p);

// mincore(2) vector bit 0 marks a resident page.
bool is_mincore_page_resident(unsigned char mincore_byte);

// gettimeofday(2) as fractional seconds; used for chart throttling.
double now_seconds();

#ifdef __linux__
// Heuristic for whether mincore(2) reports accurate residency: since kernel
// 5.2 it only does so when the process has write permission on the file
// (side-channel mitigation). Verbatim from upstream.
bool can_do_mincore(const struct stat& st);
#endif

// Result of raise_nofile_rlimit(): the distinct outcomes get distinct
// user-facing messages, so the caller can produce them verbatim.
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

// Bumps RLIMIT_NOFILE cur/max by one. Error-code based: the caller turns
// the outcome into the original fatal messages ("system open file limit
// reached" / "... retry as root" / getrlimit/setrlimit diagnostics).
RaiseNofileResult raise_nofile_rlimit();

} // namespace platform
