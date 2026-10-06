// core/crawl.h — filesystem traversal.
//
// The Crawler is a producer: it walks the filesystem and emits items
// through a caller-supplied callback. It knows nothing about Session,
// Report, Options, FileHandle, cache::* or CLI flags.
//
// Traversal policy is fixed:
//   * symlinks are NOT followed (a symlink -> Failure{Kind::Symlink});
//   * no filesystem-boundary check (cross freely);
//   * hardlink dedup ENABLED (st_nlink > 1 seen by (dev, ino) -> skip);
//   * MAX_CRAWL_DEPTH = 1024 (overflow -> FatalError);
//   * symlink loops -> Failure{Kind::CrawlLoop}.
//
// Lifetime: the caller creates one Crawler per process() call; the
// seen-inodes tree and counters live for the Crawler's lifetime, so dedup
// works ACROSS roots in one call.

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "common.h"

namespace vmt {

// One emitted item: exactly one side is meaningful.
//   error == nullptr -> a file (regular or block device) at `path`
//   error != nullptr -> a traversal-level Failure for `path`
// `path` (and *error) are valid ONLY during the callback; copy if needed.
struct CrawlItem {
    std::string_view path;
    const Failure* error;   // nullptr => file item
};

using CrawlVisitor = std::function<void(const CrawlItem&)>;

class Crawler {
public:
    Crawler();
    ~Crawler();

    Crawler(const Crawler&) = delete;
    Crawler& operator=(const Crawler&) = delete;

    // Traverses one root (file, directory, or symlink per the fixed
    // policy), emitting items to the visitor. Takes the root BY VALUE;
    // walk_impl mutates the copy in place (trailing-slash strip).
    void walk(std::string path, const CrawlVisitor& visitor);

    // Directories entered so far (incremented before opendir; a failed
    // opendir still counts). Summed over all walk() calls on this Crawler.
    int64_t dirs_seen() const { return state_.total_dirs; }

private:
    struct State {
        int64_t total_dirs = 0;
        int curr_crawl_depth = 0;
        ino_t crawl_inodes[1024];      // MAX_CRAWL_DEPTH
    };

    // Keyed on (dev, ino): tracks files already seen for hardlink dedup.
    struct DevInoHash {
        size_t operator()(const dev_and_inode& k) const noexcept {
            return std::hash<dev_t>{}(k.dev) ^ (std::hash<ino_t>{}(k.ino) << 1);
        }
    };
    std::unordered_set<dev_and_inode, DevInoHash> seen_inodes_;

    // Mutates `path` in place (trailing-slash strip) — by design, see walk().
    void walk_impl(std::string& path, const CrawlVisitor& visitor);

    State state_;
};

} // namespace vmt
