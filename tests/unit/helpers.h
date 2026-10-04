// tests/unit/helpers.h — shared test setup for cache/session tests.
//
// Each helper here is justified by immediate use in test_cache.cpp /
// test_session.cpp (not speculation). If Phase 2/3 changes the API
// signatures, these helpers are where the edits concentrate — the test
// assertions (the behavior contracts) survive unchanged.

#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "test.h"
#include "core/cache.h"
#include "core/session.h"

namespace unit {

// ---------------------------------------------------------------------------
// TempFile: RAII temp file of n pages, fd kept open for the test's lifetime.
// ---------------------------------------------------------------------------
struct TempFile {
    std::string path;
    int fd = -1;
    int64_t size;   // bytes

    explicit TempFile(int n_pages)
        : size(static_cast<int64_t>(n_pages) * pagesize) {
        char tmpl[] = "/tmp/vmtouch-unit-XXXXXX";
        fd = mkstemp(tmpl);
        if (fd == -1) { std::perror("mkstemp"); std::abort(); }
        path = tmpl;
        if (ftruncate(fd, size) == -1) { std::perror("ftruncate"); std::abort(); }
    }
    ~TempFile() {
        if (fd != -1) ::close(fd);
        ::unlink(path.c_str());
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// ---------------------------------------------------------------------------
// RecordingSink: REMOVED in B2 (the sink layer is gone — verbs return
// Result<VerbOutcome>, no events). Cache-level tests read the Result data
// directly; session-level tests read failures()/stats().
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// WarningCapture (A1): captures structured Failure records instead of text.
// `contains(text)` keeps the old substring-check ergonomics by formatting
// each record the same way the CLI does.
// ---------------------------------------------------------------------------
struct WarningCapture {
    std::vector<Failure> failures;
    void add(Failure f) { failures.push_back(std::move(f)); }
    bool contains(const std::string& fragment) const {
        for (const auto& f : failures)
            if (format(f).find(fragment) != std::string::npos) return true;
        return false;
    }
    bool empty() const { return failures.empty(); }
    size_t size() const { return failures.size(); }
    // test-side formatter mirroring the CLI's format_failure (A1-drift fix:
    // all 19 kinds carry the exact upstream site texts)
    static std::string format(const Failure& f) {
        switch (f.kind) {
            case Failure::Kind::Open:          return "unable to open " + f.path;
            case Failure::Kind::Stat:          return "unable to stat " + f.path;
            case Failure::Kind::FstatFailed:   return "unable to fstat " + f.path;
            case Failure::Kind::IoctlSizeFailed: return "unable to ioctl " + f.path;
            case Failure::Kind::Map:           return "unable to mmap file " + f.path;
            case Failure::Kind::MunmapFailed:  return "unable to munmap file " + f.path;
            case Failure::Kind::OpendirFailed: return "unable to opendir " + f.path;
            case Failure::Kind::TooLarge:      return "file " + f.path + " too large, skipping";
            case Failure::Kind::BeyondEof:     return "file " + f.path + " smaller than offset, skipping";
            case Failure::Kind::NotRegular:    return "skipping non-regular file: " + f.path;
            case Failure::Kind::Symlink:       return "not following symbolic link " + f.path;
            case Failure::Kind::SeparateFs:    return "not recursing into separate filesystem " + f.path;
            case Failure::Kind::CrawlLoop:     return "symbolic link loop detected: " + f.path;
            case Failure::Kind::PathTooLong:   return "path too long " + f.path;
            case Failure::Kind::MincoreInaccurate: return "mincore inaccurate";
            case Failure::Kind::EvictFailed:   return "unable to evict " + f.path;
            case Failure::Kind::EvictAdviseFailed:  return "unable to posix_fadvise file " + f.path;
            case Failure::Kind::EvictInvalidateFailed: return "unable to msync invalidate file " + f.path;
            case Failure::Kind::Closedir:      return "unable to closedir " + f.path;
        }
        return "";
    }
};

// ---------------------------------------------------------------------------
// Session wiring (B4): Session(Options); tests build Options with the
// library-level fields (verb/range/max_file_size/collect_failures).
// ---------------------------------------------------------------------------
struct SharedSinkState {};

inline std::unique_ptr<Session>
make_session(std::shared_ptr<SharedSinkState>, WarningCapture& warn,
             const FileInfoRange& range = {}, bool quiet = false) {
    Options o;
    o.collect_failures = true;   // tests read failures via get_report()
    o.range = range;
    auto s = std::make_unique<Session>(o);
    return s;
}

inline std::unique_ptr<Session>
make_session(std::shared_ptr<SharedSinkState>, WarningCapture& warn,
             const Options& opts, const FileInfoRange& range = {}) {
    Options o = opts;
    o.collect_failures = true;
    if (range.offset || range.max_len) o.range = range;
    auto s = std::make_unique<Session>(o);
    return s;
}

// Convenience: a fresh shared sink state as shared_ptr (kept for call-site
// compatibility; the state is no longer populated — read the Report).
inline std::shared_ptr<SharedSinkState> make_shared_state() {
    return std::make_shared<SharedSinkState>();
}

} // namespace unit
