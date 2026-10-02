// core/filters.h — -i (ignore) and -I (include/filename-filter) pattern
// semantics. A4: the FileFilter CLASS is dissolved per the approved plan —
// the pattern lists are caller policy now, living in Options
// (Options.ignore / Options.include), and matching is a free function the
// crawler inlines mid-walk (-i must prune DURING traversal: a directory
// matching -i is never descended, zero syscalls — touch-mode correctness).
//
// Matching semantics are unchanged and still pinned by tests: fnmatch(3)
// on the basename only; no -i patterns => ignore nothing; no -I patterns
// => keep everything. The fatal caps (pattern too long / too many) keep
// their exact upstream message texts and now live in the add_* functions
// below, called by the CLI during option parsing.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common.h"

namespace filters {

// True if the path should be skipped: basename matches any -i pattern.
// False when no ignore patterns are configured (upstream behavior).
bool is_ignored(const std::vector<std::string>& ignore, std::string_view path);

// False if the file should be skipped: basename matches none of the -I
// patterns. True (keep) when no include patterns are configured.
bool is_included(const std::vector<std::string>& include, std::string_view path);

// Adds one -i pattern with the upstream caps (pattern too long / too many
// patterns -> FatalError, message text preserved verbatim).
void add_ignore(std::vector<std::string>& ignore, std::string pattern);

// Adds one -I pattern; same caps and texts as add_ignore().
void add_include(std::vector<std::string>& include, std::string pattern);

} // namespace filters
