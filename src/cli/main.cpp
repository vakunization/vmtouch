/***********************************************************************
vmtouch - the Virtual Memory Toucher

Portable file system cache diagnostics and control

by Doug Hoyte (doug@hcsw.org)

Compilation:
  gcc -Wall -O3 -o vmtouch vmtouch.c

************************************************************************

Copyright 2009-2023 Doug Hoyte and contributors

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***********************************************************************/


#if defined(__linux__) || (defined(__hpux) && !defined(__LP64__))
// Make sure off_t is 64 bits on linux and when creating 32bit programs
// on HP-UX.
#define _FILE_OFFSET_BITS 64
#endif

#ifdef __linux__
// Required for posix_fadvise() on some linux systems
#define _XOPEN_SOURCE 600
// Required for mincore() on some linux systems
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#endif

#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>       // freopen, select(2)-era FILE* for reopen_all
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>   // mlockall
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli_options.h"
#include "core/common.h"
#include "core/platform.h"
#include "core/session.h"




int exit_pipe[2];

int daemon_pid;

// wait is passed explicitly: this must work both in the daemon child (which
// owns `o`) and from main()'s catch handler without any global CliOptions*.
void send_exit_signal(const CliOptions& o, char code) {
  if (daemon_pid == 0 && o.wait) {
    if (write(exit_pipe[1], &code, 1) < 0)
      std::fputs("vmtouch: FATAL: write\n", stderr);
  }
}

void usage() {
  std::cout << std::format(
      "\n"
      "vmtouch v{} - the Virtual Memory Toucher by Doug Hoyte\n"
      "Portable file system cache diagnostics and control\n\n"
      "Usage: vmtouch [OPTIONS] ... FILES OR DIRECTORIES ...\n\n"
      "Options:\n"
      "  -t touch pages into memory\n"
      "  -e evict pages from memory\n"
      "  -l lock pages in physical memory with mlock(2)\n"
      "  -L lock pages in physical memory with mlockall(2)\n"
      "  -d daemon mode\n"
      "  -m <size> max file size to touch\n"
      "  -p <range> use the specified portion instead of the entire file\n"
      "  -b <list file> get files or directories from the list file\n"
      "  -0 in batch mode (-b) separate paths with NUL byte instead of newline\n"
      "  -w wait until all pages are locked (only useful together with -d)\n"
      "  -P <pidfile> write a pidfile (only useful together with -l or -L)\n"
      "  -o <type> output in machine friendly format.  'kv' for key=value pairs.\n"
      "  -v verbose\n"
      "  -q quiet\n",
      VMTOUCH_VERSION);
  exit(1);
}

// Fatal situations do not exit() here: the code throws FatalError and
// main()'s single handler prints the message and exits 1.
static void fatal(std::string_view msg) {
  throw FatalError(std::string(msg));
}

static void warning(bool quiet, std::string_view msg) {
  if (!quiet)
    std::cerr << std::format("vmtouch: WARNING: {}\n", msg);
}

// Converts a structured Failure record to the historical warning text.
// The library never sees these strings.
static std::string format_failure(const Failure& f) {
  switch (f.kind) {
    case Failure::Kind::Open:
      return "unable to open " + f.path + " (" + strerror(f.err) + "), skipping";
    case Failure::Kind::Stat:
      return "unable to stat " + f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::FstatFailed:
      return "unable to fstat " + f.path + " (" + strerror(f.err) + "), skipping";
    case Failure::Kind::IoctlSizeFailed:
      return "unable to ioctl " + f.path + " (" + strerror(f.err) + "), skipping";
    case Failure::Kind::Map:
      return "unable to mmap file " + f.path + " (" + strerror(f.err) + "), skipping";
    case Failure::Kind::MunmapFailed:
      return "unable to munmap file " + f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::OpendirFailed:
      return "unable to opendir " + f.path + " (" + strerror(f.err) + "), skipping";
    case Failure::Kind::TooLarge:
      return "file " + f.path + " too large, skipping";
    case Failure::Kind::BeyondEof:
      return "file " + f.path + " smaller than offset, skipping";
    case Failure::Kind::NotRegular:
      return "skipping non-regular file: " + f.path;
    case Failure::Kind::Symlink:
      return "not following symbolic link " + f.path;
    case Failure::Kind::SeparateFs:
      return "not recursing into separate filesystem " + f.path;
    case Failure::Kind::CrawlLoop:
      return "symbolic link loop detected: " + f.path;
    case Failure::Kind::PathTooLong:
      return "path too long " + f.path;
    case Failure::Kind::MincoreInaccurate:
      return "Process does not have write permission, residency chart will not be accurate";
    case Failure::Kind::EvictFailed:
      return "unable to evict " + f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::EvictAdviseFailed:
      return "unable to posix_fadvise file " + f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::EvictInvalidateFailed:
      return "unable to msync invalidate file " + f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::MlockFailed:
      // Historical text (a fatal there; a warning here).
      return "mlock: " + f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::MincoreFailed:
      // No upstream text exists: mincore failure was always fatal there.
      return "unable to mincore file " + f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::Fatal:
      // Terminal record: rendered as the FATAL text. The message text
      // already carries the site formatting from the engine.
      return f.path.empty()
                 ? std::string(strerror(f.err))
                 : f.path + " (" + strerror(f.err) + ")";
    case Failure::Kind::Closedir:
      return "unable to closedir " + f.path + " (" + strerror(f.err) + ")";
  }
  return "";
}

static void reopen_all() {
  if (freopen("/dev/null", "r", stdin) == NULL ||
      freopen("/dev/null", "w", stdout) == NULL ||
      freopen("/dev/null", "w", stdout) == NULL)
    fatal(std::format("freopen: {}", strerror(errno)));
}

static int wait_for_child() {
  int exit_read = 0;
  char exit_value = 0;
  int wait_status;

  while (1) {
    struct timeval tv;
    fd_set rfds;
    FD_ZERO(&rfds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    FD_SET(exit_pipe[0], &rfds);
    if (select(exit_pipe[0] + 1, &rfds, NULL, NULL, &tv) < 0)
      fatal(std::format("select: {}", strerror(errno)));

    if (waitpid(daemon_pid, &wait_status, WNOHANG) > 0)
      fatal("daemon shut down unexpectedly");

    if (FD_ISSET(exit_pipe[0], &rfds))
      break;
  }
  exit_read = read(exit_pipe[0], &exit_value, 1);
  if (exit_read < 0)
    fatal(std::format("read: {}", strerror(errno)));
  return exit_value;
}

void go_daemon(CliOptions& o) {
  daemon_pid = fork();
  if (daemon_pid == -1)
    fatal(std::format("fork: {}", strerror(errno)));
  if (daemon_pid) {
    if (o.wait)
      exit(wait_for_child());
    exit(0);
  }

  if (setsid() == -1)
    fatal(std::format("setsid: {}", strerror(errno)));

  if (!o.wait) reopen_all();
}


// Human-readable size: 123 -> "123", 4096 -> "4K", etc.
static std::string pretty_print_size(int64_t inp) {
  if (inp < 1024)
    return std::format("{}", inp);
  if (inp / 1024 < 1024)
    return std::format("{}K", inp / 1024);
  if (inp / (1024 * 1024) < 1024)
    return std::format("{}M", inp / (1024 * 1024));
  return std::format("{}G", inp / (1024 * 1024 * 1024));
}

// Parses "4096", "4k", "100M", "1.5G" (case-insensitive suffix).
int64_t parse_size(std::string_view inp) {
  const std::string_view errstr = "bad size. examples: 4096, 4k, 100M, 1.5G";

  if (inp.empty())
    fatal(errstr);

  int64_t mult = 1;
  const char mult_char = static_cast<char>(std::tolower(static_cast<unsigned char>(inp.back())));
  if (std::isalpha(static_cast<unsigned char>(mult_char))) {
    switch (mult_char) {
      case 'k': mult = 1024; break;
      case 'm': mult = 1024 * 1024; break;
      case 'g': mult = 1024 * 1024 * 1024; break;
      default: fatal(std::format("unknown size multiplier: {}", mult_char));
    }
    inp.remove_suffix(1);
  }

  double val = 0;
  const char* begin = inp.data();
  const char* end = inp.data() + inp.size();
  const auto [ptr, ec] = std::from_chars(begin, end, val);
  if (ec != std::errc{} || ptr != end || val < 0)
    fatal(errstr);

  val *= mult;
  if (val > static_cast<double>(INT64_MAX))
    fatal(errstr);

  return static_cast<int64_t>(val);
}

// Parses "SIZE", "-SIZE", "SIZE-SIZE" into a page-aligned file range.
void parse_range(std::string_view inp, FileInfoRange& f) {
  int64_t upper_range = 0;
  int64_t lower_range = 0;

  const size_t first_dash = inp.find('-');
  if (first_dash == std::string_view::npos) {
    upper_range = parse_size(inp);   // single value provided
  } else {
    const std::string_view before = inp.substr(0, first_dash);
    std::string_view rest = inp.substr(first_dash + 1);

    if (!before.empty())
      lower_range = parse_size(before);   // value before hyphen

    const size_t second_dash = rest.find('-');
    if (second_dash != std::string_view::npos)
      fatal("malformed range: multiple hyphens");

    if (!rest.empty())
      upper_range = parse_size(rest);   // value after hyphen
  }

  // offset must be multiple of pagesize
  f.offset = (lower_range / pagesize) * pagesize;

  if (upper_range) {
    if (upper_range <= f.offset)
      fatal("range limits out of order");

    f.max_len = upper_range - f.offset;
  }
}


// -i/-I filters are gone; these stubs remain only to keep the getopt
// switch shape simple.
void parse_ignore_item(const char * /*inp*/, Options& /*o*/) {}
void parse_filename_filter_item(const char * /*inp*/, Options& /*o*/) {}




#ifdef __linux__
// (can_do_mincore lives in core/platform.cpp.)
#endif


static std::unique_ptr<Session> g_session;




// Batch driver (-b): reads paths separated by newline or NUL (-0) from a
// list file or stdin ("-") and accumulates them into `extra`. Input-
// driving is a CLI concern: the library only ever sees process() per path.
static void read_batch_paths(const CliOptions& o, std::vector<std::string>& extra) {
  FILE* f;
  char* line = NULL;
  size_t len = 0;
  ssize_t read;
  int delim = o.zero_delim ? '\0' : '\n';

  if (!strcmp(o.batch, "-")) {
    f = stdin;
  } else {
    f = fopen(o.batch, "r");
    if (!f) {
      warning(false, std::format("unable to open {} ({})", o.batch, strerror(errno)));
      return;
    }
  }

  while ((read = getdelim(&line, &len, delim, f)) != -1) {
    line[read - 1] = '\0';   // strip the delimiter
    extra.emplace_back(line);
  }

  free(line);
  if (f != stdin)
    fclose(f);
}

static void remove_pidfile(CliOptions& o) {
  if (unlink(o.pidfile) < 0 && errno != ENOENT) {
    warning(o.quiet, std::format("unable to remove pidfile {} ({})", o.pidfile, strerror(errno)));
  }
}

static void write_pidfile(CliOptions& o) {
  std::ofstream f(o.pidfile);
  if (!f) {
    warning(o.quiet, std::format("unable to open pidfile {} ({})", o.pidfile, strerror(errno)));
    return;
  }

  f << getpid() << '\n';
  f.close();

  if (!f) {
    warning(o.quiet, std::format("unable to write to pidfile {} ({})", o.pidfile, strerror(errno)));
    remove_pidfile(o);
  }
}

// Signal handlers must be void(int), so the pidfile cleanup handler needs
// one narrow file-scope pointer. Assigned only immediately before
// sigaction registration, and only when -P is in use.
static const CliOptions* g_pidfile_options = nullptr;

static void signal_handler_clear_pidfile(int) {
  if (g_pidfile_options)
    remove_pidfile(const_cast<CliOptions&>(*g_pidfile_options));
}

static void register_signals_for_pidfile(const CliOptions& o) {
  g_pidfile_options = &o;
  struct sigaction sa = {};
  sa.sa_handler = signal_handler_clear_pidfile;
  if (sigaction(SIGINT, &sa, NULL) < 0 ||
      sigaction(SIGTERM, &sa, NULL) < 0 ||
      sigaction(SIGQUIT, &sa, NULL) < 0) {
    warning(o.quiet, std::format("unable to register signals for pidfile ({})", strerror(errno)));
  }
}






static int run(int argc, char **argv) {
  int ch, i;
  struct timeval start_time;
  struct timeval end_time;

  CliOptions o;          // CLI-level state (never rides on core Options)
  Options lib_opts;      // the library's Options (verb/range/max_file_size/collect)
  FileInfoRange f;
  const std::string prog = argv[0];

  if (pipe(exit_pipe))
    fatal(std::format("pipe: {}", strerror(errno)));

  platform::init();

  // -f/-F/-h/-i/-I are gone: traversal policy is fixed, the flags are
  // rejected as unknown options (-> usage).
  while((ch = getopt(argc, argv, "tevqlLd0p:b:m:P:wo:")) != -1) {
    switch(ch) {
      case '?': usage(); break;
      case 't': o.touch = 1; break;
      case 'e': o.evict = 1; break;
      case 'q': o.quiet = 1; break;
      case 'v': o.verbose++; break;
      case 'l': o.lock = 1;
                o.touch = 1; break;
      case 'L': o.lockall = 1;
                o.touch = 1; break;
      case 'd': o.daemon = 1; break;
      case 'p': parse_range(optarg, f); break;
      case 'm': {
        const int64_t val = parse_size(optarg);
        lib_opts.max_file_size = static_cast<size_t>(val);
        if (val != static_cast<int64_t>(lib_opts.max_file_size))
          fatal("value for -m too big to fit in a size_t");
        break;
      }
      case 'w': o.wait = 1; break;
      case 'b': o.batch = optarg; break;
      case '0': o.zero_delim = 1; break;
      case 'P': o.pidfile = optarg; break;
      case 'o': o.output = optarg; break;
    }
  }

  argc -= optind;
  argv += optind;

  if (o.touch && o.evict)
    fatal("invalid option combination: -t and -e");

  if (o.evict && o.lock)
    fatal("invalid option combination: -e and -l");

  if (o.lock && o.lockall) fatal("invalid option combination: -l and -L");

  if (o.daemon) {
    if (!(o.lock || o.lockall)) fatal("daemon mode must be combined with -l or -L");
    if (!o.wait) {
      o.quiet = 1;
      o.verbose = 0;
    }
  }

  if (o.wait && !o.daemon) fatal("wait mode must be combined with -d");

  if (o.quiet && o.verbose) fatal("invalid option combination: -q and -v");

  if (o.pidfile && (!o.lock && !o.lockall)) fatal("pidfile can only be created when -l or -L is specified");

  if (!argc && !o.batch) {
    std::cout << std::format("{}: no files or directories specified\n", prog);
    usage();
  }

  // Session created AFTER option parsing/validation so it holds the final
  // library Options. ONE process() call carries all roots (argv + batch):
  // cross-root dedup + one Report.
  lib_opts.range = f;
  lib_opts.collect_failures = true;   // the CLI drains records (quiet gates printing)
  if (o.touch) lib_opts.verb = Options::Verb::Warm;
  if (o.evict) lib_opts.verb = Options::Verb::Evict;
  if (o.lock)  lib_opts.verb = Options::Verb::Lock;
  if (o.lockall) lib_opts.verb = Options::Verb::Warm;   // -L -> Warm; mlockall below
  g_session = std::make_unique<Session>(lib_opts);

  // Must happen before fork(): mlock() is not inherited across fork().
  if (o.daemon) go_daemon(o);

  gettimeofday(&start_time, NULL);

  std::vector<std::string> roots;
  if (o.batch) read_batch_paths(o, roots);
  for (i=0; i<argc; i++) roots.emplace_back(argv[i]);

  const Status st = g_session->process(roots);

  gettimeofday(&end_time, NULL);

  // Failures (including the Fatal record) come from the Report; the quiet
  // policy gates PRINTING here in the CLI.
  if (!o.quiet) {
    for (const Failure& f : g_session->get_report().failures) {
      if (f.kind == Failure::Kind::Fatal)
        continue;   // rendered below as the FATAL line
      std::string msg = format_failure(f);
      if (!msg.empty())
        std::cerr << std::format("vmtouch: WARNING: {}\n", msg);
    }
  }

  const Session::Report& rep = g_session->get_report();
  int64_t total_pages_in_core_size = rep.total_pages_in_core * pagesize;
  int64_t total_pages_size         = rep.total_pages * pagesize;
  double  total_pages_in_core_perc = 100.0 * rep.total_pages_in_core / rep.total_pages;
  double  elapsed                  = (end_time.tv_sec - start_time.tv_sec) + (double)(end_time.tv_usec - start_time.tv_usec)/1000000.0;

  if (st == Status::Fatal) {
    // The Fatal record is always stored, never gated.
    for (const Failure& f : rep.failures) {
      if (f.kind == Failure::Kind::Fatal) {
        std::cerr << std::format("vmtouch: FATAL: {}\n", f.path);
        break;
      }
    }
    send_exit_signal(o, 1);
    return 1;
  }

  if (o.lock || o.lockall) {
    if (o.lockall) {
      if (mlockall(MCL_CURRENT))
        fatal(std::format("unable to mlockall ({})", strerror(errno)));
    }

    if (o.pidfile) {
      register_signals_for_pidfile(o);
      write_pidfile(o);
    }

    if (!o.quiet)
      std::cout << std::format("LOCKED {} pages ({})\n", rep.total_pages, pretty_print_size(total_pages_size));

    if (o.wait) reopen_all();

    send_exit_signal(o, 0);
    select(0, NULL, NULL, NULL, NULL);
    exit(0);
  }

  if (!o.quiet) {
    if (o.output == NULL) {
      if (o.verbose)
        std::cout << "\n";
      std::cout << std::format("           Files: {}\n", rep.total_files);
      std::cout << std::format("     Directories: {}\n", rep.total_dirs);
      if (o.touch) {
        std::cout << std::format("   Touched Pages: {} ({})\n", rep.total_pages, pretty_print_size(total_pages_size));
      } else if (o.evict) {
        std::cout << std::format("   Evicted Pages: {} ({})\n", rep.total_pages, pretty_print_size(total_pages_size));
      } else {
        std::cout << std::format("  Resident Pages: {}/{}  ", rep.total_pages_in_core, rep.total_pages);
        std::cout << std::format("{}/", pretty_print_size(total_pages_in_core_size));
        std::cout << std::format("{}  ", pretty_print_size(total_pages_size));
        if (rep.total_pages)
          std::cout << std::format("{:.3}%", total_pages_in_core_perc);
        std::cout << "\n";
      }
      std::cout << std::format("         Elapsed: {:.5} seconds\n", elapsed);
    } else if (std::string_view(o.output).starts_with("kv")) {
      const std::string_view desc = o.touch ? "Touched" :
                                    o.evict ? "Evicted" :
                                              "Resident";
      std::cout << std::format(
          "Files={} Directories={} {}Pages={} TotalPages={} {}Size={} TotalSize={} {}Percent={:.3} Elapsed={:.5}\n",
          rep.total_files, rep.total_dirs, desc, rep.total_pages_in_core, rep.total_pages,
          desc, total_pages_in_core_size, total_pages_size, desc, total_pages_in_core_perc, elapsed);
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  // Single fatal handler for the whole program: FatalError thrown anywhere
  // (option parsing, crawl, cache ops) lands here.
  try {
    return run(argc, argv);
  } catch (const FatalError& e) {
    std::cerr << std::format("vmtouch: FATAL: {}\n", e.what());
    if (daemon_pid == 0 && exit_pipe[1] != -1) {
      const char code = 1;
      const ssize_t ignored [[maybe_unused]] = write(exit_pipe[1], &code, 1);
    }
    return 1;
  }
}
