// core/cli_options.h — the CLI's OWN option state (B5).
//
// Spec rule: "the library knows nothing about the CLI." The interim CLI
// fields rode on core Options through B4; B5 moves them here, where they
// belong. Daemon/pidfile/batch/wait/output/quiet/verbose are CLI-level
// concerns (process management + output policy), never library state.

#pragma once

#include "core/common.h"

struct CliOptions {
    // Verb selection (validated: -t/-e/-l/-L are mutually exclusive).
    bool touch = false;      // -t
    bool evict = false;      // -e
    bool lock = false;       // -l
    bool lockall = false;    // -L (implies touch)

    // Output policy.
    bool quiet = false;      // -q
    int verbose = 0;         // -v (accepted; totals-only per the ratified spec)
    const char* output = nullptr;   // -o (only "kv")

    // Process management.
    bool daemon = false;     // -d
    bool wait = false;       // -w
    const char* pidfile = nullptr;  // -P

    // Input mode.
    const char* batch = nullptr;    // -b
    bool zero_delim = false;        // -0
};
