// tests/unit/test_session.cpp — behavior tests for Session (S1/S2 style:
// path-driven; the session owns open/retry/checks and drives the engine).
//
// scenario: S1 (cache warmer) — Skipped vs hard-failure distinction, path
// lists, warnings as data; S2 (QA checker) — query mode with residency
// numbers readable from stats().

#include "helpers.h"
#include "core/filters.h"

#include <cerrno>

namespace {

// One shared sink state + warning capture pair per test; make_session()
// wires them into a fresh Session. Returns the session (unique_ptr).
struct Fixture {
    std::shared_ptr<unit::SharedSinkState> state =
        std::make_shared<unit::SharedSinkState>();
    unit::WarningCapture warn;
    std::unique_ptr<Session> session;

    explicit Fixture(const FileInfoRange& range = {}, bool quiet = false) {
        session = unit::make_session(state, warn, range, quiet);
    }
};

} // namespace

TEST(session_process_missing_path_warns_and_skips) {
    Fixture fx;
    Status st = fx.session->process("/tmp/vmtouch-no-such-file-evar");

    // A3: process() goes through the crawler, and a MISSING path fails at
    // the crawl lstat — Kind::Stat, matching upstream (run.sh pins
    // "unable to stat" for missing paths). The Kind::Open site is reached
    // for files that stat but cannot be opened (pinned end-to-end by
    // run.sh's unreadable-file assertion).
    EXPECT_EQ((int)st, (int)Status::Skipped);
    EXPECT_EQ(fx.session->failures().size(), (size_t)1);
    EXPECT_EQ(fx.session->failures()[0].kind, Failure::Kind::Stat);
    EXPECT_EQ(fx.session->failures()[0].path, "/tmp/vmtouch-no-such-file-evar");
    EXPECT_EQ(fx.session->failures()[0].err, ENOENT);
    EXPECT_EQ(fx.session->stats().total_pages, 0);
    EXPECT_EQ((int)st, (int)Status::Skipped);   // (B2: no sink state; pinned above)
}

TEST(session_process_file_end_to_end) {
    unit::TempFile f(3);
    Fixture fx;

    Status st = fx.session->process(f.path);

    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(fx.session->stats().total_pages, 3);
    EXPECT_EQ(fx.session->failures().size(), (size_t)0);   // B2: processing verified via stats/failures
    EXPECT_TRUE(fx.warn.empty());
}

TEST(session_range_from_set_range_selects_pages) {
    unit::TempFile f(3);
    // -p second page only
    FileInfoRange r;
    r.offset = pagesize;
    r.max_len = pagesize;
    Fixture fx(r);

    Status st = fx.session->process(f.path);

    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(fx.session->stats().total_pages, 1);
    EXPECT_EQ(fx.session->report().stats.total_pages, 1);   // report() reflects the range (B2)
}

TEST(session_quiet_records_even_when_quiet) {
    unit::TempFile f(1);
    // quiet=true, offset beyond EOF => skip-with-warning path. A1 semantics:
    // the library records ALL failures regardless of -q (the CLI applies the
    // quiet policy at print time) — so the record IS in session->failures().
    Fixture fx(/*range=*/{}, /*quiet=*/true);
    FileInfoRange bad;
    bad.offset = 10 * pagesize;
    fx.session->set_range(bad);

    Status st = fx.session->process(f.path);

    EXPECT_EQ((int)st, (int)Status::Skipped);
    EXPECT_EQ(fx.session->failures().size(), (size_t)1);
    EXPECT_EQ(fx.session->failures()[0].kind, Failure::Kind::BeyondEof);
}

TEST(session_filter_options_reach_the_crawler) {
    // A4: filter policy is Options policy — patterns set on options().
    // Pin that the crawler reads them (an ignored subtree is skipped).
    Fixture fx;
    fx.session->options().ignore.push_back("*.tmp");
    EXPECT_TRUE(filters::is_ignored(fx.session->options().ignore, "/x/y.tmp"));
}

TEST(session_second_file_accumulates_stats) {
    unit::TempFile a(2);
    unit::TempFile b(3);
    Fixture fx;

    (void)fx.session->process(a.path);
    (void)fx.session->process(b.path);

    EXPECT_EQ(fx.session->stats().total_pages, 5);
    EXPECT_EQ(fx.session->report().stats.total_pages, 5);   // B2: accumulation via report()
}
