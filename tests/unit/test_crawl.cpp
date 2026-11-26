// tests/unit/test_crawl.cpp — behavior tests for the traversal producer
// (B3 surface: Crawler::walk(root, visitor) + CrawlItem).
//
// The Crawler is now a pure producer: it emits items (file path or a
// traversal Failure) and knows nothing about Session/Report/cache. The
// behavior pinned here is upstream vmtouch's traversal semantics with the
// RATIFIED fixed policy: symlinks not followed, no fs-boundary check,
// hardlink dedup ON, depth 1024, loops -> CrawlLoop Failure.
//
// scenario: S1 (cache warmer with directory roots), S2 (checker over a
// test tree); traversal correctness is shared by all embedders.

#include "test.h"

#include "core/crawl.h"
#include "helpers.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <string>
#include <vector>

namespace {

using vmt::Crawler;
using vmt::CrawlItem;

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
        if (ftruncate(::fileno(fp), static_cast<int64_t>(n_pages) * pagesize) == -1) {
            std::perror("ftruncate");
            std::abort();
        }
        std::fclose(fp);
    }

    void symlink(const std::string& target, const std::string& name) {
        const std::string t = target[0] == '/' ? target : child(target);
        if (::symlink(t.c_str(), child(name).c_str()) == -1) {
            std::perror("symlink");
            std::abort();
        }
    }

    void hardlink(const std::string& target, const std::string& name) {
        if (::link(child(target).c_str(), child(name).c_str()) == -1) {
            std::perror("link");
            std::abort();
        }
    }

private:
    static void remove_recursive(const std::string& dir) {
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

// Collects what the visitor saw. Paths are copied (string_view is valid
// only during the callback — spec2 §5.9).
struct Collected {
    std::vector<std::string> files;
    std::vector<Failure> errors;

    vmt::CrawlVisitor visitor() {
        return [this](const CrawlItem& item) {
            if (item.error != nullptr)
                errors.push_back(*item.error);
            else
                files.emplace_back(item.path);
        };
    }
};

bool has_file(const Collected& c, const std::string& path) {
    for (const auto& f : c.files)
        if (f == path) return true;
    return false;
}

} // namespace

TEST(crawl_single_file_emits_one_item) {
    unit::TempFile f(1);
    Crawler crawler;
    Collected c;

    crawler.walk(f.path, c.visitor());

    EXPECT_EQ(c.files.size(), (size_t)1);
    EXPECT_EQ(c.files[0], f.path);
    EXPECT_EQ(c.errors.size(), (size_t)0);
    EXPECT_EQ(crawler.dirs_seen(), 0);
}

TEST(crawl_directory_nesting_counts_files_and_dirs) {
    TempDir tree;
    tree.mkdir("a");
    tree.mkdir("a/b");
    tree.mkfile("a/f1", 1);
    tree.mkfile("a/b/f2", 2);
    tree.mkfile("top", 1);

    Crawler crawler;
    Collected c;
    crawler.walk(tree.path, c.visitor());

    EXPECT_EQ(c.files.size(), (size_t)3);      // a/f1, a/b/f2, top
    EXPECT_EQ(crawler.dirs_seen(), 3);         // tree root + a + a/b
    EXPECT_EQ(c.errors.size(), (size_t)0);
    EXPECT_TRUE(has_file(c, tree.child("a/f1")));
    EXPECT_TRUE(has_file(c, tree.child("a/b/f2")));
}

TEST(crawl_missing_root_emits_stat_error) {
    Crawler crawler;
    Collected c;
    crawler.walk("/tmp/vmtouch-no-such-root-evar", c.visitor());

    EXPECT_EQ(c.files.size(), (size_t)0);
    EXPECT_EQ(c.errors.size(), (size_t)1);
    EXPECT_EQ((int)c.errors[0].kind, (int)Failure::Kind::Stat);
    EXPECT_EQ(c.errors[0].err, ENOENT);
}

TEST(crawl_throwing_visitor_closes_all_open_directories) {
    TempDir tree;
    tree.mkdir("nested");
    tree.mkfile("nested/file", 1);
    unit::TempFile probe(0);

    // dup returns the lowest free descriptor. Probe both slots used by
    // the outer and nested directory streams.
    const auto free_slots = [&] {
        const std::array slots{::dup(probe.fd), ::dup(probe.fd)};
        for (const int fd : slots) {
            EXPECT_TRUE(fd >= 0);
            if (fd >= 0)
                ::close(fd);
        }
        return slots;
    };
    const auto before = free_slots();

    struct VisitorError {};
    Crawler crawler;
    EXPECT_THROWS_AS(crawler.walk(tree.path, [](const CrawlItem&) {
        throw VisitorError{};
    }), VisitorError);

    const auto after = free_slots();
    EXPECT_EQ(after[0], before[0]);
    EXPECT_EQ(after[1], before[1]);

    TempDir next;
    next.mkfile("file", 1);
    Collected items;
    crawler.walk(next.path, items.visitor());
    EXPECT_TRUE(has_file(items, next.child("file")));
    EXPECT_TRUE(items.errors.empty());
}

TEST(crawl_symlink_not_followed) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);
    tree.symlink(tree.child("d"), "link");

    Crawler crawler;
    Collected c;
    crawler.walk(tree.path, c.visitor());

    // Fixed policy: the link is not followed -> Symlink error, only d/f emitted.
    EXPECT_EQ(c.errors.size(), (size_t)1);
    EXPECT_EQ((int)c.errors[0].kind, (int)Failure::Kind::Symlink);
    EXPECT_EQ(c.files.size(), (size_t)1);
    EXPECT_TRUE(has_file(c, tree.child("d/f")));
}

TEST(crawl_symlink_with_trailing_separators_is_not_followed) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);
    tree.symlink("d", "link");

    Crawler crawler;
    Collected c;
    crawler.walk(tree.child("link") + "///", c.visitor());

    EXPECT_TRUE(c.files.empty());
    EXPECT_EQ(c.errors.size(), (size_t)1);
    if (!c.errors.empty())
        EXPECT_EQ(c.errors[0].kind, Failure::Kind::Symlink);
    EXPECT_EQ(crawler.dirs_seen(), 0);
}

TEST(crawl_hardlink_dedup_default_once) {
    TempDir tree;
    tree.mkfile("orig", 2);
    tree.hardlink("orig", "alias");

    Crawler crawler;
    Collected c;
    crawler.walk(tree.path, c.visitor());

    // Dedup ON (fixed policy): one inode -> one emitted file.
    EXPECT_EQ(c.files.size(), (size_t)1);
    EXPECT_EQ(c.errors.size(), (size_t)0);
}

TEST(crawl_hardlink_dedup_across_walks_on_one_crawler) {
    // B3: state lives for the CRAWLER's lifetime, so dedup holds ACROSS
    // roots walked through the same instance (run.sh pins "hl hl2 -> Files: 1").
    unit::TempFile f(1);
    char alias[] = "/tmp/vmtouch-crawl-alias-XXXXXX";
    const int afd = mkstemp(alias);
    EXPECT_TRUE(afd != -1);
    ::close(afd);
    ::unlink(alias);                                  // free the name
    EXPECT_EQ(::link(f.path.c_str(), alias), 0);      // now a hardlink to f
    // Raise nlink by linking again under another name is unnecessary: the
    // two names already share one inode => st_nlink == 2.
    struct stat sb;
    EXPECT_EQ(::stat(f.path.c_str(), &sb), 0);
    EXPECT_TRUE(sb.st_nlink >= 2);

    Crawler crawler;
    Collected c;
    crawler.walk(f.path, c.visitor());     // first root: emitted
    crawler.walk(alias, c.visitor());      // second root, same inode: deduped

    EXPECT_EQ(c.files.size(), (size_t)1);   // dedup across walks
    ::unlink(alias);
}

TEST(crawl_symlink_loop_emits_loop_error_and_terminates) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);
    tree.symlink(tree.path, "d/loop");   // d/loop -> tree root

    // Fixed policy: symlinks are never followed, so the loop is not
    // traversed at all; the link itself yields a Symlink error and the
    // walk terminates. (The inode-stack loop check remains in the code as
    // defense in depth — spec2 §5.7 — but is unreachable without -f.)
    Crawler crawler;
    Collected c;
    crawler.walk(tree.path, c.visitor());

    bool saw_symlink = false;
    for (const auto& e : c.errors)
        if (e.kind == Failure::Kind::Symlink) saw_symlink = true;
    EXPECT_TRUE(saw_symlink);
    EXPECT_EQ(c.files.size(), (size_t)1);      // d/f only
}

TEST(crawl_trailing_slash_normalized) {
    TempDir tree;
    tree.mkdir("d");
    tree.mkfile("d/f", 1);

    Crawler crawler;
    Collected c;
    crawler.walk(tree.path + "/", c.visitor());

    EXPECT_EQ(c.files.size(), (size_t)1);
    EXPECT_TRUE(has_file(c, tree.child("d/f")));
    // No double slash in emitted paths (trailing slash stripped).
    for (const auto& f : c.files)
        EXPECT_TRUE(f.find("//") == std::string::npos);
}

TEST(crawl_not_regular_emits_error) {
    TempDir tree;
    if (::mkfifo(tree.child("pipe").c_str(), 0644) == -1) {
        SKIP("mkfifo failed");
    }

    Crawler crawler;
    Collected c;
    crawler.walk(tree.path, c.visitor());

    EXPECT_EQ(c.files.size(), (size_t)0);
    EXPECT_EQ(c.errors.size(), (size_t)1);
    EXPECT_EQ((int)c.errors[0].kind, (int)Failure::Kind::NotRegular);
}
