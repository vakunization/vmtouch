// core/crawl.h — filesystem traversal for the vmtouch core library.
//
// Crawler owns its traversal state (depth stack, seen-inodes dedup tree,
// device pinning, file/dir totals) and holds references to the Session
// (which performs the per-file work) and the FileFilter. Ownership rule
// (agreed): the Crawler must not outlive the Session.
//
// The traversal logic is transplanted verbatim from the monolith's
// vmtouch_crawl(): trailing-slash strip, symlink handling, -F same-
// filesystem check, hardlink dedup (st_nlink > 1), symlink-loop detection,
// MAX_CRAWL_DEPTH. Deviations required by the seam:
//   * warning() -> Session::warning() (quiet handled there);
//   * fatal()   -> throw FatalError (single handler in the CLI);
//   * per-file work -> Session::process_file() instead of vmtouch_file();
//   * the seen-inodes tsearch tree lives in CrawlState and is freed in its
//     destructor (replacing the atexit workaround from the leak fix).

#pragma once

#include <string>

#include <search.h>   // tsearch/tfind/tdestroy

#include "common.h"
#include "session.h"

class Crawler {
public:
    explicit Crawler(Session& session);
    ~Crawler();

    Crawler(const Crawler&) = delete;
    Crawler& operator=(const Crawler&) = delete;

    // Traverses one path (file, directory, or symlink per options).
    // Takes the path BY VALUE: the parameter is this frame's private
    // mutable copy, replacing the C hack of mutating a shared buffer to
    // strip the trailing '/'. Recursion passes child paths the same way.
    void walk(std::string path);

    // Batch driver (from -b): reads paths separated by newline or NUL
    // (options().zero_delim) from a list file or stdin ("-") and walks each.
    void walk_batch(const std::string& list_path);

    // Totals for the summary. (Pages live in Session::stats().)
    int64_t total_files() const { return state_.total_files; }
    int64_t total_dirs() const { return state_.total_dirs; }

private:
    void walk_impl(std::string& path);

    struct State {
        int64_t total_files = 0;
        int64_t total_dirs = 0;
        int curr_crawl_depth = 0;
        ino_t crawl_inodes[1024];      // MAX_CRAWL_DEPTH, verbatim for now
        void* seen_inodes = nullptr;   // tsearch tree until modernization
        dev_t orig_device = 0;
        int orig_device_inited = 0;
    };

    State state_;
    Session& session_;
};
