// core/filters.h — ownership of the -i (ignore) and -I (filename filter)
// pattern lists and the matching logic.
//
// Replaces four globals from the monolith (two char* arrays + two counters)
// and fixes the latent bug where is_ignored()/is_filename_filtered() took
// the list as a parameter but read the GLOBAL counters — the class owns
// both, so list and count can never disagree.
//
// Matching stays fnmatch(3) (basename only, same flags as upstream) until
// the modernization phase.

#pragma once

#include <string>
#include <string_view>
#include <vector>

class FileFilter {
public:
    // Adds one -i pattern. Throws FatalError on the same conditions the
    // monolith's parse_ignore_item() treated as fatal (pattern too long,
    // too many patterns) — message text preserved.
    void add_ignore(std::string pattern);

    // Adds one -I pattern. Same fatal conditions as add_ignore().
    void add_filename_filter(std::string pattern);

    // True if the file should be skipped: basename matches any -i pattern.
    // False when no ignore patterns are configured (upstream behavior).
    bool is_ignored(std::string_view path) const;

    // False if the file should be skipped: basename matches none of the -I
    // patterns. True (keep) when no filter patterns are configured.
    bool is_filename_filtered(std::string_view path) const;

    bool has_ignores() const { return !ignores_.empty(); }
    bool has_filters() const { return !filters_.empty(); }

private:
    bool basename_matches(const std::vector<std::string>& patterns,
                          std::string_view path) const;

    std::vector<std::string> ignores_;
    std::vector<std::string> filters_;
};
