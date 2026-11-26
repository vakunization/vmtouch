# Changelog

## Unreleased — C++20 fork

- Replaced the C build with a C++20 static core and CLI, built with CMake.
  Added unit and CLI regression suites and a sanitizer configuration.
- Removed `-f`, `-F`, `-i`, and `-I`. Traversal skips symbolic links, crosses
  filesystem boundaries, and always deduplicates hardlinks. `-h` now means
  help, as does `--help`.
- `-v` no longer prints per-file paths or residency charts. Output is an
  aggregate summary, optionally in `-o kv` format.
- File-level syscall failures, including `mincore` and `mlock` failures,
  are reported without aborting traversal. A foreground lock run still
  waits after reporting any per-file failures.
- `-L` retains the warmed target mappings until `mlockall(MCL_CURRENT)`
  runs and keeps them mapped for the duration of the lock run.
- `-d -w` reports daemon readiness or startup failure to the parent;
  `-P` pidfiles are removed on handled termination signals.
