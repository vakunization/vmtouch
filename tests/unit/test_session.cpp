// tests/unit/test_session.cpp — behavior tests for Session (B4 surface:
// process(span)/process_fd/get_report, Report, Status::Fatal).
//
// scenario: S1 (cache warmer) — path lists, failed/good distinction,
// warnings as data; S2 (QA checker) — query mode, residency numbers;
// S3 (mlock loader) — caller-owned fd, WHY-it-failed.

#include "helpers.h"
#include "core/session.h"

#include <cerrno>
#include <fstream>
#include <memory>
#include <sys/resource.h>
#include <sys/stat.h>

namespace {

struct Fixture {
    std::unique_ptr<vmt::Session> session;

    explicit Fixture(const FileInfoRange& range = {})
        : session(std::make_unique<vmt::Session>(
              Options{.range = range, .collect_failures = true})) {}

    std::span<const std::string> one(const std::string& p) {
        roots_.assign(1, p);
        return std::span<const std::string>(roots_);
    }
    std::vector<std::string> roots_;
};

std::span<const std::string> span_of(const std::vector<std::string>& v) {
    return std::span<const std::string>(v);
}

struct DeepTree {
    char root[sizeof("/tmp/vmtouch-depth-XXXXXX")] = "/tmp/vmtouch-depth-XXXXXX";
    std::string leaf;

    bool make(size_t dirs) {
        if (!::mkdtemp(root))
            return false;
        leaf = root;
        for (size_t i = 1; i < dirs; ++i) {
            const std::string next = leaf + "/d";
            if (::mkdir(next.c_str(), 0700) != 0)
                return false;
            leaf = next;
        }
        return true;
    }

    ~DeepTree() {
        while (leaf.size() > std::char_traits<char>::length(root)) {
            ::rmdir(leaf.c_str());
            leaf.resize(leaf.size() - 2);
        }
        if (!leaf.empty())
            ::rmdir(root);
    }
};

} // namespace

TEST(session_process_missing_path_records_stat_and_fails) {
    Fixture fx;
    const std::vector<std::string> roots = {"/tmp/vmtouch-no-such-file-evar"};

    Status st = fx.session->process(span_of(roots));

    EXPECT_EQ((int)st, (int)Status::Ok);   // per-file problem; not fatal
    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.total_files, 0);           // nothing discovered
    EXPECT_EQ(r.failed, 1);                // the root itself failed
    EXPECT_EQ(r.failures.size(), (size_t)1);
    EXPECT_EQ((int)r.failures[0].kind, (int)Failure::Kind::Stat);
    EXPECT_EQ(r.failures[0].err, ENOENT);
    EXPECT_EQ(r.failures[0].path, "/tmp/vmtouch-no-such-file-evar");
}

TEST(session_process_file_end_to_end) {
    unit::TempFile f(3);
    Fixture fx;

    Status st = fx.session->process(fx.one(f.path));

    EXPECT_EQ((int)st, (int)Status::Ok);
    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.total_files, 1);
    EXPECT_EQ(r.failed, 0);
    EXPECT_EQ(r.total_pages, 3);
    EXPECT_EQ(r.failures.size(), (size_t)0);
    EXPECT_TRUE(r.elapsed_seconds >= 0.0);
}

TEST(session_single_call_multiple_roots_one_report) {
    unit::TempFile a(2);
    unit::TempFile b(3);
    Fixture fx;
    const std::vector<std::string> roots = {a.path, b.path};

    (void)fx.session->process(span_of(roots));

    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.total_files, 2);
    EXPECT_EQ(r.total_pages, 5);
    EXPECT_EQ(r.failed, 0);
}

TEST(session_report_resets_between_calls) {
    unit::TempFile a(2);
    unit::TempFile b(3);
    Fixture fx;

    (void)fx.session->process(fx.one(a.path));
    EXPECT_EQ(fx.session->get_report().total_pages, 2);

    // Second call: ONE call = ONE Report (reset at entry, spec2 §2.5).
    (void)fx.session->process(fx.one(b.path));
    const vmt::Session::Report& r2 = fx.session->get_report();
    EXPECT_EQ(r2.total_files, 1);
    EXPECT_EQ(r2.total_pages, 3);   // not 5 — the Report was reset
}

TEST(session_hardlink_dedup_resets_between_calls) {
    unit::TempFile original(1);
    unit::TempFile alias(0);
    EXPECT_EQ(::unlink(alias.path.c_str()), 0);
    EXPECT_EQ(::link(original.path.c_str(), alias.path.c_str()), 0);
    const std::vector<std::string> roots{original.path, alias.path};
    Fixture fx;

    EXPECT_EQ(fx.session->process(roots), Status::Ok);
    EXPECT_EQ(fx.session->get_report().total_files, 1);
    EXPECT_EQ(fx.session->process(roots), Status::Ok);
    EXPECT_EQ(fx.session->get_report().total_files, 1);
    EXPECT_EQ(fx.session->get_report().total_pages, 1);
}

TEST(session_fatal_keeps_partial_report_without_failure_collection) {
    constexpr size_t max_depth = 1024;
    struct rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur < max_depth + 16)
        SKIP("soft descriptor limit too low to reach the traversal depth limit");
    DeepTree tree;
    const bool made_tree = tree.make(max_depth + 1);
    EXPECT_TRUE(made_tree);
    if (!made_tree)
        return;

    unit::TempFile file(1);
    vmt::Session session(Options{.collect_failures = false});
    const std::vector<std::string> roots{file.path, file.path + "/missing", tree.root};
    EXPECT_EQ(session.process(roots), Status::Fatal);
    const auto& report = session.get_report();
    EXPECT_EQ(report.total_files, 1);
    EXPECT_EQ(report.failed, 1);
    EXPECT_EQ(report.total_pages, 1);
    EXPECT_EQ(report.total_dirs, static_cast<int64_t>(max_depth));
    EXPECT_EQ(report.failures.size(), size_t{1});
    if (!report.failures.empty()) {
        EXPECT_EQ(report.failures[0].kind, Failure::Kind::Fatal);
        EXPECT_TRUE(report.failures[0].path.starts_with("maximum directory crawl depth reached:"));
    }
}

TEST(session_range_selects_pages) {
    unit::TempFile f(3);
    FileInfoRange r;
    r.offset = platform::page_size();
    r.max_len = platform::page_size();
    Fixture fx(r);

    (void)fx.session->process(fx.one(f.path));

    EXPECT_EQ(fx.session->get_report().total_pages, 1);
}

TEST(session_empty_file_counted_not_failed) {
    unit::TempFile f(0);
    Fixture fx;

    (void)fx.session->process(fx.one(f.path));

    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.total_files, 1);   // discovered
    EXPECT_EQ(r.failed, 0);        // empty is NOT a failure (spec2 §7.13)
    EXPECT_EQ(r.total_pages, 0);
    EXPECT_EQ(r.failures.size(), (size_t)0);
}

TEST(session_collect_failures_false_still_counts_failed) {
    vmt::Session session(Options{.collect_failures = false});

    const std::vector<std::string> roots = {"/tmp/vmtouch-no-such-file-evar"};
    (void)session.process(span_of(roots));

    const vmt::Session::Report& r = session.get_report();
    EXPECT_EQ(r.failed, 1);                 // counted regardless
    EXPECT_EQ(r.failures.size(), (size_t)0);  // but not collected
}

// ---------------------------------------------------------------------------
// process_fd (S3) — B4 owner amendment: EXACTLY the same behavior as
// process(), only the input differs (caller-owned fd, no path).
// ---------------------------------------------------------------------------

TEST(session_process_fd_ok_and_report) {
    unit::TempFile f(3);
    Fixture fx;

    Status st = fx.session->process_fd(f.fd, 0, 0);

    EXPECT_EQ((int)st, (int)Status::Ok);
    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.total_pages, 3);                 // Report semantics identical to process()
    EXPECT_EQ(r.failed, 0);
    EXPECT_EQ(r.total_files, 0);                 // no path discovery for a raw fd
    EXPECT_TRUE(fx.session->get_report().failures.empty());
}

TEST(session_process_fd_does_not_close_borrowed_descriptor) {
    unit::TempFile f(1);
    {
        Fixture fx;
        EXPECT_EQ(fx.session->process_fd(f.fd, 0, 0), Status::Ok);
        EXPECT_TRUE(::fcntl(f.fd, F_GETFD) >= 0);
    }
    EXPECT_TRUE(::fcntl(f.fd, F_GETFD) >= 0);
}

TEST(session_retains_lock_mapping_until_destruction) {
#if defined(__linux__)
    if (!std::ifstream("/proc/self/maps"))
        SKIP("/proc/self/maps is unavailable");
    unit::TempFile f(1);
    const auto is_mapped = [&] {
        std::ifstream maps("/proc/self/maps");
        for (std::string line; std::getline(maps, line);) {
            if (line.ends_with(f.path))
                return true;
        }
        return false;
    };
    EXPECT_FALSE(is_mapped());
    {
        vmt::Session session(Options{.verb = Options::Verb::Lock, .collect_failures = true});
        EXPECT_EQ(session.process_fd(f.fd, 0, 0), Status::Ok);
        EXPECT_TRUE(session.get_report().failures.empty());
        EXPECT_EQ(session.get_report().total_pages, 1);
        EXPECT_TRUE(is_mapped());
        EXPECT_EQ(session.process({}), Status::Ok);
        EXPECT_EQ(session.get_report().total_pages, 0);
        EXPECT_TRUE(is_mapped());
    }
    EXPECT_FALSE(is_mapped());
#else
    SKIP("checking file mappings requires Linux /proc/self/maps");
#endif
}

TEST(session_process_fd_resets_report_like_process) {
    unit::TempFile a(2);
    unit::TempFile b(3);
    Fixture fx;

    (void)fx.session->process_fd(a.fd, 0, 0);
    EXPECT_EQ(fx.session->get_report().total_pages, 2);

    (void)fx.session->process_fd(b.fd, 0, 0);
    EXPECT_EQ(fx.session->get_report().total_pages, 3);   // reset, not accumulated
}

TEST(session_process_fd_range) {
    unit::TempFile f(4);
    Fixture fx;

    const auto page_size = platform::page_size();
    (void)fx.session->process_fd(f.fd, page_size, page_size);   // second page only

    EXPECT_EQ(fx.session->get_report().total_pages, 1);
}

TEST(session_process_fd_bad_fd_records_open_failure) {
    Fixture fx;

    Status st = fx.session->process_fd(-1, 0, 0);

    EXPECT_EQ((int)st, (int)Status::Ok);   // not fatal
    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.failed, 1);
    EXPECT_EQ(r.failures.size(), (size_t)1);
    EXPECT_EQ((int)r.failures[0].kind, (int)Failure::Kind::Open);
    EXPECT_EQ(r.failures[0].err, EBADF);
}

TEST(session_process_fd_beyond_eof_records_beyond_eof) {
    unit::TempFile f(1);
    Fixture fx;

    (void)fx.session->process_fd(f.fd, 10 * platform::page_size(), 0);

    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.failed, 1);
    EXPECT_EQ((int)r.failures[0].kind, (int)Failure::Kind::BeyondEof);
}

TEST(session_invalid_ranges_are_per_file_failures) {
    unit::TempFile f(1);
    Fixture fx;
    const FileInfoRange invalid[] = {{-1, 0}, {0, -1}, {1, 0}};
    for (const auto& range : invalid) {
        EXPECT_EQ(fx.session->process_fd(f.fd, range.offset, range.max_len), Status::Ok);
        const auto& r = fx.session->get_report();
        EXPECT_EQ(r.total_pages, 0);
        EXPECT_EQ(r.failed, 1);
        EXPECT_EQ(r.failures.size(), size_t{1});
        if (!r.failures.empty()) {
            EXPECT_EQ(r.failures[0].kind, Failure::Kind::Map);
            EXPECT_EQ(r.failures[0].err, EINVAL);
            EXPECT_EQ(r.failures[0].offset, range.offset);
        }
    }

    Fixture by_path({0, -1});
    EXPECT_EQ(by_path.session->process(by_path.one(f.path)), Status::Ok);
    EXPECT_EQ(by_path.session->get_report().failed, 1);
}

TEST(session_get_report_zeroed_before_any_call) {
    Fixture fx;
    const vmt::Session::Report& r = fx.session->get_report();
    EXPECT_EQ(r.total_files, 0);
    EXPECT_EQ(r.failed, 0);
    EXPECT_EQ(r.total_pages, 0);
    EXPECT_EQ(r.failures.size(), (size_t)0);
}

TEST(session_empty_path_list_is_valid) {
    Fixture fx;
    const std::vector<std::string> none;

    Status st = fx.session->process(span_of(none));

    EXPECT_EQ((int)st, (int)Status::Ok);
    EXPECT_EQ(fx.session->get_report().total_files, 0);
    EXPECT_EQ(fx.session->get_report().failed, 0);
}
