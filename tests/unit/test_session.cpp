// tests/unit/test_session.cpp — behavior tests for Session (S1/S2 style:
// path-driven; the session owns open/retry/checks and drives the engine).
//
// scenario: S1 (cache warmer) — Skipped vs hard-failure distinction, path
// lists, warnings as data; S2 (QA checker) — query mode with residency
// numbers readable from stats().

#include "helpers.h"

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
    Status st = fx.session->process_file("/tmp/vmtouch-no-such-file-evar");

    EXPECT_EQ((int)st, (int)Status::Skipped);
    EXPECT_TRUE(fx.warn.contains("unable to open"));
    EXPECT_TRUE(fx.warn.contains("/tmp/vmtouch-no-such-file-evar"));
    EXPECT_EQ(fx.session->stats().total_pages, 0);
    EXPECT_TRUE(fx.state->file_starts.empty());
}

TEST(session_process_file_end_to_end) {
    unit::TempFile f(3);
    Fixture fx;

    Status st = fx.session->process_file(f.path);

    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(fx.session->stats().total_pages, 3);
    EXPECT_EQ(fx.state->file_starts.size(), (size_t)1);
    EXPECT_EQ(fx.state->done.size(), (size_t)1);
    EXPECT_EQ(fx.state->done[0].first, f.path);   // sink saw the right path
    EXPECT_TRUE(fx.warn.messages.empty());
}

TEST(session_range_from_set_range_selects_pages) {
    unit::TempFile f(3);
    // -p second page only
    FileInfoRange r;
    r.offset = pagesize;
    r.max_len = pagesize;
    Fixture fx(r);

    Status st = fx.session->process_file(f.path);

    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(fx.session->stats().total_pages, 1);
    EXPECT_EQ(fx.state->done[0].second.first, 1);
}

TEST(session_quiet_suppresses_warnings_but_not_processing) {
    unit::TempFile f(1);
    // quiet=true, offset beyond EOF => skip-with-warning path, but -q:
    // the warning must be suppressed while Skipped is still returned.
    Fixture fx(/*range=*/{}, /*quiet=*/true);
    FileInfoRange bad;
    bad.offset = 10 * pagesize;
    fx.session->set_range(bad);

    Status st = fx.session->process_file(f.path);

    EXPECT_EQ((int)st, (int)Status::Skipped);
    EXPECT_TRUE(fx.warn.messages.empty());   // quiet: nothing reaches the sink
}

TEST(session_filter_is_accessible_for_wiring) {
    // The CLI fills a FileFilter before the Session exists and moves it in;
    // Crawler reads it through the session. Pin that plumbing exists.
    Fixture fx;
    fx.session->filter().add_ignore("*.tmp");
    EXPECT_TRUE(fx.session->filter().is_ignored("/x/y.tmp"));
}

TEST(session_second_file_accumulates_stats) {
    unit::TempFile a(2);
    unit::TempFile b(3);
    Fixture fx;

    (void)fx.session->process_file(a.path);
    (void)fx.session->process_file(b.path);

    EXPECT_EQ(fx.session->stats().total_pages, 5);
    EXPECT_EQ(fx.state->done.size(), (size_t)2);
}
