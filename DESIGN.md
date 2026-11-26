# C++ core

`vmtouch_core` is a static CMake target. `src/core/session.h` is the intended
in-tree entry point. The core is not installed as a development package and
does not yet publish a C ABI.

## One session, one report per call

Construct `vmt::Session` with `Options`: choose an inspect, warm, evict, or lock
verb, a range, a maximum file size, and whether to record individual failures.
`process()` takes a span of paths. `process_fd()` instead borrows an open
descriptor: it does not open or close it. Its explicit range overrides the
session's default range. Both range values must be non-negative, and the
offset must be page-aligned. A zero length means to EOF.

Both operations replace the previous `Report`. `get_report()` returns a
borrowed reference; read or copy its contents before the next call. A session
is not safe for concurrent calls. Independent calls can still affect the
same OS page cache and process-wide resource limits.

- `Status::Ok` means the call completed, not that all files succeeded.
  Per-item failures increment `Report::failed`; `collect_failures` controls
  whether their details are kept.
- `Status::Fatal` means the run stopped. The report retains partial totals
  and always includes a fatal record. Other exceptions, such as
  `std::bad_alloc`, can propagate.
- A successful `Lock` keeps its mapping in the session until destruction,
  even when later calls replace the report. `Warm` normally unmaps immediately.
  `Options::retain_warm_mappings` lets the caller retain warmed mappings without
  locking them; the CLI uses it to implement `-L` with `mlockall`.

## Boundaries

The crawler streams paths and errors to a visitor. It skips symbolic links
encountered during the walk, traverses filesystem boundaries, deduplicates
hardlinked files across roots within the same call, and caps recursion at 1024
directory levels. Directory streams, descriptors opened by the core, and
mapped regions have scoped owners.

The cache engine works on descriptors and returns results without writing to
the terminal. Transient syscall failures get at most three attempts. `EINTR`
retries immediately; `EAGAIN` and `EBUSY` wait before another attempt. The CLI
owns diagnostics, exit statuses, daemon startup, signals, and pidfiles.
