# Embedder scenarios (lens for unit tests and the future C API)

These sketches are deliberately rough — they are not a design, they are the
use cases the Phase-3 C API will be reviewed against, and the lens for the
unit-test phase. Each test file names the scenarios it serves.

## S1 — DevOps cache warmer (Go service)

> Before our analytics job starts, touch the day's Parquet files so they're
> in page cache. No shelling out to the CLI: error codes I can log, and the
> resident/total numbers per run for Prometheus. Never lock anything
> accidentally. One mount is read-only — tell me what got skipped, not just
> "failed".

Needs: process a caller-supplied list of paths; numeric results; Skipped vs
hard-failure distinction; a warning/progress channel other than stdout;
locks only when explicitly requested.

## S2 — QA cache-state checker (Python, test fixture)

> My tests are flaky because a previous test left files hot. I want:
> assert this file is 100% resident; evict it; assert 0%. Fail my test with
> a readable message when not resident. Tell me whether residency reporting
> is even accurate on this kernel (the mincore permission heuristic) so I
> can skip the check.

Needs: query mode as a first-class operation; evict; per-file residency
numbers; the kernel heuristic exposed (today it's a warning in verbose
output); human-readable problem strings.

## S3 — mlock model loader (C/C++ ML inference server)

> At startup I mmap my model files and mlock them. I already have the fd
> open — do NOT open the path again, it may have been swapped underneath
> me. Lock my already-mapped region, and tell me WHY mlock failed
> (RLIMIT_MEMLOCK too low is a config error, not a crash). Do not unmap
> anything afterward. Optional: residency chart delivered to my own logger.

Needs: the cache layer reachable directly with a caller-owned fd; distinct
failure reasons; Lock-mode skip-munmap semantics; chart bytes through a
custom sink.

## S4 — the CLI itself

`src/cli/main.cpp` is the first consumer. Whatever the CLI still needs that
the library makes awkward is a design bug we already live with.

## How these are used

1. Every unit test names the scenarios it serves; behavior tests outlive
   API changes (only setup lines get edited).
2. Friction found while writing tests is tagged `// scenario: ...` and
   collected for Phase 3, not fixed ad hoc.
3. Phase 3 review: for each sketch, "can a Go/Python/C author write it
   against the C API without shelling out or reaching into internals?"
   Each sketch should become a runnable example in `examples/`.
