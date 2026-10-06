// tests/unit/test_platform.cpp — behavior tests for core/platform.
//
// The page math, mincore decoding, and version comparison are the
// library's most basic contracts.

#include "test.h"

#include "core/platform.h"
#include "core/common.h"

#include <unistd.h>

// The CLI's parse helpers live in main.cpp; exercised here through the
// same rules they enforce (size/range parsing is contract, tested via
// the KernelVersion and pure helpers where possible).

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

TEST(kernel_version_parse_and_compare) {
    using platform::KernelVersion;
    using platform::parse_kernel_version;

    const auto v = parse_kernel_version("6.9.4-arch1");
    EXPECT_EQ(v.major, 6);
    EXPECT_EQ(v.minor, 9);

    const auto plain = parse_kernel_version("5.2");
    EXPECT_EQ(plain.major, 5);
    EXPECT_EQ(plain.minor, 2);

    const auto single = parse_kernel_version("4");
    EXPECT_EQ(single.major, 4);
    EXPECT_EQ(single.minor, 0);

    const auto garbage = parse_kernel_version("not-a-version");
    EXPECT_EQ(garbage.major, 0);
    EXPECT_EQ(garbage.minor, 0);

    using KV = platform::KernelVersion;
    constexpr KV v4_19{4, 19}, v5_1{5, 1}, v5_2{5, 2}, v5_9{5, 9}, v6_0{6, 0}, v6_1{6, 1};

    // Ordering: the 6.x regression case that broke the old hand-rolled
    // comparison (ver[0] < 5 || ver[1] < 2 misread 6.1 as "old").
    EXPECT_TRUE(v4_19 < v5_2);
    EXPECT_TRUE(v5_1 < v5_2);
    EXPECT_FALSE(v5_2 < v5_2);
    EXPECT_TRUE(v5_2 >= v5_2);
    EXPECT_TRUE(v6_1 >= v5_2);   // the fix
    EXPECT_TRUE(v6_0 >= v5_2);
    EXPECT_TRUE(v5_9 >= v5_2);
    EXPECT_FALSE(v5_1 >= v5_2);
}
