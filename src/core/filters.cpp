// core/filters.cpp — see filters.h.

#include "filters.h"

#include <libgen.h>     // basename(3), same as upstream
#include <fnmatch.h>

#include <cstring>

#include "common.h"

namespace {

// Upstream caps: kept as fatal conditions (message text preserved) rather
// than silently raised — the modernization phase will revisit them.
constexpr size_t kMaxFilenameLength = 1024;
constexpr size_t kMaxNumberOfIgnores = 1024;
constexpr size_t kMaxNumberOfFilenameFilters = 1024;

// Upstream matches the basename only. basename(3) may modify its argument
// (it takes a char*), so work on an owned, NUL-terminated copy — same
// strdup/_basename dance the monolith did.
std::string basename_of(std::string_view path) {
    std::string copy(path);
    return std::string(basename(copy.data()));
}

} // namespace

void FileFilter::add_ignore(std::string pattern) {
    if (pattern.size() > kMaxFilenameLength)
        throw FatalError("too long pattern provided to -i: " + pattern);
    if (ignores_.size() >= kMaxNumberOfIgnores)
        throw FatalError("too many patterns passed to -i. Max is " +
                         std::to_string(kMaxNumberOfIgnores));
    ignores_.push_back(std::move(pattern));
}

void FileFilter::add_filename_filter(std::string pattern) {
    if (pattern.size() > kMaxFilenameLength)
        throw FatalError("too long pattern provided to -I: " + pattern);
    if (filters_.size() >= kMaxNumberOfFilenameFilters)
        throw FatalError("too many patterns passed to -I. Max is " +
                         std::to_string(kMaxNumberOfFilenameFilters));
    filters_.push_back(std::move(pattern));
}

bool FileFilter::is_ignored(std::string_view path) const {
    if (ignores_.empty())
        return false;
    return basename_matches(ignores_, path);
}

bool FileFilter::is_filename_filtered(std::string_view path) const {
    if (filters_.empty())
        return true;   // no -I patterns: keep everything (upstream behavior)
    return basename_matches(filters_, path);
}

bool FileFilter::basename_matches(const std::vector<std::string>& patterns,
                                  std::string_view path) const {
    const std::string filename = basename_of(path);
    for (const auto& pattern : patterns) {
        if (fnmatch(pattern.c_str(), filename.c_str(), 0) == 0)
            return true;
    }
    return false;
}
