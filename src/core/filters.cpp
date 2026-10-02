// core/filters.cpp — see filters.h.
//
// A4: the class became free functions over the Options-owned lists. The
// matching logic is the same code the monolith and the FileFilter class
// used: fnmatch(3) on the basename only (basename(3) may modify its
// argument, so match on an owned NUL-terminated copy — same strdup dance
// the monolith did).

#include "filters.h"

#include <libgen.h>     // basename(3), same as upstream
#include <fnmatch.h>

#include <cstring>

namespace filters {

namespace {

constexpr size_t kMaxFilenameLength = 1024;
constexpr size_t kMaxNumberOfIgnores = 1024;
constexpr size_t kMaxNumberOfFilenameFilters = 1024;

std::string basename_of(std::string_view path) {
    std::string copy(path);
    return std::string(basename(copy.data()));
}

bool basename_matches(const std::vector<std::string>& patterns,
                      std::string_view path) {
    const std::string filename = basename_of(path);
    for (const auto& pattern : patterns) {
        if (fnmatch(pattern.c_str(), filename.c_str(), 0) == 0)
            return true;
    }
    return false;
}

} // namespace

bool is_ignored(const std::vector<std::string>& ignore, std::string_view path) {
    if (ignore.empty())
        return false;
    return basename_matches(ignore, path);
}

bool is_included(const std::vector<std::string>& include, std::string_view path) {
    if (include.empty())
        return true;   // no -I patterns: keep everything (upstream behavior)
    return basename_matches(include, path);
}

void add_ignore(std::vector<std::string>& ignore, std::string pattern) {
    if (pattern.size() > kMaxFilenameLength)
        throw FatalError("too long pattern provided to -i: " + pattern);
    if (ignore.size() >= kMaxNumberOfIgnores)
        throw FatalError("too many patterns passed to -i. Max is " +
                         std::to_string(kMaxNumberOfIgnores));
    ignore.push_back(std::move(pattern));
}

void add_include(std::vector<std::string>& include, std::string pattern) {
    if (pattern.size() > kMaxFilenameLength)
        throw FatalError("too long pattern provided to -I: " + pattern);
    if (include.size() >= kMaxNumberOfFilenameFilters)
        throw FatalError("too many patterns passed to -I. Max is " +
                         std::to_string(kMaxNumberOfFilenameFilters));
    include.push_back(std::move(pattern));
}

} // namespace filters
