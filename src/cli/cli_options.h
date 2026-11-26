// cli/cli_options.h — the CLI's own option state.
//
// Daemon/pidfile/batch/wait/output/quiet/verbose are CLI-level concerns
// (process management + output policy), never library state.

#pragma once

#include <string_view>

inline constexpr std::string_view VMTOUCH_VERSION = "1.3.1";

struct CliOptions {
    // Lock modes imply touching; eviction cannot be combined with touching
    // or locking, and the two lock modes cannot be combined.
    bool touch = false;      // -t
    bool evict = false;      // -e
    bool lock = false;       // -l
    bool lockall = false;    // -L (implies touch)

    // Output policy.
    bool quiet = false;      // -q
    int verbose = 0;         // -v (accepted; totals-only)
    std::string_view output;         // -o (only "kv")

    // Process management.
    bool daemon = false;     // -d
    bool wait = false;       // -w
    std::string_view pidfile;        // -P

    // Input mode.
    std::string_view batch;         // -b
    bool zero_delim = false;        // -0
};
