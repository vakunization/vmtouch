// core/session.h — the Session façade: the library's only public entry.
//
//   Session(Options)
//   Status process(std::span<const std::string> paths)
//   Status process_fd(int fd, int64_t offset, int64_t max_len)
//   const Report& get_report() const
//
// Contract:
//   * FatalError is caught internally and converted to Status::Fatal plus
//     a Failure{Kind::Fatal} record; any other exception (bad_alloc)
//     propagates to the caller;
//   * process_fd() behaves exactly like process() — same Report semantics,
//     same fatal handling — but takes a caller-owned fd that the library
//     neither opens nor closes;
//   * ONE process/process_fd call fills ONE Report (reset at entry); the
//     Report of a fatal call holds everything accumulated up to the
//     failure point INCLUDING the Fatal record;
//   * kept_regions_ (Lock) accumulates across calls; the Session must not
//     be destroyed while the locks are needed;
//   * collect_failures (Options) gates Report.failures EXCEPT the Fatal
//     record, which is always stored (it is the only fatal channel).

#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "common.h"
#include "cache.h"
#include "crawl.h"

namespace vmt {

class Session {
public:
    // The call result. Invariants:
    //   total_files — files DISCOVERED (before open); includes empty files
    //                 and files that later fail;
    //   failed      — files not processed toward the goal (open/verb/crawl
    //                 failures); empty files are NOT failed;
    //   failures.size() == failed + (fatal ? 1 : 0) under collect_failures
    //                 (the Fatal record is always appended);
    //   elapsed_seconds — wall clock of the call, measured through fatals.
    struct Report {
        int64_t total_files = 0;
        int64_t failed = 0;
        int64_t total_dirs = 0;
        int64_t total_pages = 0;
        int64_t total_pages_in_core = 0;
        double elapsed_seconds = 0.0;
        std::vector<Failure> failures;   // gated by Options::collect_failures (+ always Fatal)
    };

    explicit Session(Options opts);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Traverses and processes all roots; order preserved. An empty list is
    // valid (zeroed Report). Does not throw FatalError (converted to
    // Status::Fatal internally); bad_alloc propagates.
    Status process(std::span<const std::string> paths);

    // S3: caller-owned fd; same Report semantics as process().
    Status process_fd(int fd, int64_t offset, int64_t max_len);

    // The Report of the LAST process()/process_fd() call. Valid until the
    // next call or destruction. Before any call: a zeroed Report.
    const Report& get_report() const { return report_; }

private:
    // One discovered file: open -> dispatch -> classify (visitor body).
    // Takes string_view — no extra copy per file.
    void handle_file(std::string_view path, Report& r);

    // Verb dispatch + classification for an already-open fd. Does NOT
    // close the fd. Returns false on Failed (the Failure is recorded);
    // throws only for infrastructural causes.
    bool run_verb_on_fd(int fd, const std::string& path, const FileInfoRange& range,
                        Report& r);

    // The boundary wrapper: catches FatalError -> Status::Fatal + record.
    Status catch_fatal_and_run(const std::function<void(Report&)>& body, Report& r);

    Options opts_;
    Report report_;                                  // last call's Report
    std::vector<cache::MappedRegion> kept_regions_;  // Lock: alive for process lifetime
};

} // namespace vmt
