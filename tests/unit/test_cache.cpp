// tests/unit/test_cache.cpp — behavior tests for the page-cache verb engine
// (B2 surface: Result<VerbOutcome>, no sinks, no paths).
//
// S3-style: the caller opens the fd and hands it over; the verbs
// (inspect/warm/evict/lock) never see a path and never emit events.
// The behavior pinned here is upstream vmtouch's (already pinned
// end-to-end by tests/run.sh); here it is pinned at the module boundary.
//
// Note: touch/evict tests manipulate the real kernel page cache of their
// temp files, which are private to the test process and deleted after.

#include "helpers.h"

#include <sys/mman.h>
#include <sys/resource.h>   // getrlimit, RLIMIT_MEMLOCK

namespace {

using cache::Params;
using cache::VerbOutcome;

int64_t pages_for(int64_t bytes) { return (bytes + pagesize - 1) / pagesize; }

} // namespace

TEST(cache_query_fresh_file_accounting) {
    unit::TempFile f(4);
    auto r = cache::inspect(f.fd, Params{});

    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().stats.total_pages, 4);           // whole file: 4 pages
    EXPECT_FALSE(static_cast<bool>(r.value().region));   // inspect munmaps inside (spec2 §7.12)
}

TEST(cache_query_resident_count_is_consistent) {
    unit::TempFile f(4);
    // Make pages 0 and 2 definitely resident: mmap and read every 2nd page.
    void* m = mmap(nullptr, f.size, PROT_READ, MAP_SHARED, f.fd, 0);
    EXPECT_TRUE(m != MAP_FAILED);
    volatile char sum = 0;
    for (int64_t off = 0; off < f.size; off += 2 * pagesize)
        sum += static_cast<char*>(m)[off];
    munmap(m, f.size);

    auto r = cache::inspect(f.fd, Params{});

    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().stats.total_pages, 4);
    // Touched 2 pages; readahead may pull in more, never fewer.
    EXPECT_TRUE(r.value().stats.total_pages_in_core >= 2);
    EXPECT_TRUE(r.value().stats.total_pages_in_core <= 4);
}

TEST(cache_touch_makes_all_pages_resident) {
    unit::TempFile f(3);
    // Evict first so the touch has something to do (best effort; if the
    // filesystem ignores eviction the assertions still hold: a touched
    // file must end up fully resident).
    (void)cache::evict(f.fd, Params{});

    auto r = cache::warm(f.fd, Params{});
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().stats.total_pages, 3);
    // After -t the file must be fully resident (upstream contract). Verify
    // via a fresh query on the same fd:
    auto q = cache::inspect(f.fd, Params{});
    EXPECT_EQ(q.value().stats.total_pages, 3);
    EXPECT_EQ(q.value().stats.total_pages_in_core, 3);   // touched => all resident
}

TEST(cache_empty_file_silent_ok_zero_stats) {
    unit::TempFile f(0);   // zero bytes
    auto r = cache::inspect(f.fd, Params{});

    EXPECT_TRUE(r.is_ok());                    // Ok, NOT Skipped (spec2 §7.13)
    EXPECT_EQ(r.value().stats.total_pages, 0);
    EXPECT_EQ(r.value().stats.total_pages_in_core, 0);
    EXPECT_FALSE(static_cast<bool>(r.value().region));
}

TEST(cache_file_larger_than_max_is_error_with_kind) {
    unit::TempFile f(2);
    Params p;
    p.max_file_size = static_cast<size_t>(pagesize);   // -m 1 page
    auto r = cache::inspect(f.fd, p);

    EXPECT_TRUE(r.is_error());
    EXPECT_EQ((int)r.error().kind, (int)Failure::Kind::TooLarge);
    EXPECT_TRUE(r.error().path.empty());       // verbs are path-free; session attaches
    EXPECT_EQ(r.error().err, 0);
}

TEST(cache_offset_beyond_eof_is_error_with_offset) {
    unit::TempFile f(2);
    Params p;
    p.range.offset = 10 * pagesize;
    auto r = cache::inspect(f.fd, p);

    EXPECT_TRUE(r.is_error());
    EXPECT_EQ((int)r.error().kind, (int)Failure::Kind::BeyondEof);
    EXPECT_EQ(r.error().offset, 10 * pagesize);
}

TEST(cache_partial_range_counts_only_selected_pages) {
    unit::TempFile f(4);
    // -p second page only: offset = 1 page, max_len = 1 page
    Params p;
    p.range.offset = pagesize;
    p.range.max_len = pagesize;
    auto r = cache::inspect(f.fd, p);

    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().stats.total_pages, 1);   // only the selected page counts
}

TEST(cache_mapped_region_ownership_and_move) {
    // B2: inspect munmaps inside itself (spec2 §7.12) — its region is
    // empty. The ownership/move/unmap semantics are pinned on lock() (the
    // one verb that hands a LIVE mapping to the caller).
    unit::TempFile f(2);
    auto r = cache::lock(f.fd, Params{});
    EXPECT_TRUE(r.is_ok());
    EXPECT_TRUE(static_cast<bool>(r.value().region));

    // Move: the source loses ownership; the destination munmaps on destruction.
    void* mem = r.value().region.mem();
    int64_t len = r.value().region.len();
    cache::MappedRegion moved = std::move(r.value().region);
    EXPECT_FALSE(static_cast<bool>(r.value().region));
    EXPECT_TRUE(static_cast<bool>(moved));
    EXPECT_TRUE(moved.mem() == mem);     // pointer identity survives the move
    EXPECT_EQ(moved.len(), len);

    // Reading through the moved mapping still works (faulted in by lock's
    // touch pass).
    volatile char c = static_cast<char*>(moved.mem())[0];
    (void)c;

    // unmap(int&): idempotent, second call is a no-op returning true.
    int err = 0;
    EXPECT_TRUE(moved.unmap(err));
    EXPECT_FALSE(static_cast<bool>(moved));
    EXPECT_TRUE(moved.unmap(err));       // idempotent
    EXPECT_TRUE(moved.mem() == nullptr); // cleared — do NOT dereference after unmap
}

TEST(cache_evict_reports_advisory_failure_kind) {
    // Eviction of a fresh private temp file normally succeeds; this test
    // pins the Ok shape (stats populated, region empty). The failure kind
    // (EvictAdviseFailed) cannot be triggered deterministically without a
    // bad fd — pinned at the classification level in B6 via session tests.
    unit::TempFile f(2);
    auto r = cache::evict(f.fd, Params{});
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().stats.total_pages, 2);
    EXPECT_FALSE(static_cast<bool>(r.value().region));
}

// scenario: S3 (mlock model loader) — "tell me WHY mlock failed". Frozen
// split (owner ruling, OVERTURNED 2026-10-05): non-retryable AND
// retryable-after-N mlock failures are now a per-file Failed record —
// Failed{MlockFailed, errno}, traversal continues (bugs.md DEV-1).
//
// NOTE: skipped under ASan/UBSan — ASan intercepts mlock(2) and its
// interceptor succeeds where the real syscall would fail with ENOMEM, so
// the failure path cannot be exercised in a sanitizer build (verified by
// raw-syscall comparison). The no-sanitizer CI run covers this test.
TEST(cache_lock_failure_is_failed_record) {
#ifdef __SANITIZE_ADDRESS__
    std::printf("  skip lock-failure test (ASan intercepts mlock)\n");
    return;
#endif
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY) {
        std::printf("  skip lock-failure test (memlock unlimited)\n");
        return;
    }
    // File far larger than the memlock budget => mlock must fail.
    const int64_t need = static_cast<int64_t>(rl.rlim_cur) + 64 * 1024 * 1024;
    const int npages = static_cast<int>((need + pagesize - 1) / pagesize);
    if (npages > 2000000) {   // keep the temp file bounded (8 TB page space guard)
        std::printf("  skip lock-failure test (rlimit too large)\n");
        return;
    }
    unit::TempFile f(npages);
    auto r = cache::lock(f.fd, Params{});
    EXPECT_TRUE(r.is_error());                                  // Failed, not fatal
    EXPECT_EQ((int)r.error().kind, (int)Failure::Kind::MlockFailed);
    EXPECT_TRUE(r.error().path.empty());                        // verbs are path-free
    EXPECT_TRUE(r.error().err != 0);                            // errno carried (ENOMEM)
}

TEST(cache_lock_success_returns_live_region) {
    // Lock hands the mapping to the caller (spec2 §7.10): region populated,
    // still valid, caller munmaps (we do, explicitly — the file is private).
    unit::TempFile f(2);
    auto r = cache::lock(f.fd, Params{});
    EXPECT_TRUE(r.is_ok());
    EXPECT_TRUE(static_cast<bool>(r.value().region));
    EXPECT_EQ(r.value().stats.total_pages, 2);

    int err = 0;
    EXPECT_TRUE(r.value().region.unmap(err));
}
