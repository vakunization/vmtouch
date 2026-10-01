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


#define RESIDENCY_CHART_WIDTH 60
#define CHART_UPDATE_INTERVAL 0.1

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
// for O_NOATIME
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <limits.h>
#include <inttypes.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <search.h>
#include <libgen.h>
#include <fnmatch.h>

#if defined(__linux__)
// Used to find size of block devices
#include <sys/ioctl.h>
#include <sys/mount.h>
// Used to check kernal version to know if mincore reports correctly
#include <sys/utsname.h>
#endif

#include "core/common.h"
#include "core/platform.h"
#include "core/filters.h"
#include "core/cache.h"
#include "core/session.h"
#include "core/crawl.h"

long pagesize;




int exit_pipe[2];

int daemon_pid;


// wait is passed explicitly: this must work both in the daemon child (which
// owns `o`) and from main()'s catch handler without any global Options*.
void send_exit_signal(const Options& o, char code) {
  if (daemon_pid == 0 && o.wait) {
    if (write(exit_pipe[1], &code, 1) < 0)
      fprintf(stderr, "vmtouch: FATAL: write: %s", strerror(errno));
  }
}

void usage() {
  printf("\n");
  printf("vmtouch v%s - the Virtual Memory Toucher by Doug Hoyte\n", VMTOUCH_VERSION);
  printf("Portable file system cache diagnostics and control\n\n");
  printf("Usage: vmtouch [OPTIONS] ... FILES OR DIRECTORIES ...\n\nOptions:\n");
  printf("  -t touch pages into memory\n");
  printf("  -e evict pages from memory\n");
  printf("  -l lock pages in physical memory with mlock(2)\n");
  printf("  -L lock pages in physical memory with mlockall(2)\n");
  printf("  -d daemon mode\n");
  printf("  -m <size> max file size to touch\n");
  printf("  -p <range> use the specified portion instead of the entire file\n");
  printf("  -f follow symbolic links\n");
  printf("  -F don't crawl different filesystems\n");
  printf("  -h also count hardlinked copies\n");
  printf("  -i <pattern> ignores files and directories that match this pattern\n");
  printf("  -I <pattern> only process files that match this pattern\n");
  printf("  -b <list file> get files or directories from the list file\n");
  printf("  -0 in batch mode (-b) separate paths with NUL byte instead of newline\n");
  printf("  -w wait until all pages are locked (only useful together with -d)\n");
  printf("  -P <pidfile> write a pidfile (only useful together with -l or -L)\n");
  printf("  -o <type> output in machine friendly format.  'kv' for key=value pairs.\n");
  printf("  -v verbose\n");
  printf("  -q quiet\n");
  exit(1);
}

// Fatal situations no longer exit() here: the library/CLI code throws
// FatalError and main()'s single handler prints the message, sends the
// daemon exit signal and exits 1. Message text is byte-identical to the
// old exit-based fatal().
static void fatal(const char *fmt, ...) {
  va_list ap;
  char buf[4096];

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  throw FatalError(buf);
}

// Old vararg warning: the quiet flag must be the SECOND arg. Kept until
// commit 6 moves the quiet handling into Session's warning helper.
static void warning(const char *fmt, bool quiet, ...) {
  va_list ap;
  char buf[4096];

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (!quiet) fprintf(stderr, "vmtouch: WARNING: %s\n", buf);
}

static void reopen_all() {
  if (freopen("/dev/null", "r", stdin) == NULL ||
      freopen("/dev/null", "w", stdout) == NULL ||
      freopen("/dev/null", "w", stdout) == NULL)
    fatal("freopen: %s", strerror(errno));
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
      fatal("select: %s", strerror(errno));

    if (waitpid(daemon_pid, &wait_status, WNOHANG) > 0)
      fatal("daemon shut down unexpectedly");

    if (FD_ISSET(exit_pipe[0], &rfds))
      break;
  }
  exit_read = read(exit_pipe[0], &exit_value, 1);
  if (exit_read < 0)
    fatal("read: %s", strerror(errno));
  return exit_value;
}

void go_daemon(Options& o) {
  daemon_pid = fork();
  if (daemon_pid == -1)
    fatal("fork: %s", strerror(errno));
  if (daemon_pid) {
    if (o.wait)
      exit(wait_for_child());
    exit(0);
  }

  if (setsid() == -1)
    fatal("setsid: %s", strerror(errno));

  if (!o.wait) reopen_all();
}


char *pretty_print_size(int64_t inp) {
  static char output[100];

  if (inp<1024) {
    snprintf(output, sizeof(output), "%" PRId64, inp);
    return output;
  }
  inp /= 1024;
  if (inp<1024) {
    snprintf(output, sizeof(output), "%" PRId64 "K", inp);
    return output;
  }
  inp /= 1024;
  if (inp<1024) {
    snprintf(output, sizeof(output), "%" PRId64 "M", inp);
    return output;
  }
  inp /= 1024;
  snprintf(output, sizeof(output), "%" PRId64 "G", inp);
  return output;
}

/*
 *  Convert ASCII string to int64_t number
 *  Note: The inp parameter can't be a character constant
 *        because it will be overwritten.
 */
int64_t parse_size(char *inp) {
  char *tp;
  int len=strlen(inp);
  const char *errstr = "bad size. examples: 4096, 4k, 100M, 1.5G";
  char mult_char;
  int mult=1;
  double val;

  if (len < 1) fatal(errstr);

  mult_char = tolower(inp[len-1]);

  if (isalpha(mult_char)) {
    switch(mult_char) {
      case 'k': mult = 1024; break;
      case 'm': mult = 1024*1024; break;
      case 'g': mult = 1024*1024*1024; break;
      default: fatal("unknown size multiplier: %c", mult_char);
    }
    inp[len-1] = '\0';
  }

  val = strtod(inp, &tp);

  if (val < 0 || val == HUGE_VAL || *tp != '\0') fatal(errstr);

  val *= mult;

  if (val > INT64_MAX) fatal(errstr);

  return (int64_t) val;
}

int64_t bytes2pages(int64_t bytes) {
  return (bytes+pagesize-1) / pagesize;
}

void parse_range(char *inp, FileInfoRange& f) {
  char *token;
  int64_t upper_range=0;
  int64_t lower_range=0;

  token = strsep(&inp,"-");

  if (inp == NULL)
    upper_range = parse_size(token); // single value provided
  else {
    if (*token != '\0')
      lower_range = parse_size(token); // value before hyphen

    token = strsep(&inp,"-");
    if (*token != '\0')
      upper_range = parse_size(token); // value after hyphen

    if ((token = strsep(&inp,"-")) != NULL) fatal("malformed range: multiple hyphens");
  }

  // offset must be multiple of pagesize
  f.offset = (lower_range / pagesize) * pagesize;

  if (upper_range) {
    if (upper_range <= f.offset) fatal("range limits out of order");

    f.max_len = upper_range - f.offset;
  }
}


// -i/-I parsing and matching moved to core/filters.{h,cpp} (FileFilter).
// These thin wrappers keep the getopt switch unchanged; fatal conditions
// (too long / too many patterns) are now thrown as FatalError from inside
// FileFilter with identical message text.
void parse_ignore_item(const char *inp, FileFilter& f) {
  if (inp == NULL) return;
  f.add_ignore(inp);
}

void parse_filename_filter_item(const char *inp, FileFilter& f) {
  if (inp == NULL) return;
  f.add_filename_filter(inp);
}


// Raises RLIMIT_NOFILE via the platform module and translates the outcome
// into the exact fatal messages the monolith produced.
static void increment_nofile_rlimit() {
  const auto res = platform::raise_nofile_rlimit();
  switch (res.outcome) {
    case platform::RaiseNofileResult::Outcome::Raised:
      return;
    case platform::RaiseNofileResult::Outcome::GetrlimitFailed:
      fatal("increment_nofile_rlimit: getrlimit (%s)", strerror(res.err));
    case platform::RaiseNofileResult::Outcome::PermissionDenied:
      if (getuid() == 0 || geteuid() == 0) fatal("system open file limit reached");
      fatal("open file limit reached and unable to increase limit. retry as root");
    case platform::RaiseNofileResult::Outcome::SetrlimitFailed:
      fatal("increment_nofile_rlimit: setrlimit (%s)", strerror(res.err));
  }
}

double gettimeofday_as_double() {
  return platform::now_seconds();
}



void print_page_residency_chart(FILE *out, char *mincore_array, int64_t pages_in_file) {
  int64_t pages_in_core=0;
  int64_t pages_per_char;
  int64_t i,j=0,curr=0;

  if (pages_in_file <= RESIDENCY_CHART_WIDTH) pages_per_char = 1;
  else pages_per_char = (pages_in_file / RESIDENCY_CHART_WIDTH) + 1;

  fprintf(out, "\r[");

  for (i=0; i<pages_in_file; i++) {
    if (platform::is_mincore_page_resident(mincore_array[i])) {
      curr++;
      pages_in_core++;
    }
    j++;
    if (j == pages_per_char) {
      if (curr == pages_per_char) fprintf(out, "O");
      else if (curr == 0) fprintf(out, " ");
      else fprintf(out, "o");

      j = curr = 0;
    }
  }

  if (j) {
    if (curr == j) fprintf(out, "O");
    else if (curr == 0) fprintf(out, " ");
    else fprintf(out, "o");
  }

  fprintf(out, "] %" PRId64 "/%" PRId64, pages_in_core, pages_in_file);

  fflush(out);
}


#ifdef __linux__
// Moved verbatim to core/platform.cpp as platform::can_do_mincore().
#endif


// CLI-side ProgressSink: renders sink calls exactly as the monolith printed
// them, keeping -v output byte-identical (tests/run.sh parses it).
void print_page_residency_chart(FILE *out, char *mincore_array, int64_t pages_in_file); // fwd: defined below

class ChartSink final : public cache::ProgressSink {
public:
  void on_file_start(std::string_view path) override {
    printf("%.*s\n", static_cast<int>(path.size()), path.data());
  }
  void on_evicting(std::string_view path) override {
    printf("Evicting %.*s\n", static_cast<int>(path.size()), path.data());
  }
  void on_chart(std::span<const unsigned char> pages, int64_t pages_in_file) override {
    print_page_residency_chart(stdout,
        const_cast<char*>(reinterpret_cast<const char*>(pages.data())), pages_in_file);
  }
  void on_file_done(std::string_view, int64_t, int64_t) override {
    printf("\n");   // was the trailing printf("\n") of the verbose block
  }
};

static ChartSink g_chart_sink;
static std::unique_ptr<Session> g_session;
static std::unique_ptr<Crawler> g_crawler;




static void remove_pidfile(Options& o) {
  int res = 0;

  res = unlink(o.pidfile);
  if (res < 0 && errno != ENOENT) {
    warning("unable to remove pidfile %s (%s)", o.quiet, o.pidfile, strerror(errno));
  }
}

static void write_pidfile(Options& o) {
  FILE *f = NULL;
  size_t wrote = 0;

  f = fopen(o.pidfile, "w");
  if (!f) {
    warning("unable to open pidfile %s (%s), skipping", o.quiet, o.pidfile, strerror(errno));
    return;
  }

  wrote = fprintf(f, "%d\n", getpid());

  fclose(f);

  if (wrote < 0) {
    warning("unable to write to pidfile %s (%s), deleting it", o.quiet, o.pidfile, strerror(errno));
    remove_pidfile(o);
  }
}

// Signal handlers must be void(int), so the pidfile cleanup handler needs
// one narrow file-scope pointer. It is assigned only here, immediately
// before sigaction registration, and only when -P is in use — replacing the
// old never-assigned o_ptr (null deref on every fatal path, caught by UBSan).
static const Options* g_pidfile_options = nullptr;

static void signal_handler_clear_pidfile(int) {
  if (g_pidfile_options)
    remove_pidfile(const_cast<Options&>(*g_pidfile_options));
}

static void register_signals_for_pidfile(const Options& o) {
  g_pidfile_options = &o;
  struct sigaction sa = {0};
  sa.sa_handler = signal_handler_clear_pidfile;
  if (sigaction(SIGINT, &sa, NULL) < 0 ||
      sigaction(SIGTERM, &sa, NULL) < 0 ||
      sigaction(SIGQUIT, &sa, NULL) < 0) {
    warning("unable to register signals for pidfile (%s), skipping", o.quiet, strerror(errno));
  }
}






static int run(int argc, char **argv) {
  int ch, i;
  char *prog = argv[0];
  struct timeval start_time;
  struct timeval end_time;

  Options o;
  FileInfoRange f;
  FileFilter cli_filter;   // filled during getopt, ownership moves into Session

  if (pipe(exit_pipe))
    fatal("pipe: %s", strerror(errno));

  platform::init();

  while((ch = getopt(argc, argv, "tevqlLdfFh0i:I:p:b:m:P:wo:")) != -1) {
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
      case 'f': o.followsymlinks = 1; break;
      case 'F': o.singlefilesystem = 1; break;
      case 'h': o.ignorehardlinkeduplictes = 1; break;
      case 'p': parse_range(optarg, f); break;
      case 'i': parse_ignore_item(optarg, cli_filter); break;
      case 'I': parse_filename_filter_item(optarg, cli_filter); break;
      case 'm': {
        int64_t val = parse_size(optarg);
        o.max_file_size = (size_t) val;
        if (val != (int64_t) o.max_file_size) fatal("value for -m too big to fit in a size_t");
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

  if (o.touch && o.evict) {
  	fatal("invalid option combination: -t and -e");
  }

  if (o.evict && o.lock) {
    fatal("invalid option combination: -e and -l");
  }

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
    printf("%s: no files or directories specified\n", prog);
    usage();
  }

  // Session created AFTER option parsing/validation so it holds the final
  // Options; owns the ChartSink and the warning printer (quiet handled
  // inside Session::warning).
  g_session = std::make_unique<Session>(o, std::move(cli_filter), std::make_unique<ChartSink>(),
      [](std::string_view msg) {
        fprintf(stderr, "vmtouch: WARNING: %.*s\n",
                static_cast<int>(msg.size()), msg.data());
      });
  g_session->set_range(f);
  g_crawler = std::make_unique<Crawler>(*g_session);

  // Must be done now because mlock() not inherited across fork()
  if (o.daemon) go_daemon(o);

  gettimeofday(&start_time, NULL);

  if (o.batch) {
      g_crawler->walk_batch(o.batch);
  }

  for (i=0; i<argc; i++) g_crawler->walk(argv[i]);

  gettimeofday(&end_time, NULL);

  const Stats& s_stats = g_session->stats();   // session is the single source of truth now
  int64_t total_pages_in_core_size = s_stats.total_pages_in_core * pagesize;
  int64_t total_pages_size         = s_stats.total_pages * pagesize;
  double  total_pages_in_core_perc = 100.0 * s_stats.total_pages_in_core / s_stats.total_pages;
  double  elapsed                  = (end_time.tv_sec - start_time.tv_sec) + (double)(end_time.tv_usec - start_time.tv_usec)/1000000.0;

  if (o.lock || o.lockall) {
    if (o.lockall) {
      if (mlockall(MCL_CURRENT))
        fatal("unable to mlockall (%s)", strerror(errno));
    }

    if (o.pidfile) {
      register_signals_for_pidfile(o);
      write_pidfile(o);
    }

    if (!o.quiet) printf("LOCKED %" PRId64 " pages (%s)\n", s_stats.total_pages, pretty_print_size(total_pages_size));

    if (o.wait) reopen_all();

    send_exit_signal(o, 0);
    select(0, NULL, NULL, NULL, NULL);
    exit(0);
  }

  if (!o.quiet) {
    if (o.output == NULL) {
      if (o.verbose) printf("\n");
      printf("           Files: %" PRId64 "\n", g_crawler->total_files());
      printf("     Directories: %" PRId64 "\n", g_crawler->total_dirs());
      if (o.touch)
        printf("   Touched Pages: %" PRId64 " (%s)\n", s_stats.total_pages, pretty_print_size(total_pages_size));
      else if (o.evict)
        printf("   Evicted Pages: %" PRId64 " (%s)\n", s_stats.total_pages, pretty_print_size(total_pages_size));
      else {
        printf("  Resident Pages: %" PRId64 "/%" PRId64 "  ", s_stats.total_pages_in_core, s_stats.total_pages);
        printf("%s/", pretty_print_size(total_pages_in_core_size));
        printf("%s  ", pretty_print_size(total_pages_size));
        if (s_stats.total_pages)
          printf("%.3g%%", total_pages_in_core_perc);
        printf("\n");
      }
      printf("         Elapsed: %.5g seconds\n", elapsed);
    } else if (strncmp(o.output, "kv", 2) == 0) {
      const char *desc = o.touch ? "Touched" :
                   o.evict ? "Evicted" :
                             "Resident";
      printf("Files=%" PRId64 " Directories=%" PRId64 " %sPages=%" PRId64 " TotalPages=%" PRId64 " %sSize=%" PRId64 " TotalSize=%" PRId64 " %sPercent=%.3g Elapsed=%.5g\n", 
       g_crawler->total_files(), g_crawler->total_dirs(), desc, s_stats.total_pages_in_core, s_stats.total_pages, desc, total_pages_in_core_size, total_pages_size, desc, total_pages_in_core_perc, elapsed);
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  // Single fatal handler for the whole program: FatalError thrown anywhere
  // (option parsing, crawl, cache ops) lands here. Byte-identical output to
  // the old exit-based fatal(): "vmtouch: FATAL: <msg>\n" + daemon exit
  // signal + status 1.
  try {
    return run(argc, argv);
  } catch (const FatalError& e) {
    fprintf(stderr, "vmtouch: FATAL: %s\n", e.what());
    if (daemon_pid == 0 && exit_pipe[1] != -1) {
      const char code = 1;
      const ssize_t ignored [[maybe_unused]] = write(exit_pipe[1], &code, 1);
    }
    return 1;
  }
}
