// tests/unit/test_platform.cpp — behavior tests for core/platform.
//
// Scenario lens: these helpers are used by every embedder (S1-S3); the
// page math and mincore decoding are the library's most basic contracts.

#include "test.h"

#include "core/platform.h"
#include "core/common.h"

#include <unistd.h>

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

TEST(now_seconds_is_monotonic_enough) {
    const double a = platform::now_seconds();
    const double b = platform::now_seconds();
    EXPECT_TRUE(b >= a);                       // does not go backwards
    EXPECT_TRUE(b - a < 1.0);                  // two calls take < 1s
    EXPECT_TRUE(a > 1'700'000'000.0);          // plausible epoch (>= Nov 2023)
}
