/***********************************************************************
vmtouch - the Virtual Memory Toucher

Portable file system cache diagnostics and control

by Doug Hoyte (doug@hcsw.org)

C++20 port: built with CMake (see CMakeLists.txt)

************************************************************************

Copyright 2009-2023 Doug Hoyte and contributors

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***********************************************************************/


#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>       // freopen for reopen_all
#include <cstring>      // strerror
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <signal.h>
#include <sys/mman.h>   // mlockall
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#include "cli_options.h"
#include "core/common.h"
#include "core/file_handle.h"
#include "core/platform.h"
#include "core/session.h"

// Only a waiting parent and its daemon child need a status pipe.
struct DaemonChannel {
  std::optional<vmt::FileHandle> reader;
  std::optional<vmt::FileHandle> writer;

  // Sends at most one status byte, then closes the writer. A parent that
  // has already gone away is not a reason to stop the daemon.
  void send_exit_status(char code) noexcept {
    if (!writer || reader)
      return;
    ssize_t n;
    do {
      n = ::write(writer->fd(), &code, 1);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EPIPE)
      std::fputs("vmtouch: FATAL: write\n", stderr);
    writer.reset();
  }
};

// Set by the signal handler; the main flow handles pidfile removal.
static volatile sig_atomic_t g_pidfile_needs_removal = 0;

// Error-path usage goes to stderr so `vmtouch --bogus 2>err.log` is useful.
[[noreturn]] static void usage(int code) {
  std::ostream& os = (code == 0) ? std::cout : std::cerr;
  os << std::format(
      "\n"
      "vmtouch v{} - the Virtual Memory Toucher by Doug Hoyte\n"
      "Portable file system cache diagnostics and control\n\n"
      "Usage: vmtouch [OPTIONS] ... FILES OR DIRECTORIES ...\n\n"
      "Options:\n"
      "  -h, --help show this help and exit\n"
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
  os.flush();
  exit(code);
}
// Fatal situations do not exit() here: the code throws FatalError and
// main()'s single handler prints the message and exits 1.
[[noreturn]] static void fatal(std::string_view msg) {
  throw FatalError(std::string(msg));
}

static void warning(bool quiet, std::string_view msg) {
  if (!quiet)
    std::cerr << std::format("vmtouch: WARNING: {}\n", msg);
}

// errno text for messages (strerror wrapper; safe on all paths here).
static std::string_view errno_message(int err) {
  return strerror(err);
}

// Converts a structured Failure record to the historical warning text.
// The library never sees these strings.
static std::string format_failure(const Failure& f) {
  switch (f.kind) {
    case Failure::Kind::Open:
      return std::format("unable to open {} ({}), skipping", f.path, strerror(f.err));
    case Failure::Kind::Stat:
      return std::format("unable to stat {} ({})", f.path, strerror(f.err));
    case Failure::Kind::FstatFailed:
      return std::format("unable to fstat {} ({}), skipping", f.path, strerror(f.err));
    case Failure::Kind::IoctlSizeFailed:
      return std::format("unable to ioctl {} ({}), skipping", f.path, strerror(f.err));
    case Failure::Kind::Map:
      return std::format("unable to mmap file {} ({}), skipping", f.path, strerror(f.err));
    case Failure::Kind::MunmapFailed:
      return std::format("unable to munmap file {} ({})", f.path, strerror(f.err));
    case Failure::Kind::OpendirFailed:
      return std::format("unable to opendir {} ({}), skipping", f.path, strerror(f.err));
    case Failure::Kind::TooLarge:
      return std::format("file {} too large, skipping", f.path);
    case Failure::Kind::BeyondEof:
      return std::format("file {} smaller than offset, skipping", f.path);
    case Failure::Kind::NotRegular:
      return std::format("skipping non-regular file: {}", f.path);
    case Failure::Kind::Symlink:
      return std::format("not following symbolic link {}", f.path);
    case Failure::Kind::CrawlLoop:
      return std::format("symbolic link loop detected: {}", f.path);
    case Failure::Kind::EvictAdviseFailed:
      return std::format("unable to posix_fadvise file {} ({})", f.path, strerror(f.err));
    case Failure::Kind::EvictInvalidateFailed:
      return std::format("unable to msync invalidate file {} ({})", f.path, strerror(f.err));
    case Failure::Kind::MlockFailed:
      // Historical text (a fatal there; a warning here).
      return std::format("mlock: {} ({})", f.path, strerror(f.err));
    case Failure::Kind::MincoreFailed:
      // No historical text exists: mincore failure was always fatal there.
      return std::format("unable to mincore file {} ({})", f.path, strerror(f.err));
    case Failure::Kind::Fatal:
      // Terminal record: never rendered as a warning (the caller prints
      // the FATAL line from f.path directly). Unreachable here.
      return f.path;
    case Failure::Kind::Closedir:
      return std::format("unable to closedir {} ({})", f.path, strerror(f.err));
  }
  return "";
}

static void reopen_all() {
  if (freopen("/dev/null", "r", stdin) == nullptr ||
      freopen("/dev/null", "w", stdout) == nullptr ||
      freopen("/dev/null", "w", stderr) == nullptr)
    fatal(std::format("freopen: {}", errno_message(errno)));
}

static int wait_for_child(const vmt::FileHandle& reader) {
  unsigned char exit_value = 0;
  ssize_t n;
  do {
    n = ::read(reader.fd(), &exit_value, 1);
  } while (n < 0 && errno == EINTR);
  if (n < 0)
    fatal(std::format("read: {}", errno_message(errno)));
  if (n == 0)   // EOF: child closed the pipe without writing a status byte
    fatal("daemon shut down unexpectedly (closed status pipe)");
  return exit_value;
}

// The parent returns an exit status; the child returns no status and runs.
static std::optional<int> go_daemon(const CliOptions& o, DaemonChannel& channel) {
  if (o.wait) {
    int ends[2];
    if (::pipe(ends) != 0)
      fatal(std::format("pipe: {}", errno_message(errno)));
    channel.reader.emplace(vmt::FileHandle::adopt(ends[0]));
    channel.writer.emplace(vmt::FileHandle::adopt(ends[1]));
  }

  const pid_t child = ::fork();
  if (child == -1)
    fatal(std::format("fork: {}", errno_message(errno)));
  if (child != 0) {
    channel.writer.reset();
    return o.wait ? wait_for_child(*channel.reader) : 0;
  }

  if (o.wait) {
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore.sa_mask);
    if (::sigaction(SIGPIPE, &ignore, nullptr) < 0)
      fatal(std::format("sigaction: {}", errno_message(errno)));
    channel.reader.reset();
  }

  if (setsid() == -1)
    fatal(std::format("setsid: {}", errno_message(errno)));

  if (!o.wait) reopen_all();
  return std::nullopt;
}


// Human-readable size: 123 -> "123", 4096 -> "4K", 5G+ -> "5G".
// Integer floor, as upstream: 1536 bytes prints "1K".
static std::string pretty_print_size(int64_t bytes) {
  static constexpr std::string_view unit[] = {"", "K", "M", "G"};
  int64_t v = bytes;
  size_t u = 0;
  while (v >= 1024 && u < 3) {
    v /= 1024;
    ++u;
  }
  return std::format("{}{}", v, unit[u]);
}

static int64_t pages_to_bytes(int64_t pages) {
  const auto page_size = platform::page_size();
  if (pages < 0 || pages > INT64_MAX / page_size)
    fatal("page count is too large to report in bytes");
  return pages * page_size;
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

  const char* begin = inp.data();
  const char* end = inp.data() + inp.size();
  int64_t whole = 0;
  const auto integer = std::from_chars(begin, end, whole);
  if (integer.ec == std::errc{} && integer.ptr == end) {
    if (whole < 0 || whole > INT64_MAX / mult)
      fatal(errstr);
    return whole * mult;
  }

  double val = 0;
  const auto [ptr, ec] = std::from_chars(begin, end, val);
  if (ec != std::errc{} || ptr != end)
    fatal(errstr);

  val *= mult;
  // INT64_MAX rounds up when converted to double. Use the exclusive 2^63
  // bound directly; these comparisons also reject infinities and NaNs.
  if (!(val >= 0.0 && val < 0x1p63))
    fatal(errstr);

  return static_cast<int64_t>(val);
}

// Parses "SIZE", "-SIZE", "SIZE-SIZE" into a page-aligned file range.
FileInfoRange parse_range(std::string_view inp) {
  FileInfoRange f;
  int64_t upper_range = 0;
  int64_t lower_range = 0;

  if (inp.empty() || inp == "-")
    fatal("bad range. examples: 1M, 1M-2M, 2M-");

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

  const auto page_size = platform::page_size();
  f.offset = (lower_range / page_size) * page_size;

  if (upper_range) {
    if (upper_range <= f.offset)
      fatal("range limits out of order");

    f.max_len = upper_range - f.offset;
  }
  return f;
}

// Batch driver (-b): reads paths separated by newline or NUL (-0) from a
// list file or stdin ("-") and accumulates them into `extra`. Input-
// driving is a CLI concern: the library only ever sees process() per path.
// A list file that cannot be opened is fatal: the user asked for exactly
// these paths, and silently producing an empty run (exit 0) would hide it.
static void read_batch_paths(const CliOptions& o, std::vector<std::string>& extra) {
  const int delim = o.zero_delim ? '\0' : '\n';

  std::ifstream file;
  std::istream* f = &std::cin;
  if (o.batch != "-") {
    file.open(std::string(o.batch));
    if (!file)
      fatal(std::format("unable to open batch list file {} ({})", o.batch,
                        errno_message(errno)));
    f = &file;
  }

  std::string line;
  while (std::getline(*f, line, static_cast<char>(delim)))
    extra.push_back(line);
}

static void remove_pidfile(bool quiet, std::string_view path) {
  if (unlink(std::string(path).c_str()) < 0 && errno != ENOENT) {
    warning(quiet, std::format("unable to remove pidfile {} ({})", path, errno_message(errno)));
  }
}

static bool write_pidfile(const CliOptions& o) {
  std::ofstream f(std::string(o.pidfile));
  if (!f) {
    warning(o.quiet, std::format("unable to open pidfile {} ({})", o.pidfile, errno_message(errno)));
    return false;
  }

  f << getpid() << '\n';
  f.close();

  if (!f) {
    warning(o.quiet, std::format("unable to write to pidfile {} ({})", o.pidfile, errno_message(errno)));
    remove_pidfile(o.quiet, o.pidfile);
    return false;
  }
  return true;
}

// The handler is async-signal-safe: it only sets a flag.
static void signal_handler_request_pidfile_removal(int) {
  g_pidfile_needs_removal = 1;
}

class PidfileSignals {
public:
  PidfileSignals() {
    sigset_t managed{};
    ::sigemptyset(&managed);
    for (int signal : signals_)
      ::sigaddset(&managed, signal);
    if (::sigprocmask(SIG_BLOCK, &managed, &previous_) != 0)
      fatal(std::format("sigprocmask: {}", errno_message(errno)));

    struct sigaction action{};
    action.sa_handler = signal_handler_request_pidfile_removal;
    ::sigemptyset(&action.sa_mask);
    for (int signal : signals_) {
      if (::sigaction(signal, &action, nullptr) < 0) {
        const int err = errno;
        ::sigprocmask(SIG_SETMASK, &previous_, nullptr);
        fatal(std::format("sigaction: {}", errno_message(err)));
      }
    }
  }

  ~PidfileSignals() { ::sigprocmask(SIG_SETMASK, &previous_, nullptr); }
  PidfileSignals(const PidfileSignals&) = delete;
  PidfileSignals& operator=(const PidfileSignals&) = delete;

  void wait() const {
    sigset_t wait_mask = previous_;
    for (int signal : signals_)
      ::sigdelset(&wait_mask, signal);

    // Atomically unblock and wait so a signal delivered just before this
    // call cannot leave the daemon parked with its cleanup flag already set.
    while (!g_pidfile_needs_removal) {
      if (::sigsuspend(&wait_mask) < 0 && errno != EINTR)
        fatal(std::format("sigsuspend: {}", errno_message(errno)));
    }
  }

private:
  static constexpr int signals_[] = {SIGINT, SIGTERM, SIGQUIT};
  sigset_t previous_{};
};

// Parses argv into CLI options, library options, and the range.
// Returns the operand (path) list. Unknown flags and missing values
// terminate the process (usage/fatal).
static std::vector<std::string_view> parse_cli_args(int argc, char **argv,
                                                    CliOptions& o, Options& lib_opts,
                                                    FileInfoRange& f) {
  // -f/-F/-i/-I are gone: traversal policy is fixed, the flags are
  // rejected as unknown options (-> usage). -h/--help show usage + exit 0.
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  size_t i = 0;
  for (; i < args.size(); ++i) {
    std::string_view arg = args[i];

    if (arg == "--help") {
      usage(0);
    }
    if (arg == "--") {   // explicit end-of-options
      ++i;
      break;
    }
    if (!arg.starts_with('-') || arg == "-")
      break;             // first operand: everything from here is a path

    // Clustered short flags: -qv processes q then v. A flag that takes a
    // value consumes the rest of the cluster or the next argument.
    for (size_t c = 1; c < arg.size(); ++c) {
      const char ch = arg[c];
      auto value = [&]() -> std::string_view {
        if (c + 1 < arg.size()) {                 // attached: -p1M-2M
          const std::string_view v = arg.substr(c + 1);
          c = arg.size();
          return v;
        }
        if (i + 1 < args.size())                  // separate: -p 1M-2M
          return args[++i];
        fatal(std::format("option -{} requires an argument", ch));   // noreturn
      };

      switch (ch) {
        case 'h': usage(0); break;   // -h recycled as help (old meaning gone)
        case 't': o.touch = true; break;
        case 'e': o.evict = true; break;
        case 'q': o.quiet = true; break;
        case 'v': o.verbose++; break;
        case 'l': o.lock = true; o.touch = true; break;
        case 'L': o.lockall = true; o.touch = true; break;
        case 'd': o.daemon = true; break;
        case 'w': o.wait = true; break;
        case '0': o.zero_delim = true; break;
        case 'p': f = parse_range(value()); break;
        case 'm': {
          const int64_t val = parse_size(value());
          lib_opts.max_file_size = static_cast<size_t>(val);
          if (val != static_cast<int64_t>(lib_opts.max_file_size))
            fatal("value for -m too big to fit in a size_t");
          break;
        }
        case 'b': o.batch = value(); break;
        case 'P': o.pidfile = value(); break;
        case 'o': o.output = value(); break;
        default:
          usage(1);   // unknown flag (covers the dropped -f/-F/-i/-I)
      }
    }
  }

  return std::vector<std::string_view>(args.begin() + i, args.end());
}

// Validates flag combinations; adjusts output policy for quiet daemon mode.
static void validate_cli(CliOptions& o, const std::vector<std::string_view>& operands) {
  // Evict checks come first: -l/-L imply touch at parse time, so a combined
  // -e -l/-L must be reported as such, not as the generic -t/-e clash.
  if (o.evict && o.lock)
    fatal("invalid option combination: -e and -l");

  if (o.evict && o.lockall)
    fatal("invalid option combination: -e and -L");

  if (o.touch && o.evict)
    fatal("invalid option combination: -t and -e");

  if (o.lock && o.lockall) fatal("invalid option combination: -l and -L");

  if (o.daemon) {
    if (!(o.lock || o.lockall)) fatal("daemon mode must be combined with -l or -L");
    if (!o.wait) {
      o.quiet = true;
      o.verbose = 0;   // -v accepted but totals-only; daemon has no totals to print
    }
  }

  if (o.wait && !o.daemon) fatal("wait mode must be combined with -d");

  if (o.quiet && o.verbose) fatal("invalid option combination: -q and -v");

  if (!o.output.empty() && o.output != "kv")
    fatal(std::format("invalid -o type: {} (only 'kv' is supported)", o.output));

  if (!o.pidfile.empty() && (!o.lock && !o.lockall)) fatal("pidfile can only be created when -l or -L is specified");

  if (operands.empty() && o.batch.empty()) {
    std::cerr << "vmtouch: no files or directories specified\n";
    usage(1);
  }
}

// Maps CLI verb selection to library Options.
// ONE process() call later carries all roots (argv + batch): cross-root
// dedup + one Report.
static Options make_session_options(const CliOptions& o, Options opts) {
  opts.collect_failures = true;   // the CLI drains records (quiet gates printing)
  if (o.touch) opts.verb = Options::Verb::Warm;
  if (o.evict) opts.verb = Options::Verb::Evict;
  if (o.lock)  opts.verb = Options::Verb::Lock;
  if (o.lockall) opts.verb = Options::Verb::Warm;   // -L -> Warm; mlockall in the caller
  return opts;
}

// Prints per-file warnings from the Report (quiet gates printing). The
// Fatal record is skipped here: it renders as the FATAL line in the caller.
static void print_failure_warnings(const vmt::Session& session, bool quiet) {
  if (quiet)
    return;
  for (const Failure& f : session.get_report().failures) {
    if (f.kind == Failure::Kind::Fatal)
      continue;
    std::string msg = format_failure(f);
    if (!msg.empty())
      std::cerr << std::format("vmtouch: WARNING: {}\n", msg);
  }
}

// Human summary (default or kv format) for a non-lock run.
static void print_summary(const vmt::Session::Report& rep, const CliOptions& o) {
  if (o.quiet)
    return;

  const int64_t total_pages_in_core_size = pages_to_bytes(rep.total_pages_in_core);
  const int64_t total_pages_size         = pages_to_bytes(rep.total_pages);
  // Guard the division: an all-empty-files run has total_pages == 0, and
  // an unguarded kv Percent prints -nan.
  const double  total_pages_in_core_perc =
      rep.total_pages == 0
          ? 0.0
          : 100.0 * rep.total_pages_in_core / rep.total_pages;
  const double  elapsed                  = rep.elapsed_seconds;   // steady_clock, through fatals

  if (o.output.empty()) {
    if (o.verbose)
      std::cout << "\n";
    std::cout << std::format("           Files: {}\n", rep.total_files);
    std::cout << std::format("     Directories: {}\n", rep.total_dirs);
    if (o.touch) {
      std::cout << std::format("   Touched Pages: {} ({})\n", rep.total_pages, pretty_print_size(total_pages_size));
    } else if (o.evict) {
      std::cout << std::format("   Evicted Pages: {} ({})\n", rep.total_pages, pretty_print_size(total_pages_size));
    } else {
      std::cout << std::format("  Resident Pages: {}/{}  ", rep.total_pages_in_core, rep.total_pages)
                << std::format("{}/", pretty_print_size(total_pages_in_core_size))
                << std::format("{}  ", pretty_print_size(total_pages_size));
      if (rep.total_pages)
        std::cout << std::format("{:.3}%", total_pages_in_core_perc);
      std::cout << "\n";
    }
    std::cout << std::format("         Elapsed: {:.5} seconds\n", elapsed);
  } else if (o.output.starts_with("kv")) {
    const std::string_view desc = o.touch ? "Touched" :
                                  o.evict ? "Evicted" :
                                            "Resident";
    std::cout << std::format(
        "Files={} Directories={} {}Pages={} TotalPages={} {}Size={} TotalSize={} {}Percent={:.3} Elapsed={:.5}\n",
        rep.total_files, rep.total_dirs, desc, rep.total_pages_in_core, rep.total_pages,
        desc, total_pages_in_core_size, total_pages_size, desc, total_pages_in_core_perc, elapsed);
  }
}

static int run(int argc, char **argv, DaemonChannel& daemon) {
  CliOptions o;          // CLI-level state (never rides on core Options)
  Options lib_opts;      // the library's Options (verb/range/max_file_size/collect)

  const std::vector<std::string_view> operands = parse_cli_args(argc, argv, o, lib_opts, lib_opts.range);
  validate_cli(o, operands);

  // Must happen before fork(): mlock() is not inherited across fork().
  if (o.daemon) {
    if (const auto parent_status = go_daemon(o, daemon))
      return *parent_status;
  }

  vmt::Session session(make_session_options(o, lib_opts));

  std::vector<std::string> roots;
  if (!o.batch.empty()) read_batch_paths(o, roots);
  for (const std::string_view op : operands)
    roots.emplace_back(op);

  const Status st = session.process(roots);
  print_failure_warnings(session, o.quiet);

  const vmt::Session::Report& rep = session.get_report();

  if (st == Status::Fatal) {
    // The Fatal record is always stored, never gated.
    for (const Failure& f : rep.failures) {
      if (f.kind == Failure::Kind::Fatal) {
        std::cerr << std::format("vmtouch: FATAL: {}\n", f.path);
        break;
      }
    }
    daemon.send_exit_status(1);
    return 1;
  }

  if (o.lock || o.lockall) {
    if (o.lockall) {
      if (mlockall(MCL_CURRENT))
        fatal(std::format("unable to mlockall ({})", errno_message(errno)));
    }

    if (!o.quiet)
      std::cout << std::format("LOCKED {} pages ({})\n", rep.total_pages, pretty_print_size(pages_to_bytes(rep.total_pages)));

    std::optional<PidfileSignals> pidfile_signals;
    if (!o.pidfile.empty())
      pidfile_signals.emplace();
    // Redirect before writing the pidfile so a redirection failure cannot
    // leave a stale pidfile behind in the waiting-daemon path.
    if (o.wait) {
      std::cout.flush();
      reopen_all();
    }
    const bool owns_pidfile = pidfile_signals && write_pidfile(o);
    daemon.send_exit_status(0);
    if (pidfile_signals)
      pidfile_signals->wait();
    else
      select(0, nullptr, nullptr, nullptr, nullptr);   // stay resident until a signal
    if (owns_pidfile)
      remove_pidfile(o.quiet, o.pidfile);
    return 0;
  }

  print_summary(rep, o);
  return 0;
}

int main(int argc, char **argv) {
  // Single fatal handler for the whole program: FatalError thrown anywhere
  // (option parsing, crawl, cache ops) lands here; bad_alloc and other
  // std::exceptions get a message instead of std::terminate.
  DaemonChannel daemon;
  try {
    return run(argc, argv, daemon);
  } catch (const FatalError& e) {
    std::cerr << std::format("vmtouch: FATAL: {}\n", e.what());
    daemon.send_exit_status(1);
    return 1;
  } catch (const std::exception& e) {
    std::cerr << std::format("vmtouch: FATAL: {}\n", e.what());
    daemon.send_exit_status(1);
    return 1;
  }
}
