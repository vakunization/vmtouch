// tests/unit/test_filters.cpp — behavior tests for core/filters.
//
// Serves all scenarios (S1-S3): every embedder that walks a tree gets the
// same ignore/filter semantics the CLI promised. The semantics pinned here
// are upstream vmtouch's: fnmatch(3) on the basename; no -i patterns =>
// ignore nothing; no -I patterns => keep everything.

#include "test.h"

#include "core/filters.h"
#include "core/common.h"

namespace {

// Setup helper: build a FileFilter from initializer lists. If the API
// changes in Phase 2/3, this is (ideally) the only place to edit.
FileFilter make_filter(std::vector<std::string> ignores = {},
                       std::vector<std::string> filters = {}) {
    FileFilter f;
    for (auto& i : ignores) f.add_ignore(i);
    for (auto& x : filters) f.add_filename_filter(x);
    return f;
}

} // namespace

TEST(ignore_empty_list_ignores_nothing) {
    const auto f = make_filter();
    EXPECT_FALSE(f.is_ignored("/var/data/file.log"));
    EXPECT_FALSE(f.is_ignored("/etc/passwd"));
}

TEST(ignore_matches_basename) {
    const auto f = make_filter({std::string("*.tmp")});
    EXPECT_TRUE(f.is_ignored("/var/cache/junk.tmp"));
    EXPECT_TRUE(f.is_ignored("junk.tmp"));                 // bare name
    EXPECT_FALSE(f.is_ignored("/var/cache/keep.log"));
}

TEST(ignore_matches_exact_name) {
    const auto f = make_filter({std::string("node_modules")});
    EXPECT_TRUE(f.is_ignored("/srv/project/node_modules"));
    EXPECT_FALSE(f.is_ignored("/srv/project/node_modules.exe"));  // no suffix match
}

TEST(ignore_question_mark_and_brackets) {
    const auto f1 = make_filter({std::string("file?log")});
    EXPECT_TRUE(f1.is_ignored("/x/file1log"));
    EXPECT_FALSE(f1.is_ignored("/x/file12log"));

    const auto f2 = make_filter({std::string("[ab].bin")});
    EXPECT_TRUE(f2.is_ignored("/x/a.bin"));
    EXPECT_TRUE(f2.is_ignored("/x/b.bin"));
    EXPECT_FALSE(f2.is_ignored("/x/c.bin"));
}

TEST(ignore_directory_paths_use_last_component) {
    // Upstream matches the basename, so a dir pattern hits dirs by name.
    const auto f = make_filter({std::string("sub")});
    EXPECT_TRUE(f.is_ignored("/srv/data/sub"));
}

TEST(filter_empty_list_keeps_everything) {
    const auto f = make_filter({}, {});   // no -I patterns at all
    EXPECT_TRUE(f.is_filename_filtered("/var/data/anything.bin"));  // keep
    EXPECT_TRUE(f.is_filename_filtered("/var/data/anything.log"));  // keep
}

TEST(filter_keeps_only_matching) {
    const auto f = make_filter({}, {std::string("*.log")});   // -I *.log
    EXPECT_TRUE(f.is_filename_filtered("/d/keep.log"));
    EXPECT_FALSE(f.is_filename_filtered("/d/skip.tmp"));
}

TEST(filter_multiple_patterns_either_matches) {
    const auto f = make_filter({}, {std::string("*.log"), std::string("*.tmp")});
    EXPECT_TRUE(f.is_filename_filtered("/d/a.log"));
    EXPECT_TRUE(f.is_filename_filtered("/d/a.tmp"));
    EXPECT_FALSE(f.is_filename_filtered("/d/a.bin"));
}

TEST(both_lists_independent) {
    const auto f = make_filter({std::string("*.tmp")}, {std::string("*.log")});
    // -i removes tmp files; -I keeps only log files; both applied by caller.
    EXPECT_TRUE(f.is_ignored("/d/x.tmp"));
    EXPECT_FALSE(f.is_ignored("/d/x.log"));
    EXPECT_TRUE(f.is_filename_filtered("/d/x.log"));
    EXPECT_FALSE(f.is_filename_filtered("/d/x.tmp"));   // filtered AND ignored
}

TEST(too_long_pattern_throws_fatal) {
    FileFilter f;
    std::string long_pattern(2000, 'x');
    bool threw = false;
    try {
        f.add_ignore(long_pattern);
    } catch (const FatalError& e) {
        threw = true;
        EXPECT_TRUE(std::string(e.what()).find("too long pattern provided to -i") != std::string::npos);
    }
    EXPECT_TRUE(threw);
}

TEST(too_long_filter_pattern_throws_fatal) {
    FileFilter f;
    std::string long_pattern(2000, 'y');
    EXPECT_THROWS_AS(f.add_filename_filter(long_pattern), FatalError);
}
