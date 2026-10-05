// core/crawl.h — filesystem traversal (spec2 §5).
//
// B3: the Crawler is a PRODUCER. It walks the filesystem and emits items
// through a caller-supplied callback; it knows nothing about Session,
// Report, Options, FileHandle, cache::* or CLI flags (knowledge map §10).
//
// Traversal policy is FIXED as upstream defaults (ratified ruling: the
// -f/-F/-h flags are dropped):
//   * symlinks are NOT followed (a symlink -> Failure{Kind::Symlink});
//   * no filesystem-boundary check (upstream default: cross freely);
//   * hardlink dedup ENABLED (st_nlink > 1 seen by (dev, ino) -> skip);
//   * MAX_CRAWL_DEPTH = 1024 (overflow -> FatalError);
//   * symlink loops -> Failure{Kind::CrawlLoop} (upstream behavior; the
//     loop check runs against the depth stack of directory inodes).
//
// Lifetime: the caller creates one Crawler per process() call; the
// seen-inodes tree and counters live for the Crawler's lifetime, so dedup
// works ACROSS roots in one call (pinned by run.sh: "hl hl2 -> Files: 1").

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <search.h>   // tsearch/tfind/tdestroy

#include "common.h"

namespace vmt {

// One emitted item (spec2 §2.7): exactly one side is meaningful.
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

    // Takes the root BY VALUE; recursion passes child copies. walk_impl
    // mutates the string IN PLACE (DEC-6(a): documented at the signature)
    // to strip the trailing '/' exactly like the C original did.
    void walk(std::string path, const CrawlVisitor& visitor);

    // Directories ENTERED so far (incremented before opendir; a failed
    // opendir still counts). Summed over all walk() calls on this Crawler.
    int64_t dirs_seen() const { return state_.total_dirs; }

private:
    struct State {
        int64_t total_dirs = 0;
        int curr_crawl_depth = 0;
        ino_t crawl_inodes[1024];      // MAX_CRAWL_DEPTH, verbatim for now
        void* seen_inodes = nullptr;   // tsearch tree until modernization
    };

    // Mutates `path` in place (trailing-slash strip) — by design, see walk().
    void walk_impl(std::string& path, const CrawlVisitor& visitor);

    State state_;
};

} // namespace vmt
