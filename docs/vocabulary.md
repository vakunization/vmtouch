# Vocabulary — the library's frozen data types (A5)

This document is the single reference for the value types the library
returns. It exists so the **Phase-3 C API** can map them 1:1 without
re-deriving anything from the code.

Rule: these are *plain data*. The library never prints, never exits, and
never formats text; the CLI does all of that. Field order below is the
documentation order, not necessarily the layout order.

Fixed width: every enum is explicitly `int32_t`-backed (or an explicit
`int` where noted) and every enum value is spelled out. Adding values is
allowed; **renumbering is not** — the C enum depends on the numbers.

---

## 1. `Failure::Kind` — why a path was skipped (common.h)

Reported by the crawler and the verb engine; recorded by the Session
regardless of `-q`; formatted by the CLI.

| Value | Kind | Origin site | CLI text (upstream-identical) |
|---|---|---|---|
| 1 | `Open` | session open / batch | `unable to open PATH (ERR), skipping` |
| 2 | `Stat` | crawl lstat/stat | `unable to stat PATH (ERR)` |
| 3 | `Map` | mmap | `unable to mmap file PATH (ERR), skipping` |
| 4 | `TooLarge` | `-m` gate | `file PATH too large, skipping` |
| 5 | `BeyondEof` | `-p` offset | `file PATH smaller than offset, skipping` |
| 6 | `NotRegular` | crawl type check | `skipping non-regular file: PATH` |
| 7 | `Symlink` | crawl, no `-f` | `not following symbolic link PATH` |
| 8 | `SeparateFs` | crawl, `-F` | `not recursing into separate filesystem PATH` |
| 9 | `CrawlLoop` | crawl loop check | `symbolic link loop detected: PATH` |
| 10 | `PathTooLong` | crawl | `path too long PATH` |
| 11 | `MincoreInaccurate` | kernel >= 5.2 heuristic | `Process does not have write permission, residency chart will not be accurate` |
| 12 | `EvictFailed` | *(legacy umbrella; superseded by 18/19)* | `unable to evict PATH (ERR)` |
| 13 | `Closedir` | crawl | `unable to closedir PATH (ERR)` |
| 14 | `FstatFailed` | verb prelude fstat | `unable to fstat PATH (ERR), skipping` |
| 15 | `IoctlSizeFailed` | block-device size | `unable to ioctl PATH (ERR), skipping` |
| 16 | `OpendirFailed` | crawl opendir | `unable to opendir PATH (ERR), skipping` |
| 17 | `MunmapFailed` | session unmap | `unable to munmap file PATH (ERR)` |
| 18 | `EvictAdviseFailed` | `-e` (posix_fadvise) | `unable to posix_fadvise file PATH (ERR)` |
| 19 | `EvictInvalidateFailed` | `-e` (msync, BSD/Apple/Sun) | `unable to msync invalidate file PATH (ERR)` |

Values 1–13 are the original A1 set (frozen by decision); 14–19 were
added by the A1-drift fix so each upstream warning *site* renders its own
text. `EvictFailed` (12) remains for compatibility but is no longer
produced by the engine (18/19 are).

## 2. `Failure` — one recorded skip/failure (common.h)

| Field | Type | Meaning |
|---|---|---|
| `kind` | `Failure::Kind` | which site (table above) |
| `err` | `int32_t` | errno, 0 if not applicable |
| `path` | `std::string` | the path being processed; empty for fd-only ops |
| `offset` | `int64_t` | range problems (`BeyondEof`); 0 otherwise |

## 3. `Stats` — the run's page numbers (common.h)

| Field | Type | Meaning |
|---|---|---|
| `total_pages` | `int64_t` | pages in all processed files/ranges |
| `total_pages_in_core` | `int64_t` | of those, pages found resident |

## 4. `Status` — per-call outcome (common.h)

| Value | Meaning |
|---|---|
| `Ok = 0` | processed |
| `Skipped = 1` | not processed; a `Failure` record explains why |

## 5. `cache::Verb` — what to do with the mapped pages (cache.h)

| Value | Verb | CLI flag |
|---|---|---|
| `Inspect = 1` | query residency | *(neither `-t` nor `-e` nor `-l`)* |
| `Warm = 2` | touch pages into cache | `-t` |
| `Evict = 3` | drop pages from cache | `-e` |
| `Lock = 4` | touch + `mlock`, mapping kept | `-l` |

The verb × fd functions are `cache::inspect/warm/evict/lock(fd, Params,
EngineEvents*)`. `Verb` names the intent; the functions are the engine.

## 6. `cache::FileResult` — what one verb did (cache.h)

| Field | Type | Meaning |
|---|---|---|
| `code` | `Code` (below) | outcome class |
| `silent` | `bool` | skipped with NO record (empty file) |
| `skip` | `Failure::Kind` | which record to write when `!silent` |
| `err` | `int32_t` | errno for the skip/fatal |
| `offset` | `int64_t` | `BeyondEof` offset |
| `fatal` | `FatalWhat` (below) | fatal fact for the session to re-throw with path |
| `pages_total` | `int64_t` | pages in range (Ok) |
| `pages_resident` | `int64_t` | pages resident (Ok) |
| `evict_failed` | `bool` | eviction syscall failed (still `Ok`) |
| `evict_kind` | `Failure::Kind` | which eviction text (18/19) |
| `mincore_inaccurate` | `bool` | kernel >= 5.2 permission heuristic |
| `region` | `MappedRegion` | the live mapping; caller keeps or drops |

`Code`: `Ok = 0`, `Skipped = 1`, `Fatal = 2`.
`FatalWhat`: `None = 0`, `MlockFailed = 1`, `MincoreFailed = 2`,
`MisalignedMap = 3`. For `MlockFailed`/`MincoreFailed` the session builds
`mlock: PATH (ERR)` / `mincore PATH (ERR)`; for `MisalignedMap` it builds
`mmap(PATH) wasn't page aligned`. Hopeless-but-pathless situations
(allocation failure, unsupported platform) are thrown as `FatalError`
directly by the engine, so they never appear here.

## 7. `Session::Report` — the run summary (session.h)

| Field | Type | Meaning |
|---|---|---|
| `total_files` | `int64_t` | files processed |
| `total_dirs` | `int64_t` | directories entered |
| `stats` | `Stats` | the page numbers (section 3) |

Returned by `Session::report()`. The CLI renders it; the library never
does.

## 8. `cache::Params` — caller policy per call (cache.h)

| Field | Type | Meaning |
|---|---|---|
| `range` | `FileInfoRange` | `-p`: `offset` / `max_len` (0 = to EOF) |
| `max_file_size` | `size_t` | `-m` (`SIZE_MAX` = unlimited) |

## 9. `Options` — caller policy for the whole run (common.h)

The CLI fills this; the Session holds it. Notable groups: verb selection
(`touch`/`evict`/`lock`/`lockall`), output policy (`quiet`/`verbose`/
`output`/`zero_delim`), traversal policy (`followsymlinks`/
`singlefilesystem`/`ignorehardlinkeduplictes`), and the A4 filter lists
(`ignore` = `-i`, `include` = `-I`).

---

## Phase-3 C mapping notes

* Enums map to C `enum` with the numbers above; `Failure` becomes a
  struct with a `const char*` path (or two-call accessor). `Status`,
  `Verb`, `Code`, `FatalWhat`, `Failure::Kind` all fit `int`.
* `FileResult` and `Report` are plain-data structs; the C header can
  mirror them field-for-field (the `MappedRegion` member is the one
  non-POD field and will become an opaque handle in C).
* Nothing here owns heap memory except `path` and `MappedRegion`; the C
  API will need explicit ownership rules for those two only.
