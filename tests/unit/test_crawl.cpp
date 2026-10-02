// tests/unit/test_crawl.cpp — behavior tests for the filesystem traversal.
//
// Pinned behavior: upstream vmtouch's crawl semantics (already pinned
// end-to-end by tests/run.sh; here at module level for the Phase-2
// rewrite). Crawler needs a Session (per-file work) — tests observe via
// the shared sink state, session stats, and captured warnings.
//
// scenario: S1 (cache warmer with directory roots), S2 (checker over a
// test tree); traversal correctness is shared by all embedders.

#include "helpers.h"
#include "core/crawl.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// RAII temp directory (mkdtemp) that removes itself recursively on scope
// exit. Cheap rmdir-based recursion; only test-owned files inside.
struct TempDir {
    std::string path;

    TempDir() {
        char tmpl[] = "/tmp/vmtouch-crawl-XXXXXX";
        char* d = mkdtemp(tmpl);
        if (!d) { std::perror("mkdtemp"); std::abort(); }
        path = d;
    }
    ~TempDir() { remove_recursive(path); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string child(const std::string& name) const { return path + "/" + name; }

    void mkdir(const std::string& name) {
        if (::mkdir(child(name).c_str(), 0755) == -1) {
            std::perror("mkdir");
            std::abort();
        }
    }

    void mkfile(const std::string& name, int n_pages) {
        const std::string p = child(name);
        FILE* fp = std::fopen(p.c_str(), "w");
        if (!fp) { std::perror("fopen"); std::abort(); }
        // Sparse-ish: truncate to size (pages are zero-filled on read).
        if (ftruncate(::fileno(fp), static_cast<int64_t>(n_pages) * pagesize) == -1) {
            std::perror("ftruncate");
            std::abort();
        }
        std::fclose(fp);
    }

    void symlink(const std::string& target, const std::string& name) {
        // target may be absolute (loop tests) or tree-relative; join when relative.
        const std::string t = target[0] == '/' ? target : child(target);
        if (::symlink(t.c_str(), child(name).c_str()) == -1) {
            std::perror("symlink");
            std::abort();
        }
    }

    // Hardlink: ::link(target, name). Both paths are tree-relative names;
    // both get joined to the tree root (link(2) resolves oldpath relative
    // to the process CWD, which is NOT the tree).
    void hardlink(const std::string& target, const std::string& name) {
        if (::link(child(target).c_str(), child(name).c_str()) == -1) {
            std::perror("link");
            std::abort();
        }
    }

private:
    static void remove_recursive(const std::string& dir) {
        // depth-first delete; fine for small test trees
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            const std::string name = de->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = dir + "/" + name;
            struct stat sb;
            if (lstat(full.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode))
                remove_recursive(full);
            else
                ::unlink(full.c_str());
        }
        closedir(d);
        ::rmdir(dir.c_str());
    }
};

// A crawl fixture: session + crawler + shared sink state + warnings.
struct CrawlFixture {
    std::shared_ptr<unit::SharedSinkState> state =
        std::make_shared<unit::SharedSinkState>();
    unit::WarningCapture warn;
    std::unique_ptr<Session> session;
    std::unique_ptr<Crawler> crawler;

    CrawlFixture() {
        session = unit::make_session(state, warn);
        crawler = std::make_unique<Crawler>(*session);
    }
};

} // namespace

TEST(crawl_single_file_visits_session_once) {
    unit::TempFile f(1);
    CrawlFixture fx;

    fx.crawler->walk(f.path);

    EXPECT_EQ(fx.crawler->total_files(), 1);
    EXPECT_EQ(fx.crawler->total_dirs(), 0);
    EXPECT_EQ(fx.session->stats().total_pages, 1);
    EXPECT_EQ(fx.state->file_starts.size(), (size_t)1);
}

TEST(crawl_directory_nesting_counts_and_visits) {
    TempDir tree;
    tree.mkdir("a");
    tree.mkdir("a/b");
    tree.mkfile("a/f1", 1);
    tree.mkfile("a/b/f2", 2);
    tree.mkfile("top", 1);

    CrawlFixture fx;
    fx.crawler->walk(tree.path);

    EXPECT_EQ(fx.crawler->total_files(), 3);
    EXPECT_EQ(fx.crawler->total_dirs(), 3);      // tree root itself + a + a/b (root counts when walked as dir — verified against CLI)
    EXPECT_EQ(fx.session->stats().total_pages, 4);
    EXPECT_EQ(fx.state->done.size(), (size_t)3);
}

TEST(crawl_ignore_pattern_removes_files_and_dirs) {
    TempDir tree;
    tree.mkdir("keep");
    tree.mkdir("skip");
    tree.mkfile("keep/f", 1);
    tree.mkfile("skip/g", 1);
    tree.mkfile("x.tmp", 1);
    tree.mkfile("keep.log", 1);

    CrawlFixture fx;
    fx.session->filter().add_ignore("*.tmp");
    fx.session->filter().add_ignore("skip");
    fx.crawler->walk(tree.path);

    EXPECT_EQ(fx.crawler->total_files(), 2);     // keep/f and keep.log
    EXPECT_EQ(fx.session->stats().total_pages, 2);
    // Nothing ignored reached the session:
    for (const auto& d : fx.state->done)
        EXPECT_TRUE(d.first.find("skip") == std::string::npos);
}

TEST(crawl_filename_filter_keeps_only_matching) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/keep.log", 1);
    tree.mkfile("d/drop.bin", 1);

    CrawlFixture fx;
    fx.session->filter().add_filename_filter("*.log");
    fx.crawler->walk(tree.path);

    EXPECT_EQ(fx.crawler->total_files(), 1);
    EXPECT_EQ(fx.session->stats().total_pages, 1);
    EXPECT_EQ(fx.state->done[0].first, tree.child("d/keep.log"));
}

TEST(crawl_hardlink_dedup_default_once) {
    TempDir tree;
    tree.mkfile("orig", 2);
    tree.hardlink("orig", "alias");

    CrawlFixture fx;
    fx.crawler->walk(tree.path);

    EXPECT_EQ(fx.crawler->total_files(), 1);     // dedup: one inode, one visit
    EXPECT_EQ(fx.session->stats().total_pages, 2);
}

TEST(crawl_hardlink_no_dedup_with_h_flag) {
    TempDir tree;
    tree.mkfile("orig", 2);
    tree.hardlink("orig", "alias");

    // -h flips a session-owned Option: build the Session from final Options,
    // exactly as the CLI does (getopt fills Options, then Session is built).
    auto state = unit::make_shared_state();
    unit::WarningCapture warn;
    Options opts;
    opts.ignorehardlinkeduplictes = true;   // the -h flag
    auto session = unit::make_session(state, warn, opts);
    Crawler crawler(*session);

    crawler.walk(tree.path);

    EXPECT_EQ(crawler.total_files(), 2);          // both paths visited
    EXPECT_EQ(session->stats().total_pages, 4);
}

TEST(crawl_symlink_not_followed_by_default) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);
    tree.symlink(tree.child("d"), "link");

    CrawlFixture fx;
    fx.crawler->walk(tree.path);

    // the link itself is not followed: warning + only d/f visited
    EXPECT_EQ(fx.session->failures().size(), (size_t)1);
    EXPECT_EQ(fx.session->failures()[0].kind, Failure::Kind::Symlink);
    EXPECT_EQ(fx.crawler->total_files(), 1);
}

TEST(crawl_symlink_followed_with_f_flag) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);
    tree.symlink(tree.child("d"), "link");

    auto state = unit::make_shared_state();
    unit::WarningCapture warn;
    Options opts;
    opts.followsymlinks = true;   // -f
    auto session = unit::make_session(state, warn, opts);
    Crawler crawler(*session);

    crawler.walk(tree.path);

    // With plain -f the revisit of d through the link hits the hardlink
    // dedup (directories have st_nlink>=2), so d/f is processed ONCE —
    // pinned upstream behavior, verified against the CLI.
    EXPECT_EQ(crawler.total_files(), 1);
    EXPECT_EQ(session->stats().total_pages, 1);
    EXPECT_TRUE(warn.empty());
}

TEST(crawl_symlink_loop_terminates_with_warning) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);
    tree.symlink(tree.path, "d/loop");   // d/loop -> tree root

    auto state = unit::make_shared_state();
    unit::WarningCapture warn;
    Options opts;
    opts.followsymlinks = true;      // -f: loops are only reachable with -f
    opts.ignorehardlinkeduplictes = true;   // -h: dedup would shadow the loop check
    auto session = unit::make_session(state, warn, opts);
    Crawler crawler(*session);

    crawler.walk(tree.path);         // must terminate on its own

    EXPECT_EQ(session->failures().size(), (size_t)1);
    EXPECT_EQ(session->failures()[0].kind, Failure::Kind::CrawlLoop);
}

TEST(crawl_trailing_slash_normalized) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);

    CrawlFixture fx;
    fx.crawler->walk(tree.path + "/");   // trailing slash, as users type it

    EXPECT_EQ(fx.crawler->total_files(), 1);
    // no double slash in echoed paths:
    for (const auto& d : fx.state->done)
        EXPECT_TRUE(d.first.find("//") == std::string::npos);
}
