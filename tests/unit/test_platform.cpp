// tests/unit/test_platform.cpp — behavior tests for core/platform.
//
// The page math, mincore decoding, and the rlimit-raise contract are the
// library's most basic contracts.

#include "test.h"

#include "core/platform.h"
#include "core/common.h"

#include <cerrno>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

// The CLI's parse helpers live in main.cpp; exercised here through the
// same rules they enforce (size/range parsing is contract, tested via
// the pure helpers where possible).

TEST(page_size_is_sane) {
    EXPECT_EQ(platform::page_size(), sysconf(_SC_PAGESIZE));
    EXPECT_TRUE(platform::page_size() > 0);
}

TEST(aligned_p_basic) {
    const auto page_size = platform::page_size();
    EXPECT_TRUE(platform::aligned_p(nullptr));                     // offset 0
    EXPECT_TRUE(platform::aligned_p(reinterpret_cast<const void*>(static_cast<uintptr_t>(page_size))));
    EXPECT_FALSE(platform::aligned_p(reinterpret_cast<const void*>(static_cast<uintptr_t>(page_size - 1))));
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

TEST(raise_nofile_reports_actual_progress_and_preserves_hard_limit) {
    // Don't change resource limits in the unit runner itself.
    using Outcome = platform::RaiseNofileResult::Outcome;
    const pid_t child = ::fork();
    EXPECT_TRUE(child >= 0);
    if (child < 0)
        return;
    if (child == 0) {
        const struct rlimit before{16, 32};
        if (::setrlimit(RLIMIT_NOFILE, &before) != 0)
            _exit(2);
        if (platform::raise_nofile_rlimit().outcome != Outcome::Raised)
            _exit(3);
        struct rlimit after;
        if (::getrlimit(RLIMIT_NOFILE, &after) != 0 ||
            after.rlim_cur != before.rlim_max || after.rlim_max != before.rlim_max)
            _exit(4);
        if (platform::raise_nofile_rlimit().outcome != Outcome::AlreadyAtLimit)
            _exit(5);
        _exit(0);
    }
    int status = 0;
    EXPECT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
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
