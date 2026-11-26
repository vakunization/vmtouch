// core/session.cpp — see session.h.
//
// Report-update points:
//   1. FileHandle::open failed -> failed++, failures[] (gated);
//   2. cache::* returned Failed -> failed++, failures[] (gated).
// Crawl errors arrive via CrawlItem.error and count as failed BEFORE any
// total_files++ (they are not files). Ok absorbs the stats; Lock keeps
// the region. Empty file: silent Ok (no failed, no record).

#include "session.h"

#include <chrono>

#include "file_handle.h"

namespace vmt {

static double wall_seconds_since(const std::chrono::steady_clock::time_point &t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

Session::Session(Options opts) : opts_(std::move(opts)) {}

Session::~Session() = default;

// ---------------------------------------------------------------------------

Status Session::catch_fatal_and_run(const std::function<void(Report &)> &body, Report &r) {
  const auto start = std::chrono::steady_clock::now();
  try {
    body(r);
    r.elapsed_seconds = wall_seconds_since(start);
    return Status::Ok;
  } catch (const FatalError &e) {
    // The one fatal channel: always recorded (not gated by
    // collect_failures). The message text rides in `path` —
    // kind=Fatal has no site path of its own.
    Failure f;
    f.kind = Failure::Kind::Fatal;
    f.err = 0;
    f.path = e.what();
    f.offset = 0;
    r.failures.push_back(std::move(f));
    r.elapsed_seconds = wall_seconds_since(start);
    return Status::Fatal;
  }
  // No catch-all: bad_alloc propagates to the caller.
}

Status Session::process(std::span<const std::string> paths) {
  report_ = Report{}; // one call — one Report (reset at entry)

  // Per-call crawler state: dedup holds across all roots within this call.
  Crawler crawler;

  const Status status = catch_fatal_and_run(
      [&](Report &r) {
        auto visitor = [&](const CrawlItem &item) {
          if (item.error != nullptr) {
            // Traversal failure: not a file — no total_files++.
            ++r.failed;
            if (opts_.collect_failures)
              r.failures.push_back(*item.error);
            return;
          }
          handle_file(item.path, r);
        };

        for (const auto &root : paths)
          crawler.walk(root, visitor);
      },
      report_);

  // A FatalError may have interrupted the walk after entering directories.
  report_.total_dirs = crawler.dirs_seen();
  return status;
}

Status Session::process_fd(int fd, int64_t offset, int64_t max_len) {
  report_ = Report{}; // same Report semantics as process()

  return catch_fatal_and_run(
      [&](Report &r) {
        if (fd < 0) {
          ++r.failed;
          if (opts_.collect_failures)
            r.failures.push_back(Failure{Failure::Kind::Open, EBADF, "bad fd (process_fd)", 0});
          return;
        }

        if (run_verb_on_fd(fd, "", FileInfoRange{offset, max_len}, r))
          return; // Ok (or silent skip)

        ++r.failed;
      },
      report_);
}

// ---------------------------------------------------------------------------

void Session::handle_file(std::string_view path, Report &r) {
  ++r.total_files; // discovered (before open) — includes empties and failures

  std::string path_str(path); // one copy, shared by open + record
  auto opened = FileHandle::open(path_str);
  if (opened.is_error()) {
    ++r.failed;
    if (opts_.collect_failures) {
      Failure f = opened.error();
      f.path = path;
      r.failures.push_back(std::move(f));
    }
    return;
  }

  if (run_verb_on_fd(opened.value().fd(), path_str, opts_.range, r))
    return; // processed toward the goal

  ++r.failed;
}

bool Session::run_verb_on_fd(int fd, const std::string &path, const FileInfoRange &range,
                             Report &r) {
  // Ok    -> true (stats absorbed; Lock keeps the region);
  // Failed-> false (Failure recorded, path attached here);
  // Fatal -> throws (caught at the boundary).
  cache::Params params;
  params.range = range;
  params.max_file_size = opts_.max_file_size;
  params.retain_warm_mapping = opts_.retain_warm_mappings;

  // Verb dispatch. Options::Verb (call policy) selects the engine
  // function directly — no second enum.
  Result<cache::VerbOutcome> res = [&] {
    switch (opts_.verb) {
    case Options::Verb::Inspect:
      return cache::inspect(fd, params);
    case Options::Verb::Warm:
      return cache::warm(fd, params);
    case Options::Verb::Evict:
      return cache::evict(fd, params);
    case Options::Verb::Lock:
      return cache::lock(fd, params);
    }
    throw FatalError("unreachable verb");
  }();

  if (res.is_error()) {
    if (opts_.collect_failures) {
      Failure f = res.error();
      f.path = path; // the verb is path-free; the session attaches
      r.failures.push_back(std::move(f));
    }
    return false;
  }

  r.total_pages += res.value().stats.total_pages;
  r.total_pages_in_core += res.value().stats.total_pages_in_core;

  if (res.value().region)
    kept_regions_.push_back(std::move(res.value().region));
  return true;
}

} // namespace vmt
