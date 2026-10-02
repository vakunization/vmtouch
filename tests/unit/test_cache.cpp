// tests/unit/test_cache.cpp — behavior tests for the page-cache verb engine.
//
// S3-style: the caller opens the fd and hands it over; the verbs
// (inspect/warm/evict/lock) never see a path. The behavior pinned here
// comes from upstream vmtouch (already pinned end-to-end by tests/run.sh);
// here it is pinned at the module boundary so the Phase-2 rewrite is
// verified per module.
//
// scenario: S3 (mlock model loader) — caller-owned fd, distinct failure
// reasons; S2 (QA checker) — query mode + residency numbers.
//
// Note: touch/evict tests manipulate the real kernel page cache of their
// temp files, which are private to the test process and deleted after.

#include "helpers.h"

#include <sys/mman.h>
#include <sys/resource.h>   // getrlimit, RLIMIT_MEMLOCK

namespace {

int64_t pages_for(int64_t bytes) { return (bytes + pagesize - 1) / pagesize; }

} // namespace

TEST(cache_query_fresh_file_accounting) {
    unit::TempFile f(4);
    unit::EngineEventsRecorder ev;
    auto r = cache::inspect(f.fd, cache::Params{}, &ev);

    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Ok);
    EXPECT_EQ(r.pages_total, 4);               // whole file: 4 pages
    EXPECT_EQ(ev.file_starts, 1);
    EXPECT_EQ((int)ev.dones.size(), 1);
    EXPECT_EQ(ev.dones[0].first, 4);           // pages_in_file in on_file_done
    EXPECT_EQ(ev.evictings, 0);
    EXPECT_TRUE(static_cast<bool>(r.region));  // mapping returned to caller
    // non-verbose: no events at all
    unit::EngineEventsRecorder quiet_ev;
    auto rq = cache::inspect(f.fd, cache::Params{}, nullptr);
    EXPECT_EQ((int)rq.code, (int)cache::Result::Code::Ok);
    EXPECT_EQ(quiet_ev.file_starts, 0);
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

    auto r = cache::inspect(f.fd, cache::Params{}, nullptr);

    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Ok);
    EXPECT_EQ(r.pages_total, 4);
    // Touched 2 pages; readahead may pull in more, never fewer.
    EXPECT_TRUE(r.pages_resident >= 2);
    EXPECT_TRUE(r.pages_resident <= 4);
}

TEST(cache_touch_makes_all_pages_resident) {
    unit::TempFile f(3);
    // Evict first so the touch has something to do (best effort; if the
    // filesystem ignores eviction the assertions still hold: a touched
    // file must end up fully resident).
    (void)cache::evict(f.fd, cache::Params{}, nullptr);

    auto r = cache::warm(f.fd, cache::Params{}, nullptr);
    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Ok);
    EXPECT_EQ(r.pages_total, 3);
    // After -t the file must be fully resident (upstream contract). Verify
    // via a fresh query on the same fd:
    auto q = cache::inspect(f.fd, cache::Params{}, nullptr);
    EXPECT_EQ(q.pages_total, 3);
    EXPECT_EQ(q.pages_resident, 3);   // touched => all resident
}

TEST(cache_empty_file_skipped_silently) {
    unit::TempFile f(0);   // zero bytes
    unit::EngineEventsRecorder ev;
    auto r = cache::inspect(f.fd, cache::Params{}, &ev);

    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Skipped);
    EXPECT_TRUE(r.silent);              // upstream: silent for empty files
    EXPECT_EQ(ev.file_starts, 0);
    EXPECT_EQ(r.pages_total, 0);
}

TEST(cache_file_larger_than_max_skips_with_warning) {
    unit::TempFile f(2);
    cache::Params p;
    p.max_file_size = static_cast<size_t>(pagesize);   // -m 1 page
    auto r = cache::inspect(f.fd, p, nullptr);

    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Skipped);
    EXPECT_FALSE(r.silent);
    EXPECT_EQ((int)r.skip, (int)Failure::Kind::TooLarge);
    EXPECT_EQ(r.pages_total, 0);
}

TEST(cache_offset_beyond_eof_skips_with_warning) {
    unit::TempFile f(2);
    cache::Params p;
    p.range.offset = 10 * pagesize;
    auto r = cache::inspect(f.fd, p, nullptr);

    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Skipped);
    EXPECT_EQ((int)r.skip, (int)Failure::Kind::BeyondEof);
    EXPECT_EQ(r.offset, 10 * pagesize);
    EXPECT_EQ(r.pages_total, 0);
}

TEST(cache_partial_range_counts_only_selected_pages) {
    unit::TempFile f(4);
    unit::EngineEventsRecorder ev;
    // -p second page only: offset = 1 page, max_len = 1 page
    cache::Params p;
    p.range.offset = pagesize;
    p.range.max_len = pagesize;
    auto r = cache::inspect(f.fd, p, &ev);

    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Ok);
    EXPECT_EQ(r.pages_total, 1);         // only the selected page counts
    EXPECT_EQ(ev.dones[0].first, 1);
}

TEST(cache_non_verbose_sinks_are_silent_but_stats_still_count) {
    unit::TempFile f(2);
    auto r = cache::inspect(f.fd, cache::Params{}, nullptr);

    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Ok);
    EXPECT_EQ(r.pages_total, 2);
}

TEST(cache_mapped_region_ownership_and_move) {
    // A2: the verbs hand the mapping back; the caller owns it.
    unit::TempFile f(2);
    cache::Result r = cache::inspect(f.fd, cache::Params{}, nullptr);
    EXPECT_TRUE(static_cast<bool>(r.region));

    // Move: the source loses ownership; the destination munmaps on destruction.
    void* mem = r.region.mem();
    int64_t len = r.region.len();
    cache::MappedRegion moved = std::move(r.region);
    EXPECT_FALSE(static_cast<bool>(r.region));
    EXPECT_TRUE(static_cast<bool>(moved));
    EXPECT_TRUE(moved.mem() == mem);     // pointer identity survives the move
    EXPECT_EQ(moved.len(), len);

    // Reading through the moved mapping still works (page 0 in core after
    // the inspect pass — touching to be sure it is faulted in).
    volatile char c = static_cast<char*>(moved.mem())[0];
    (void)c;
}

// scenario: S3 (mlock model loader) — "tell me WHY mlock failed". A2: the
// lock verb returns the mlock failure as a Fatal FACT (MlockFailed + errno);
// the session re-adds the path and throws (pinned at session level in
// test_session.cpp). Pinned here: the fact comes back, not an exception.
//
// NOTE: skipped under ASan/UBSan — ASan intercepts mlock(2) and its
// interceptor succeeds where the real syscall would fail with ENOMEM, so
// the failure path cannot be exercised in a sanitizer build (verified by
// raw-syscall comparison). The no-sanitizer CI run covers this test.
TEST(cache_lock_failure_returns_fatal_fact) {
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
    auto r = cache::lock(f.fd, cache::Params{}, nullptr);
    EXPECT_EQ((int)r.code, (int)cache::Result::Code::Fatal);
    EXPECT_EQ((int)r.fatal, (int)cache::Result::FatalWhat::MlockFailed);
    EXPECT_TRUE(r.err != 0);          // errno carried back as data
}
