
> **Unofficial fork** of [vmtouch](https://github.com/hoytech/vmtouch) by Doug Hoyte.
> Based on version 1.3.1.
> Original license: BSD-3-Clause, Copyright 2009–2023 Doug Hoyte and contributors.
> This fork is not affiliated with or endorsed by the original author.


## What is vmtouch

vmtouch is a tool for learning about and controlling the file system cache of unix and unix-like systems. It is BSD licensed so you can basically do whatever you want with it.


### Install

    $ git clone https://github.com/hoytech/vmtouch.git
    $ cd vmtouch
    $ cmake -S . -B build
    $ cmake --build build -j
    $ sudo cmake --install build

### Build options

- `-DVMFTOUCH_SANITIZE=ON` — build with ASan+UBSan (development)
- `ctest` in the build directory runs the unit tests and the CLI
  characterization suite


### More information

* [Online manual](https://github.com/hoytech/vmtouch/blob/master/vmtouch.pod)
* [System tuning parameters](https://github.com/hoytech/vmtouch/blob/master/TUNING.md)
* [Original project](https://github.com/hoytech/vmtouch)


### Authors

Original vmtouch: copyright (c) 2009-2017 Doug Hoyte and contributors.
