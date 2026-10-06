// core/cache.h — page-cache verbs on an open file descriptor.
//
// Four free functions over an already-open fd; the only place where
// page-cache syscalls live. No path, no CLI concepts.
//
// Return contract:
//   * Ok    -> Result::Ok(VerbOutcome) — stats always; region ONLY for lock
//              (inspect/warm/evict munmap inside themselves before return);
//   * Fail  -> Result::Error(Failure) — err = the syscall's error code,
//              path EMPTY (the caller attaches the path). Any syscall
//              failure is a per-file Failed — non-retryable and
//              retryable-after-N (3x50ms, EINTR/EAGAIN/EBUSY) alike;
//              traversal continues;
//   * Fatal -> throw FatalError — infrastructure only: allocation failure,
//              misaligned mmap, unsupported platform. Traversal stops.
//
// Empty file: silent Ok with zero stats and no region (no record).

#pragma once

#include <cstdint>
#include <stddef.h>

#include <sys/mman.h>    // munmap

#include "common.h"
#include "result.h"

namespace cache {

// Caller policy per call, assembled by the Session from its Options.
struct Params {
    FileInfoRange range;                    // -p: offset / max_len (0 = to EOF)
    size_t max_file_size = SIZE_MAX;        // -m (SIZE_MAX = unlimited)
};

// What to do with the mapped pages. Values are frozen (C ABI).
enum class Verb : int32_t {
    Inspect = 1,   // query residency
    Warm    = 2,   // -t: touch pages into cache
    Evict   = 3,   // -e: drop pages from cache
    Lock    = 4,   // -l: touch + mlock, mapping kept
};

// Owning handle for a live mmap region. Move-only; the destructor munmaps
// if live. unmap() is an explicit early munmap: it clears the handle only
// on success, so a failed call can be retried and the destructor still
// owns the mapping.
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

    // Explicit early munmap. Idempotent after success; on failure returns
    // false + errno and the region stays live.
    bool unmap(int& err) {
        if (!mem_)
            return true;
        if (munmap(mem_, len_) != 0) {
            err = errno;
            return false;
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

// Ok payload: stats for all four verbs; region only for lock.
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
