// core/cache.h — page-cache verbs on an open file descriptor (spec2 §7).
//
// Four free functions over an already-open fd; the ONLY place where
// page-cache syscalls live. No path, no sinks, no CLI concepts.
//
// Return contract (one error language; R2 + DEV-1/DEV-2 adjudications,
// 2026-10-05):
//   * Ok   -> Result::Ok(VerbOutcome) — stats always; region ONLY for lock
//             (inspect/warm/evict munmap inside themselves before return);
//   * Fail -> Result::Error(Failure) — kind from §8.1 (as amended: Kind 21
//             MlockFailed, Kind 22 MincoreFailed), err = the syscall's error
//             code, path EMPTY (the caller attaches the path). ANY syscall
//             failure is a per-file Failed — non-retryable AND
//             retryable-after-N (3×50ms, EINTR/EAGAIN/EBUSY) alike;
//             traversal continues;
//   * Fatal-> throw FatalError — infrastructure only: allocation failure,
//             misaligned mmap, unsupported platform. Traversal stops.
//
// Empty file: silent Ok with zero stats and no region (no record).

#pragma once

#include <cstdint>
#include <stddef.h>

#include <sys/mman.h>    // munmap

#include "common.h"
#include "result.h"

namespace cache {

// Caller policy per call (spec2 §2.9): assembled by the Session from its
// Options. cache::* knows nothing about Options.
struct Params {
    FileInfoRange range;                    // -p: offset / max_len (0 = to EOF)
    size_t max_file_size = SIZE_MAX;        // -m (upstream default: unlimited)
};

// What to do with the mapped pages. FROZEN values (C ABI): see
// docs/vocabulary.md. The verb × fd functions below implement these.
enum class Verb : int32_t {
    Inspect = 1,   // query residency (no -t/-e/-l)
    Warm    = 2,   // -t: touch pages into cache
    Evict   = 3,   // -e: drop pages from cache
    Lock    = 4,   // -l: touch + mlock, mapping kept
};

// ---------------------------------------------------------------------------
// MappedRegion: owning handle for a live mmap region (spec2 §2.11).
// Move-only; destructor munmaps if live; unmap(int&) is an explicit early
// munmap, idempotent, returns false + errno on failure (region cleared).
// ---------------------------------------------------------------------------
class MappedRegion {
public:
    MappedRegion() = default;
    MappedRegion(void* mem, int64_t len) : mem_(mem), len_(len) {}
    ~MappedRegion() {
        if (mem_)
            munmap(mem_, len_);
    }

    MappedRegion(MappedRegion&& other) noexcept : mem_(other.mem_), len_(other.len_) {
        other.mem_ = nullptr;
        other.len_ = 0;
    }
    MappedRegion& operator=(MappedRegion&& other) noexcept {
        if (this != &other) {
            if (mem_)
                munmap(mem_, len_);
            mem_ = other.mem_;
            len_ = other.len_;
            other.mem_ = nullptr;
            other.len_ = 0;
        }
        return *this;
    }

    MappedRegion(const MappedRegion&) = delete;
    MappedRegion& operator=(const MappedRegion&) = delete;

    void* mem() const { return mem_; }
    int64_t len() const { return len_; }
    explicit operator bool() const { return mem_ != nullptr; }

    // Explicit early munmap (MISS-1 / unmap option (a), 2026-10-05):
    // the handle is cleared ONLY on success — on failure the region stays
    // live so a subsequent unmap() genuinely re-attempts munmap (the retry
    // in unmap_or_fail works), and the destructor makes a final attempt.
    // Idempotent after success: a second call is a no-op returning true.
    bool unmap(int& err) {
        if (!mem_)
            return true;
        if (munmap(mem_, len_) != 0) {
            err = errno;
            return false;   // region intentionally NOT cleared on failure
        }
        err = 0;
        mem_ = nullptr;
        len_ = 0;
        return true;
    }

private:
    void* mem_ = nullptr;
    int64_t len_ = 0;
};

// Ok payload (spec2 §2.10): stats for all four verbs; region only for lock.
struct VerbOutcome {
    Stats stats;
    MappedRegion region;   // Lock only; empty otherwise
};

// The four verbs (fd is the only identity).
vmt::Result<VerbOutcome> inspect(int fd, const Params& p);
vmt::Result<VerbOutcome> warm(int fd, const Params& p);
vmt::Result<VerbOutcome> evict(int fd, const Params& p);
vmt::Result<VerbOutcome> lock(int fd, const Params& p);

} // namespace cache
