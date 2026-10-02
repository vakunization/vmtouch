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
// RecordingSink: a ProgressSink that records calls (cache tests pass it by
// reference, so the test can inspect it after the call).
// ---------------------------------------------------------------------------
struct RecordingSink final : public cache::ProgressSink {
    std::vector<std::string> file_starts;
    std::vector<std::string> evictings;
    std::vector<int64_t> chart_page_counts;
    std::vector<std::pair<std::string, std::pair<int64_t, int64_t>>> done; // path, {pages, resident}

    void on_file_start(std::string_view p) override { file_starts.emplace_back(p); }
    void on_evicting(std::string_view p) override { evictings.emplace_back(p); }
    void on_chart(std::span<const unsigned char>, int64_t pages_in_file) override {
        chart_page_counts.push_back(pages_in_file);
    }
    void on_file_done(std::string_view p, int64_t pages, int64_t resident) override {
        done.emplace_back(std::string(p), std::make_pair(pages, resident));
    }
};

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
    // test-side formatter mirroring the CLI's format_failure
    static std::string format(const Failure& f) {
        switch (f.kind) {
            case Failure::Kind::Open:          return "unable to open " + f.path;
            case Failure::Kind::Stat:          return "unable to stat " + f.path;
            case Failure::Kind::Map:           return "unable to mmap file " + f.path;
            case Failure::Kind::TooLarge:      return "file " + f.path + " too large, skipping";
            case Failure::Kind::BeyondEof:     return "file " + f.path + " smaller than offset, skipping";
            case Failure::Kind::NotRegular:    return "skipping non-regular file: " + f.path;
            case Failure::Kind::Symlink:       return "not following symbolic link " + f.path;
            case Failure::Kind::SeparateFs:    return "not recursing into separate filesystem " + f.path;
            case Failure::Kind::CrawlLoop:     return "symbolic link loop detected: " + f.path;
            case Failure::Kind::PathTooLong:   return "path too long " + f.path;
            case Failure::Kind::MincoreInaccurate: return "mincore inaccurate";
            case Failure::Kind::EvictFailed:   return "unable to evict " + f.path;
            case Failure::Kind::Closedir:      return "unable to closedir " + f.path;
        }
        return "";
    }
};

// FailureSink adapter over WarningCapture (A1) — for direct cache-level tests.
struct WarnOf final : public cache::FailureSink {
    WarningCapture* w;
    std::string path;   // cache-level calls carry no path; attached if set
    explicit WarnOf(WarningCapture& cap, std::string p = "")
        : w(&cap), path(std::move(p)) {}
    void add(Failure::Kind kind, int err, int64_t offset = 0) override {
        w->add(Failure{kind, err, path, offset});
    }
};

// S3-style FileInfo for an already-open fd (caller owns the fd).
inline cache::FileInfo make_file_info(const std::string& path, int fd,
                                      int64_t offset = 0, int64_t max_len = 0,
                                      bool verbose = true) {
    cache::FileInfo fi;
    fi.path = path;
    fi.fd = fd;
    fi.offset = offset;
    fi.max_len = max_len;
    fi.max_file_size = SIZE_MAX;   // unlimited unless the test overrides
    fi.verbose = verbose;
    fi.lockall = false;
    return fi;
}

// ---------------------------------------------------------------------------
// Session wiring: Session owns its sink (unique_ptr), so the observable
// state lives in a shared box the test holds. make_session() is the one
// place that knows the current Session constructor shape.
// ---------------------------------------------------------------------------
struct SharedSinkState {
    std::vector<std::string> file_starts;
    std::vector<std::string> evictings;
    std::vector<int64_t> chart_page_counts;
    std::vector<std::pair<std::string, std::pair<int64_t, int64_t>>> done;
};

struct SharedSink final : public cache::ProgressSink {
    explicit SharedSink(std::shared_ptr<SharedSinkState> st) : state(std::move(st)) {}
    std::shared_ptr<SharedSinkState> state;
    void on_file_start(std::string_view p) override { state->file_starts.emplace_back(p); }
    void on_evicting(std::string_view p) override { state->evictings.emplace_back(p); }
    void on_chart(std::span<const unsigned char>, int64_t pages_in_file) override {
        state->chart_page_counts.push_back(pages_in_file);
    }
    void on_file_done(std::string_view p, int64_t pages, int64_t resident) override {
        state->done.emplace_back(std::string(p), std::make_pair(pages, resident));
    }
};

inline std::unique_ptr<Session>
make_session(std::shared_ptr<SharedSinkState> state, WarningCapture& warn,
             const FileInfoRange& range = {}, bool quiet = false) {
    Options o;
    o.quiet = quiet;
    o.verbose = 1;   // scenario: S1/S2 want per-file numbers; today those ride
                     // on the verbose flag (CLI coupling — Phase 3 agenda)
    auto s = std::make_unique<Session>(
        o,
        std::make_unique<SharedSink>(std::move(state)),
        [&warn](std::string_view) {});   // legacy WarnFn; records are read via warn
    s->set_range(range);
    return s;
}

// make_session with caller-supplied Options (the CLI's real flow: getopt
// fills Options, then the Session is constructed from the final values).
inline std::unique_ptr<Session>
make_session(std::shared_ptr<SharedSinkState> state, WarningCapture& warn,
             const Options& opts, const FileInfoRange& range = {}) {
    Options o = opts;
    if (o.verbose == 0) o.verbose = 1;   // same per-file-numbers default as above
    auto s = std::make_unique<Session>(
        o,
        std::make_unique<SharedSink>(std::move(state)),
        [&warn](std::string_view) {});   // legacy WarnFn; records are read via warn
    s->set_range(range);
    return s;
}

// Convenience: a fresh shared sink state as shared_ptr.
inline std::shared_ptr<SharedSinkState> make_shared_state() {
    return std::make_shared<SharedSinkState>();
}

} // namespace unit
