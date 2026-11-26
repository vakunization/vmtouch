// tests/unit/test_platform.cpp — behavior tests for core/platform.
//
// The page math, mincore decoding, and the rlimit-raise contract are the
// library's most basic contracts.

#include "test.h"

#include "core/platform.h"
#include "core/common.h"

#include <cerrno>
#include <sys/resource.h>
#include <unistd.h>

// The CLI's parse helpers live in main.cpp; exercised here through the
// same rules they enforce (size/range parsing is contract, tested via
// the pure helpers where possible).

TEST(page_size_is_sane) {
    platform::init();
    EXPECT_EQ(pagesize, sysconf(_SC_PAGESIZE));
    EXPECT_TRUE(pagesize == 4096 || pagesize == 8192 ||
                pagesize == 16384 || pagesize == 65536);
}

TEST(aligned_p_basic) {
    platform::init();
    EXPECT_TRUE(platform::aligned_p(nullptr));                     // offset 0
    EXPECT_TRUE(platform::aligned_p(reinterpret_cast<const void*>(static_cast<uintptr_t>(pagesize))));
    EXPECT_FALSE(platform::aligned_p(reinterpret_cast<const void*>(static_cast<uintptr_t>(pagesize - 1))));
    EXPECT_FALSE(platform::aligned_p(reinterpret_cast<const void*>(1)));
}

TEST(mincore_resident_bit) {
    // mincore(2): bit 0 of the vector byte marks residency; other bits are
    // kernel-internal (mapped, dirty...) and must NOT flip the answer.
    EXPECT_TRUE(platform::is_mincore_page_resident(0x01));
    EXPECT_TRUE(platform::is_mincore_page_resident(0x03));   // resident + extra flags
    EXPECT_TRUE(platform::is_mincore_page_resident(0x81));
    EXPECT_FALSE(platform::is_mincore_page_resident(0x00));
    EXPECT_FALSE(platform::is_mincore_page_resident(0x02));  // flags but not resident
    EXPECT_FALSE(platform::is_mincore_page_resident(0xFE));
}

TEST(raise_nofile_already_at_hard_limit_is_noop) {
    // Soft limit already at the hard limit: the raise must report Raised
    // (a no-op, not a failure) and must not touch the hard limit.
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) != 0) {
        SKIP("getrlimit failed");
    }
    r.rlim_cur = r.rlim_max;
    (void)setrlimit(RLIMIT_NOFILE, &r);   // best effort; EPERM below is fine

    const auto res = platform::raise_nofile_rlimit();
    EXPECT_TRUE(res.outcome == platform::RaiseNofileResult::Outcome::Raised ||
                res.outcome == platform::RaiseNofileResult::Outcome::PermissionDenied);

    struct rlimit after;
    EXPECT_TRUE(getrlimit(RLIMIT_NOFILE, &after) == 0);
    EXPECT_TRUE(after.rlim_max == r.rlim_max);   // hard limit untouched
}

TEST(nofile_fatal_message_texts) {
    using platform::RaiseNofileResult;

    EXPECT_STREQ(platform::nofile_fatal_message(
                     RaiseNofileResult{RaiseNofileResult::Outcome::PermissionDenied, EPERM}),
                 getuid() == 0 ? "system open file limit reached"
                               : "open file limit reached and unable to increase limit. retry as root");
    EXPECT_STREQ(platform::nofile_fatal_message(
                     RaiseNofileResult{RaiseNofileResult::Outcome::GetrlimitFailed, EACCES}),
                 "increment_nofile_rlimit: getrlimit (Permission denied)");
    EXPECT_STREQ(platform::nofile_fatal_message(
                     RaiseNofileResult{RaiseNofileResult::Outcome::SetrlimitFailed, EACCES}),
                 "increment_nofile_rlimit: setrlimit (Permission denied)");
}
