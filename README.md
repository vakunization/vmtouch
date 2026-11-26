# vmtouch

Inspect, warm, evict, or lock pages in the filesystem cache. This unofficial
C++20 fork is based on [Doug Hoyte’s vmtouch 1.3.1](https://github.com/hoytech/vmtouch).
It is not affiliated with upstream. The original C implementation is kept in
[`vmtouch.c`](vmtouch.c) for reference and is not built.

## Build and test

Use CMake 3.16 or newer and a C++20 compiler and standard library with
`std::format` support (for example, GCC/libstdc++ 13 or newer).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWARN_FLAGS=-Werror
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

CTest runs the unit and CLI characterization suites. To keep fixtures on disk
when the build directory is on tmpfs, set `VMTOUCH_TEST_DIR` to a writable,
disk-backed directory. Configure a separate build with
`-DVMFTOUCH_SANITIZE=ON` for ASan and UBSan. Set `VMFILE_PLAIN` to the absolute
path to a plain binary to exercise the real lock syscalls in the sanitized CLI
test run.

## Use

```sh
vmtouch -o kv /path/to/file       # report resident and total pages
vmtouch -t -p 0-16M /path/to/file # warm the selected range
vmtouch -e /path/to/file         # request eviction; the OS may keep pages
vmtouch -l /path/to/file         # lock pages; stay running to keep them locked
```

Run `vmtouch --help` or see the [manual](man/vmtouch.8) for all options.
See the [changelog](CHANGELOG.md) for deliberate differences from upstream and
the [design notes](DESIGN.md) for the in-tree C++ API.

BSD-3-Clause; see [LICENSE](LICENSE).
