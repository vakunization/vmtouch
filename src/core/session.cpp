// core/session.cpp — see session.h (spec2 §4).
//
// B4: the orchestrator. Two Report-update points (spec2 §4.6):
//   1. FileHandle::open failed -> failed++, failures[] (gated);
//   2. cache::* returned Failed -> failed++, failures[] (gated).
// Crawl errors arrive via CrawlItem.error and count as failed BEFORE any
// total_files++ (they are not files). Ok absorbs the stats; Lock keeps
// the region. Empty file: silent Ok (no failed, no record).
//
// Fatal boundary: catch_fatal_and_run catches FatalError, records
// Failure{Kind::Fatal, err, path-ish-text} ALWAYS (not gated), sets
// elapsed even on the fatal path, and returns Status::Fatal.

#include "session.h"

#include <chrono>

#include "file_handle.h"

namespace {

double wall_seconds_since(const std::chrono::steady_clock::time_point& t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

Session::Session(Options opts) : opts_(std::move(opts)) {}

Session::~Session() = default;

// ---------------------------------------------------------------------------

Status Session::catch_fatal_and_run(const std::function<void(Report&)>& body, Report& r) {
    const auto start = std::chrono::steady_clock::now();
    try {
        body(r);
        r.elapsed_seconds = wall_seconds_since(start);
        return Status::Ok;
    } catch (const FatalError& e) {
        // The one fatal channel: ALWAYS recorded (not gated by
        // collect_failures — the caller must be able to see what stopped
        // the traversal). The engine's message text rides in `path`
        // (kind=Fatal has no site path of its own).
        Failure f;
        f.kind = Failure::Kind::Fatal;
        f.err = 0;
        f.path = e.what();
        f.offset = 0;
        r.failures.push_back(std::move(f));
        r.elapsed_seconds = wall_seconds_since(start);
        return Status::Fatal;
    }
    // NIT-3 (2026-10-05): no catch-all — bad_alloc propagates (A-phase
    // ruling restored over the B4 noexcept boundary).
}

Status Session::process(std::span<const std::string> paths) {
    report_ = Report{};   // ONE call — ONE Report (reset at entry)

    return catch_fatal_and_run([&](Report& r) {
        // Per-call crawler state: dedup holds across ALL roots within the
        // call (run.sh pins "hl hl2 -> Files: 1"), never across calls.
        vmt::Crawler crawler;

        auto visitor = [&](const vmt::CrawlItem& item) {
            if (item.error != nullptr) {
                // Traversal failure: not a file — no total_files++.
                ++r.failed;
                if (opts_.collect_failures)
                    r.failures.push_back(*item.error);
                return;
            }
            handle_file(std::string(item.path), r);
        };

        for (const auto& root : paths)
            crawler.walk(root, visitor);

        r.total_dirs = crawler.dirs_seen();
    }, report_);
}

Status Session::process_fd(int fd, int64_t offset, int64_t max_len) {
    report_ = Report{};   // same Report semantics as process() (owner amendment)

    return catch_fatal_and_run([&](Report& r) {
        if (fd < 0) {
            ++r.failed;
            if (opts_.collect_failures)
                r.failures.push_back(Failure{Failure::Kind::Open, EBADF, "", offset});
            return;
        }

        FileInfoRange range;
        range.offset = offset;
        range.max_len = max_len;

        if (run_verb_on_fd(fd, "", range, r))
            return;   // Ok (or silent skip)

        ++r.failed;
    }, report_);
}

// ---------------------------------------------------------------------------

void Session::handle_file(const std::string& path, Report& r) {
    ++r.total_files;   // discovered (before open) — includes empties and failures

    auto opened = vmt::FileHandle::open(path);
    if (opened.is_error()) {
        ++r.failed;
        if (opts_.collect_failures) {
            Failure f = opened.error();
            f.path = path;   // FileHandle carries the path already; belt and braces
            r.failures.push_back(std::move(f));
        }
        return;
    }

    FileInfoRange range;
    range = opts_.range;
    if (run_verb_on_fd(opened.value().fd(), path, range, r))
        return;   // processed toward the goal

    ++r.failed;
}

bool Session::run_verb_on_fd(int fd, const std::string& path, const FileInfoRange& range,
                             Report& r) {
    // Ok -> true (stats absorbed; Lock keeps the region).
    // Failed -> false (Failure recorded, path attached here).
    // Fatal -> throws (caught at the boundary).
    cache::Params params;
    params.range = range;
    params.max_file_size = opts_.max_file_size;

    // Options::Verb (call policy, spec2 §2.1) -> cache::Verb (frozen engine
    // vocabulary, docs/vocabulary.md). Two enums on purpose: the engine's
    // is ABI-frozen; the Options one is free to grow CLI-level variants.
    const cache::Verb verb = [&] {
        switch (opts_.verb) {
            case Options::Verb::Inspect: return cache::Verb::Inspect;
            case Options::Verb::Warm:    return cache::Verb::Warm;
            case Options::Verb::Evict:   return cache::Verb::Evict;
            case Options::Verb::Lock:    return cache::Verb::Lock;
        }
        throw FatalError("unreachable verb");
    }();

    vmt::Result<cache::VerbOutcome> res = [&] {
        switch (verb) {
            case cache::Verb::Inspect: return cache::inspect(fd, params);
            case cache::Verb::Warm:    return cache::warm(fd, params);
            case cache::Verb::Evict:   return cache::evict(fd, params);
            case cache::Verb::Lock:    return cache::lock(fd, params);
        }
        throw FatalError("unreachable verb");
    }();

    if (res.is_error()) {
        if (opts_.collect_failures) {
            Failure f = res.error();
            f.path = path;   // the verb is path-free; the session attaches
            r.failures.push_back(std::move(f));
        }
        return false;
    }

    r.total_pages += res.value().stats.total_pages;
    r.total_pages_in_core += res.value().stats.total_pages_in_core;

    if (opts_.verb == Options::Verb::Lock) {
        kept_regions_.push_back(std::move(res.value().region));   // stays mapped
    } else if (res.value().region) {
        // Defensive: the engine munmaps inside inspect/warm/evict; only
        // Lock returns a live region. If this ever fires, record it.
        int munmap_err = 0;
        if (!res.value().region.unmap(munmap_err)) {
            if (opts_.collect_failures)
                r.failures.push_back(Failure{Failure::Kind::MunmapFailed, munmap_err, path, 0});
        }
    }
    return true;
}
