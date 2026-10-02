// tests/unit/test_filters.cpp — behavior tests for the -i/-I pattern
// semantics (A4: free functions over the Options-owned lists; the
// FileFilter class is gone but every assertion below is unchanged — the
// semantics are upstream vmtouch's: fnmatch(3) on the basename; no -i
// patterns => ignore nothing; no -I patterns => keep everything).
//
// Serves all scenarios (S1-S3): every embedder that walks a tree gets the
// same ignore/filter semantics the CLI promised.

#include "test.h"

#include "core/filters.h"
#include "core/common.h"

namespace {

using List = std::vector<std::string>;

List make_list(std::vector<std::string> patterns = {}) {
    return List(std::make_move_iterator(patterns.begin()),
                std::make_move_iterator(patterns.end()));
}

} // namespace

TEST(ignore_empty_list_ignores_nothing) {
    const auto f = make_list();
    EXPECT_FALSE(filters::is_ignored(f, "/var/data/file.log"));
    EXPECT_FALSE(filters::is_ignored(f, "/etc/passwd"));
}

TEST(ignore_matches_basename) {
    const auto f = make_list({std::string("*.tmp")});
    EXPECT_TRUE(filters::is_ignored(f, "/var/cache/junk.tmp"));
    EXPECT_TRUE(filters::is_ignored(f, "junk.tmp"));                 // bare name
    EXPECT_FALSE(filters::is_ignored(f, "/var/cache/keep.log"));
}

TEST(ignore_matches_exact_name) {
    const auto f = make_list({std::string("node_modules")});
    EXPECT_TRUE(filters::is_ignored(f, "/srv/project/node_modules"));
    EXPECT_FALSE(filters::is_ignored(f, "/srv/project/node_modules.exe"));  // no suffix match
}

TEST(ignore_question_mark_and_brackets) {
    const auto f1 = make_list({std::string("file?log")});
    EXPECT_TRUE(filters::is_ignored(f1, "/x/file1log"));
    EXPECT_FALSE(filters::is_ignored(f1, "/x/file12log"));

    const auto f2 = make_list({std::string("[ab].bin")});
    EXPECT_TRUE(filters::is_ignored(f2, "/x/a.bin"));
    EXPECT_TRUE(filters::is_ignored(f2, "/x/b.bin"));
    EXPECT_FALSE(filters::is_ignored(f2, "/x/c.bin"));
}

TEST(ignore_directory_paths_use_last_component) {
    // Upstream matches the basename, so a dir pattern hits dirs by name.
    const auto f = make_list({std::string("sub")});
    EXPECT_TRUE(filters::is_ignored(f, "/srv/data/sub"));
}

TEST(filter_empty_list_keeps_everything) {
    const auto f = make_list();   // no -I patterns at all
    EXPECT_TRUE(filters::is_included(f, "/var/data/anything.bin"));  // keep
    EXPECT_TRUE(filters::is_included(f, "/var/data/anything.log"));  // keep
}

TEST(filter_keeps_only_matching) {
    const auto f = make_list({std::string("*.log")});   // -I *.log
    EXPECT_TRUE(filters::is_included(f, "/d/keep.log"));
    EXPECT_FALSE(filters::is_included(f, "/d/skip.tmp"));
}

TEST(filter_multiple_patterns_either_matches) {
    const auto f = make_list({std::string("*.log"), std::string("*.tmp")});
    EXPECT_TRUE(filters::is_included(f, "/d/a.log"));
    EXPECT_TRUE(filters::is_included(f, "/d/a.tmp"));
    EXPECT_FALSE(filters::is_included(f, "/d/a.bin"));
}

TEST(both_lists_independent) {
    const auto ig = make_list({std::string("*.tmp")});
    const auto inc = make_list({std::string("*.log")});
    // -i removes tmp files; -I keeps only log files; both applied by caller.
    EXPECT_TRUE(filters::is_ignored(ig, "/d/x.tmp"));
    EXPECT_FALSE(filters::is_ignored(ig, "/d/x.log"));
    EXPECT_TRUE(filters::is_included(inc, "/d/x.log"));
    EXPECT_FALSE(filters::is_included(inc, "/d/x.tmp"));   // filtered AND ignored
}

TEST(too_long_pattern_throws_fatal) {
    List f;
    std::string long_pattern(2000, 'x');
    bool threw = false;
    try {
        filters::add_ignore(f, long_pattern);
    } catch (const FatalError& e) {
        threw = true;
        EXPECT_TRUE(std::string(e.what()).find("too long pattern provided to -i") != std::string::npos);
    }
    EXPECT_TRUE(threw);
}

TEST(too_long_filter_pattern_throws_fatal) {
    List f;
    std::string long_pattern(2000, 'y');
    EXPECT_THROWS_AS(filters::add_include(f, long_pattern), FatalError);
}
