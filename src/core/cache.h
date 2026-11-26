// core/cache.h — page-cache verbs on an open file descriptor.
//
// Four free functions over an already-open fd; the only place where
// page-cache syscalls live. No path, no CLI concepts.
//
// Return contract:
//   * Ok    -> Result::Ok(VerbOutcome) — stats always; region for Lock or
//              explicitly retained Warm. Other verbs unmap before return.
//   * Fail  -> Result::Error(Failure) — err = the syscall's error code,
//              path EMPTY (the caller attaches the path). Any syscall
//              failure is a per-file Failed, whether retried or not.
//              EINTR, EAGAIN, and EBUSY get at most three attempts;
//              only EAGAIN and EBUSY wait 50 ms between attempts.
//              Traversal continues. A Failed verb contributes no
//              page stats — the file's pages never reach the Report
//              (upstream counted them before the destructive step; this
//              stricter semantics is intentional — document if it changes);
//   * Fatal -> throw FatalError for infrastructure errors such as
//              misaligned mmap or an unsupported platform. Other exceptions
//              such as std::bad_alloc can propagate.
//
// Empty file: silent Ok with zero stats and no region (no record).

#pragma once

#include <cstdint>

#include <sys/mman.h> // munmap

#include "common.h"
#include "result.h"

namespace cache {

// Caller policy per call, assembled by the Session from its Options.
struct Params {
  FileInfoRange range;             // -p: offset / max_len (0 = to EOF)
  size_t max_file_size = SIZE_MAX; // -m (SIZE_MAX = unlimited)
  bool retain_warm_mapping = false;
};

// Owning handle for a live mmap region. Move-only; the destructor munmaps
// if live. unmap() is an explicit early munmap: it clears the handle only
// on success, so a failed call can be retried and the destructor still
// owns the mapping.
class MappedRegion {
public:
  MappedRegion() = default;
  MappedRegion(void *mem, int64_t len) : mem_(mem), len_(len) {}
  ~MappedRegion() {
    if (mem_)
      munmap(mem_, len_);
  }

  MappedRegion(MappedRegion &&other) noexcept : mem_(other.mem_), len_(other.len_) {
    other.mem_ = nullptr;
    other.len_ = 0;
  }
  MappedRegion &operator=(MappedRegion &&other) noexcept {
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

  MappedRegion(const MappedRegion &) = delete;
  MappedRegion &operator=(const MappedRegion &) = delete;

  void *mem() const { return mem_; }
  int64_t len() const { return len_; }
  explicit operator bool() const { return mem_ != nullptr; }

  // Explicit early munmap. Idempotent after success; on failure returns
  // false + errno and the region stays live.
  bool unmap(int &err) {
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
  void *mem_ = nullptr;
  int64_t len_ = 0;
};

// Ok payload: stats for all four verbs; region for Lock or retained Warm.
struct VerbOutcome {
  Stats stats;
  MappedRegion region;
};

// The four verbs (fd is the only identity).
vmt::Result<VerbOutcome> inspect(int fd, const Params &p);
vmt::Result<VerbOutcome> warm(int fd, const Params &p);
vmt::Result<VerbOutcome> evict(int fd, const Params &p);
vmt::Result<VerbOutcome> lock(int fd, const Params &p);

} // namespace cache
