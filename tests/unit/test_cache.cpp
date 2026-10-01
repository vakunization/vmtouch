// tests/unit/test_cache.cpp — behavior tests for the page-cache engine.
//
// S3-style: the caller opens the fd and hands it over;
// cache::process_file() never opens a path. The behavior pinned here comes
// from upstream vmtouch (already pinned end-to-end by tests/run.sh); here
// it is pinned at the module boundary so the Phase-2 rewrite is verified
// per module.
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
    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;

    auto fi = unit::make_file_info(f.path, f.fd);
    auto st = cache::process_file(fi, cache::Mode::Query, stats,
                                  unit::warn_of(warn), sink);

    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(stats.total_pages, 4);          // whole file: 4 pages
    EXPECT_EQ(sink.file_starts.size(), (size_t)1);
    EXPECT_EQ(sink.done.size(), (size_t)1);
    EXPECT_EQ(sink.done[0].second.first, 4);  // pages_in_file in on_file_done
    EXPECT_TRUE(sink.evictings.empty());
    EXPECT_TRUE(warn.messages.empty());       // fresh 4-page file: no warnings
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

    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;
    auto fi = unit::make_file_info(f.path, f.fd);
    auto st = cache::process_file(fi, cache::Mode::Query, stats,
                                  unit::warn_of(warn), sink);

    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(stats.total_pages, 4);
    // Touched 2 pages; readahead may pull in more, never fewer.
    EXPECT_TRUE(stats.total_pages_in_core >= 2);
    EXPECT_TRUE(stats.total_pages_in_core <= 4);
    // on_file_done reports the same residency the stats counted.
    EXPECT_EQ(sink.done[0].second.second, stats.total_pages_in_core);
}

TEST(cache_touch_makes_all_pages_resident) {
    unit::TempFile f(3);
    // Evict first so the touch has something to do (best effort; if the
    // filesystem ignores eviction the assertions still hold: a touched
    // file must end up fully resident).
    unit::WarningCapture warn;
    unit::RecordingSink sink_evict;
    Stats stats_evict;
    auto fi = unit::make_file_info(f.path, f.fd);
    (void)cache::process_file(fi, cache::Mode::Evict, stats_evict,
                              unit::warn_of(warn), sink_evict);

    unit::RecordingSink sink_touch;
    Stats stats_touch;
    auto st = cache::process_file(fi, cache::Mode::Touch, stats_touch,
                                  unit::warn_of(warn), sink_touch);
    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(stats_touch.total_pages, 3);
    // After -t the file must be fully resident (upstream contract). Verify
    // via a fresh query on the same fd:
    unit::RecordingSink sink_query;
    Stats stats_query;
    (void)cache::process_file(fi, cache::Mode::Query, stats_query,
                              unit::warn_of(warn), sink_query);
    EXPECT_EQ(stats_query.total_pages, 3);
    EXPECT_EQ(stats_query.total_pages_in_core, 3);   // touched => all resident
}

TEST(cache_empty_file_skipped_silently) {
    unit::TempFile f(0);   // zero bytes
    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;
    auto fi = unit::make_file_info(f.path, f.fd);
    auto st = cache::process_file(fi, cache::Mode::Query, stats,
                                  unit::warn_of(warn), sink);
    EXPECT_EQ((int)st, (int)Status::Skipped);
    EXPECT_TRUE(warn.messages.empty());      // upstream: silent for empty files
    EXPECT_TRUE(sink.file_starts.empty());
    EXPECT_EQ(stats.total_pages, 0);
}

TEST(cache_file_larger_than_max_skips_with_warning) {
    unit::TempFile f(2);
    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;
    auto fi = unit::make_file_info(f.path, f.fd);
    fi.max_file_size = static_cast<size_t>(pagesize);   // -m 1 page
    auto st = cache::process_file(fi, cache::Mode::Query, stats,
                                  unit::warn_of(warn), sink);
    EXPECT_EQ((int)st, (int)Status::Skipped);
    EXPECT_TRUE(warn.contains("too large"));
    EXPECT_EQ(stats.total_pages, 0);
    EXPECT_TRUE(sink.file_starts.empty());
}

TEST(cache_offset_beyond_eof_skips_with_warning) {
    unit::TempFile f(2);
    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;
    auto fi = unit::make_file_info(f.path, f.fd, /*offset=*/10 * pagesize);
    auto st = cache::process_file(fi, cache::Mode::Query, stats,
                                  unit::warn_of(warn), sink);
    EXPECT_EQ((int)st, (int)Status::Skipped);
    EXPECT_TRUE(warn.contains("smaller than offset"));
    EXPECT_EQ(stats.total_pages, 0);
}

TEST(cache_partial_range_counts_only_selected_pages) {
    unit::TempFile f(4);
    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;
    // -p second page only: offset = 1 page, max_len = 1 page
    auto fi = unit::make_file_info(f.path, f.fd, /*offset=*/pagesize, /*max_len=*/pagesize);
    auto st = cache::process_file(fi, cache::Mode::Query, stats,
                                  unit::warn_of(warn), sink);
    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(stats.total_pages, 1);         // only the selected page counts
    EXPECT_EQ(sink.done[0].second.first, 1);
}

TEST(cache_non_verbose_sinks_are_silent_but_stats_still_count) {
    unit::TempFile f(2);
    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;
    auto fi = unit::make_file_info(f.path, f.fd, 0, 0, /*verbose=*/false);
    auto st = cache::process_file(fi, cache::Mode::Query, stats,
                                  unit::warn_of(warn), sink);
    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(stats.total_pages, 2);
    EXPECT_TRUE(sink.file_starts.empty());   // -v off: no output calls
    EXPECT_TRUE(sink.done.empty());
    EXPECT_TRUE(sink.chart_page_counts.empty());
}

// scenario: S3 (mlock model loader) — "tell me WHY mlock failed". Pinned:
// an mlock failure surfaces as FatalError carrying the path and the errno
// text, not as a silent skip.
//
// NOTE: skipped under ASan/UBSan — ASan intercepts mlock(2) and its
// interceptor succeeds where the real syscall would fail with ENOMEM, so
// the failure path cannot be exercised in a sanitizer build (verified by
// raw-syscall comparison). The no-sanitizer CI run covers this test.
TEST(cache_lock_failure_throws_fatal_error) {
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
    unit::RecordingSink sink;
    unit::WarningCapture warn;
    Stats stats;
    auto fi = unit::make_file_info(f.path, f.fd);
    bool threw = false;
    try {
        (void)cache::process_file(fi, cache::Mode::Lock, stats,
                                  unit::warn_of(warn), sink);
    } catch (const FatalError& e) {
        threw = true;
        EXPECT_TRUE(std::string(e.what()).find("mlock:") != std::string::npos);
    }
    EXPECT_TRUE(threw);
}
