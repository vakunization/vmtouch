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
//   * repeated directories on the active path can yield Kind::CrawlLoop;
//     encountered symlinks are rejected before directory traversal.
//
// Lifetime: the caller creates one Crawler per process() call; the
// seen-inodes set and counters live for the Crawler's lifetime, so dedup
// works ACROSS roots in one call.

#pragma once

#include <cstddef>       // size_t
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <sys/types.h>   // dev_t, ino_t

#include "common.h"

namespace vmt {

// Identity of an already-seen file (hardlink dedup). Co-located with the
// only consumer (the Crawler's seen-inode set).
struct dev_and_inode {
    dev_t dev;
    ino_t ino;
    bool operator==(const dev_and_inode&) const = default;
};

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
    Crawler() = default;
    ~Crawler() = default;

    Crawler(const Crawler&) = delete;
    Crawler& operator=(const Crawler&) = delete;

    // Traverses one root (file, directory, or symlink per the fixed
    // policy), emitting items to the visitor. Takes the root BY VALUE;
    // walk_impl mutates the copy in place (trailing-slash strip).
    void walk(std::string path, const CrawlVisitor& visitor);

    // Directories entered so far (incremented before opendir; a failed
    // opendir still counts). Summed over all walk() calls on this Crawler.
    int64_t dirs_seen() const { return total_dirs_; }

private:
    // Emits a traversal-level Failure for `path` to the visitor.
    void emit(const std::string& path, Failure::Kind kind, int32_t err,
              const CrawlVisitor& visitor);

    // Mutates `path` in place (trailing-slash strip) — by design, see walk().
    void walk_impl(std::string& path, const CrawlVisitor& visitor);

    int64_t total_dirs_ = 0;
    std::vector<ino_t> depth_stack_;   // directory inodes on the current path

    // Keyed on (dev, ino): tracks files already seen for hardlink dedup.
    struct DevInoHash {
        size_t operator()(const dev_and_inode& k) const noexcept {
            return std::hash<dev_t>{}(k.dev) ^ (std::hash<ino_t>{}(k.ino) << 1);
        }
    };
    std::unordered_set<dev_and_inode, DevInoHash> seen_inodes_;
};

} // namespace vmt
