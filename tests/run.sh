#!/usr/bin/env bash

set -u -o pipefail
export LC_ALL=C

# CLI characterization: check exit statuses, aggregate output, warnings,
# and daemon lifecycle against the current fork contract (man/vmtouch.8).
# The old -f/-F/-i/-I flags are rejected; -h now means help.

VMFILE=${VMFILE:-./vmtouch}
case $VMFILE in
  /*) ;;
  *)  VMFILE=$PWD/${VMFILE#./} ;;
esac
[ -x "$VMFILE" ] || { echo "not executable: $VMFILE" >&2; exit 2; }

# Eviction tests need a disk-backed filesystem, not tmpfs. Builds on tmpfs
# can keep the executable there and point only test fixtures at a disk.
fixture_dir=${VMTOUCH_TEST_DIR:-$PWD}
T=$(mktemp -d "${fixture_dir%/}/vmtouch-test.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
case $(uname -s) in
  Linux)  fs=$(stat -f -c %T "$T" 2>/dev/null) ;;
  *)      fs= ;;
esac
[ "$fs" = tmpfs ] && {
  echo "$T is on tmpfs; set VMTOUCH_TEST_DIR to a writable, disk-backed directory" >&2
  exit 2
}

PAGESIZE=$(getconf PAGESIZE)

PASS=0 FAIL=0 SKIP=0
ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL %s\n       %s\n' "$1" "$2"; }
skip() { SKIP=$((SKIP+1)); printf '  skip %s\n' "$*"; }

eq()     { [ "$1" = "$2" ] && ok "$3" || fail "$3" "got '$1', want '$2'"; }
has()    { grep -qF -- "$2" <<<"$1" && ok "$3" || fail "$3" "missing '$2' in: ${1:0:200}"; }
lacks()  { grep -qF -- "$2" <<<"$1" && fail "$3" "unexpected '$2'" || ok "$3"; }
rc_is()  { [ "$1" = "$2" ] && ok "$3" || fail "$3" "got rc $1, want $2"; }

plain_vmfile() {
  # ASan intercepts the lock syscalls. Prefer an explicitly supplied plain
  # binary, then the conventional sibling builds, then a plain VMFILE.
  if [ -n "${VMFILE_PLAIN:-}" ] && [ -x "$VMFILE_PLAIN" ]; then
    printf '%s\n' "$VMFILE_PLAIN"
  elif [ -x "$(dirname "$VMFILE")/vmtouch-plain" ]; then
    printf '%s\n' "$(dirname "$VMFILE")/vmtouch-plain"
  elif [ -x "${VMFILE%build*}build-plain/vmtouch" ]; then
    printf '%s\n' "${VMFILE%build*}build-plain/vmtouch"
  elif [ ! -e "${VMFILE}.asan" ] && ! grep -qa __asan_init "$VMFILE" 2>/dev/null; then
    printf '%s\n' "$VMFILE"
  fi
}

# Summary readers (the ONLY output the CLI still produces per call).
total_pages()  { printf '%s\n' "$1" | grep -oE 'TotalPages=[0-9]+' | head -1 | cut -d= -f2; }
resident_pages() { printf '%s\n' "$1" | grep -oE 'ResidentPages=[0-9]+' | head -1 | cut -d= -f2; }
files_count()  { printf '%s\n' "$1" | grep -oE 'Files=[0-9]+' | head -1 | cut -d= -f2; }

mk() { dd if=/dev/zero of="$1" bs="$PAGESIZE" count="$2" 2>/dev/null; }

mk "$T/one"   1
mk "$T/ten"  10
mk "$T/big" 1000
: > "$T/empty"
mkdir -p "$T/d/sub"
mk "$T/d/a"     2
mk "$T/d/sub/b" 5

# --- usage / exit codes ------------------------------------------------------

# -h/--help are real help flags now: usage on stdout, exit 0. The old
# upstream -h ("count hardlinked copies") is gone, so -h is recycled.
"$VMFILE" -h >/dev/null 2>&1;     rc_is $? 0 "-h prints usage and exits 0"
"$VMFILE" --help >/dev/null 2>&1; rc_is $? 0 "--help prints usage and exits 0"
has "$($VMFILE --help)" "Usage: vmtouch" "--help output contains usage"
"$VMFILE" -f x >/dev/null 2>&1;   rc_is $? 1 "-f rejected (flag dropped)"
"$VMFILE" -F x >/dev/null 2>&1;   rc_is $? 1 "-F rejected (flag dropped)"
"$VMFILE" -i x y >/dev/null 2>&1; rc_is $? 1 "-i rejected (flag dropped)"
"$VMFILE" -I x y >/dev/null 2>&1; rc_is $? 1 "-I rejected (flag dropped)"

"$VMFILE" >/dev/null 2>&1; rc_is $? 1 "no args prints usage"

# --- basic verbs + summary ---------------------------------------------------

"$VMFILE" -t "$T/ten" >/dev/null 2>&1
out=$("$VMFILE" -o kv "$T/ten" 2>/dev/null)
eq "$(resident_pages "$out")" 10 "-t makes all 10 pages resident"
eq "$(total_pages "$out")" 10 "-o kv reports total 10"

out=$("$VMFILE" "$T/ten" 2>/dev/null)
has "$out" "Resident Pages: 10/10" "query summary shows full residency"

out=$("$VMFILE" -o kv "$T/one" 2>&1)
has "$out" "Files=1 "        "-o kv Files"
has "$out" "Directories=0 "  "-o kv Directories"
has "$out" "ResidentPages="  "-o kv ResidentPages"
has "$out" "TotalSize="      "-o kv TotalSize"

# empty file: counted as a file, zero pages, NOT a failure
out=$("$VMFILE" -o kv "$T/empty" 2>&1); rc=$?
eq "$(files_count "$out")" 1 "empty file counted"
eq "$(total_pages "$out")" 0 "empty file: zero pages"
rc_is "$rc" 0 "empty file exits 0"

# -v is accepted (totals-only) and silent per-file
out=$("$VMFILE" -v "$T/one" 2>&1); rc_is $? 0 "-v accepted"
lacks "$out" "$T/one" "-v: no per-file path echo (ratified deviation)"
lacks "$out" "[" "-v: no residency chart in summary output"

# -v writes to stdout, warnings to stderr
out=$("$VMFILE" -v "$T/one" 2>/dev/null)
err=$("$VMFILE" -v "$T/one" 2>&1 >/dev/null)
[ -n "$out" ] && [ -z "$err" ] \
  && ok   "-v summary writes to stdout" \
  || fail "-v stream" "out='${out:0:60}' err='${err:0:60}'"

p5=$((PAGESIZE*5-1))         # last byte of 5 pages
p1=$((PAGESIZE-1))           # last byte of page 0
np1m=$((1048576/PAGESIZE))   # pages in 1 MiB

# --- -p ranges (via summary counts) ------------------------------------------

"$VMFILE" -t -p "0-$p5" "$T/ten" >/dev/null 2>&1
out=$("$VMFILE" -o kv "$T/ten" 2>/dev/null)
# the -t pass touched 5 pages; a full-file query must still see >= 5 resident
ge() { [ "$1" -ge "$2" ] && ok "$3" || fail "$3" "got '$1', want >= '$2'"; }
ge "$(resident_pages "$out")" 5 "-p 0-$p5 touched at least 5 pages"

out=$("$VMFILE" -o kv -p 0-1M "$T/big" 2>/dev/null)
eq "$(total_pages "$out")" "$np1m" "-p 0-1M covers $np1m pages"

# multiple -p: last one wins (the range is session Options.range)
out=$("$VMFILE" -t -o kv -p "0-$p1" -p "0-$p5" "$T/ten" 2>&1); rc=$?
rc_is "$rc" 0 "multiple -p: exits 0"
eq "$(total_pages "$out")" 5 "multiple -p: last one wins (5 pages selected)"

# Numeric parsing: reject out-of-range values before converting to int64_t.
for size in 9223372036854775808 8589934592G nanM; do
  out=$("$VMFILE" -p "$size" "$T/one" 2>&1); rc=$?
  rc_is "$rc" 1 "invalid size rejected: $size"
  has "$out" "bad size" "invalid size diagnostic: $size"
done
out=$("$VMFILE" -o kv -p 9223372036854775807 "$T/one" 2>&1); rc=$?
rc_is "$rc" 0 "INT64_MAX range length accepted"
eq "$(total_pages "$out")" 1 "maximum range length capped at EOF"

# --- crawling (counts via summary) -------------------------------------------

out=$("$VMFILE" -o kv "$T/d" 2>/dev/null)
eq "$(files_count "$out")" 2 "crawl reaches files in nested dirs"
eq "$(total_pages "$out")" 7 "crawl processes nested files (2+5 pages)"

out=$("$VMFILE" -o kv "$T/one" "$T/ten" 2>/dev/null)
eq "$(files_count "$out")" 2 "two files counted"

# hardlink dedup (fixed policy: always on)
mk "$T/hl" 3
ln "$T/hl" "$T/hl2"
out=$("$VMFILE" -o kv "$T/hl" "$T/hl2" 2>/dev/null)
eq "$(files_count "$out")" 1 "hardlink dedup: two hardlinks counted as one file"

# symlinks: not followed (fixed policy); warning + exit 0
ln -s "$T/ten" "$T/link"
out=$("$VMFILE" "$T/link" 2>&1); rc_is $? 0 "symlink (not followed) exits 0"
has "$out" "not following symbolic link" "symlink warns"
out=$("$VMFILE" -o kv "$T/link" 2>/dev/null)
eq "$(files_count "$out")" 0 "symlink root: no file processed"

# symlink loop: terminates (exit 0), link yields the warning
ln -s "$T/d" "$T/d/loop"
out=$(timeout 10 "$VMFILE" "$T/d" 2>&1); rc_is $? 0 "symlink loop: walk terminates"
has "$out" "not following symbolic link" "symlink loop: link not followed"

# trailing slash: dir still crawled
out=$("$VMFILE" -o kv "$T/d/" 2>/dev/null)
eq "$(files_count "$out")" 2 "trailing slash: directory still crawled"

# --- eviction -----------------------------------------------------------------

# 0 = evicted, 1 = still hot, 2 = inconclusive
evict() {
  local i out r
  for i in 1 2 3 4 5; do
    sync 2>/dev/null
    "$VMFILE" -e "$1" >/dev/null 2>&1
    out=$("$VMFILE" -o kv "$1" 2>/dev/null)
    r=$(resident_pages "$out")
    [ -z "$r" ] && return 2
    [ "$r" = 0 ] && return 0
    sleep 0.05
  done
  return 1
}

evict "$T/big"
case $? in
  0)
    mid=$((PAGESIZE * 500))
    "$VMFILE" -t -p "$mid-$((mid + PAGESIZE*5 - 1))" "$T/big" >/dev/null 2>&1

    out=$("$VMFILE" -o kv -p "0-$p1" "$T/big" 2>/dev/null)
    eq "$(resident_pages "$out")" 0 "-t -p: page 0 left cold"

    out=$("$VMFILE" -o kv "$T/big" 2>/dev/null)
    n=$(resident_pages "$out")
    # 6 pages touched + kernel read-ahead; nowhere near 1000.
    [ "$n" -ge 6 ] && [ "$n" -lt 100 ] \
      && ok   "-t -p: $n pages resident (range+readahead)" \
      || fail "-t -p" "unexpected resident count: $n"
    ;;
  2)
    fail "-t -p" "output parser broke (summary format changed?)"
    ;;
  *)
    skip "-t -p (cannot evict $T/big)"
    ;;
esac

# --- warnings / skip semantics ------------------------------------------------

out=$("$VMFILE" "$T/nonexistent" 2>&1); rc=$?
rc_is "$rc" 0 "missing path exits 0"
has "$out" "unable to stat" "missing path warns"

mk "$T/with space" 3
out=$("$VMFILE" -o kv "$T/with space" 2>/dev/null)
eq "$(total_pages "$out")" 3 "path with space"

cp "$T/one" "$T/-dash"
out=$(cd "$T" && "$VMFILE" -o kv -- -dash 2>/dev/null)
eq "$(files_count "$out")" 1 "leading-dash filename via --"

dd if=/dev/zero of="$T/short" bs=$((PAGESIZE-1)) count=1 2>/dev/null
dd if=/dev/zero of="$T/long"  bs="$PAGESIZE"     count=1 2>/dev/null
printf x >> "$T/long"
out=$("$VMFILE" -o kv "$T/short" 2>/dev/null); eq "$(total_pages "$out")" 1 "sub-page file: 1 page"
out=$("$VMFILE" -o kv "$T/long"  2>/dev/null); eq "$(total_pages "$out")" 2 "page+1 byte: 2 pages"

# -m: oversized file counted but its pages skipped, with a warning
out=$("$VMFILE" -m 1M "$T/big" 2>&1)
has "$out" "too large" "-m: too-large file warns"
out=$("$VMFILE" -m 1M -o kv "$T/big" 2>&1)
has "$out" "Files=1 "      "-m: file still counted"
has "$out" "TotalPages=0 " "-m: pages not counted"

# -b: batch list file; -0: NUL-delimited batch input
printf '%s\n' "$T/ten" > "$T/list"
out=$("$VMFILE" -o kv -b "$T/list" 2>/dev/null); rc_is $? 0 "-b reads paths from list file"
eq "$(files_count "$out")" 1 "-b: one file counted"
printf '%s\0' "$T/one" "$T/ten" > "$T/list0"
out=$("$VMFILE" -o kv -0 -b "$T/list0" 2>/dev/null); rc_is $? 0 "-b -0 handles NUL-delimited paths"
eq "$(files_count "$out")" 2 "-b -0: two files counted"

# invalid option combinations exit 1 with a FATAL message
for combo in "-t -e" "-e -l" "-l -L" "-q -v" "-w" "-P $T/pf"; do
  # shellcheck disable=SC2086
  out=$("$VMFILE" $combo "$T/one" 2>&1); rc=$?
  if [ "$rc" = 1 ] && grep -qF 'FATAL' <<<"$out"; then
    ok "invalid combo rejected: $combo"
  else
    fail "invalid combo: $combo" "rc=$rc out='${out:0:80}'"
  fi
done

# -q suppresses both the summary and warnings
out=$("$VMFILE" -q "$T/ten" 2>&1); rc_is $? 0 "-q exits 0"
[ -z "$out" ] && ok   "-q: silent on success" \
              || fail "-q" "unexpected output: ${out:0:80}"
out=$("$VMFILE" -q "$T/nonexistent" 2>&1); rc_is $? 0 "-q warnings: exits 0"
[ -z "$out" ] && ok   "-q: warnings suppressed" \
              || fail "-q warnings" "unexpected output: ${out:0:80}"

# unreadable file: warns, skipped, exit 0 (skip when running as root)
if [ "$(id -u)" != 0 ]; then
  mk "$T/noread" 2
  chmod 000 "$T/noread"
  out=$("$VMFILE" -o kv "$T/noread" 2>&1); rc_is $? 0 "unreadable file exits 0"
  has "$out" "unable to open" "unreadable file warns and is skipped"
  out=$("$VMFILE" -o kv "$T/noread" 2>/dev/null)
  eq "$(files_count "$out")" 1 "unreadable file still counted (failed)"
  chmod 644 "$T/noread"
else
  skip "unreadable-file test (running as root)"
fi

# unreadable directory: warns with the upstream opendir text
if [ "$(id -u)" != 0 ]; then
  mkdir -p "$T/nodir"
  mk "$T/nodir/f" 1
  chmod 000 "$T/nodir"
  out=$("$VMFILE" -o kv "$T/nodir" 2>&1); rc_is $? 0 "unreadable dir exits 0"
  has "$out" "unable to opendir $T/nodir" "unreadable dir warns with opendir text"
  chmod 755 "$T/nodir"
else
  skip "unreadable-dir test (running as root)"
fi

# -p offset beyond EOF: warning, file skipped
out=$("$VMFILE" -v -p "100M-200M" "$T/ten" 2>&1); rc_is $? 0 "-p beyond EOF exits 0"
has "$out" "smaller than offset" "-p beyond EOF warns"

# --- daemon readiness, exit status, and pidfile lifecycle ---------------------

pidfile="$T/daemon.pid"
out=$(timeout 5 "$VMFILE" -d -w -l -P "$pidfile" "$T/one" 2>&1); rc=$?
rc_is "$rc" 0 "-d -w parent returns success once child is ready"
if [ -r "$pidfile" ]; then
  daemon_pid=
  read -r daemon_pid < "$pidfile"
  if [[ "$daemon_pid" =~ ^[1-9][0-9]*$ ]] && kill -TERM "$daemon_pid" 2>/dev/null; then
    ok "daemon pidfile identifies a live child"
    for ((i=0; i<100; ++i)); do
      [ -e "$pidfile" ] || break
      sleep 0.05
    done
    if [ ! -e "$pidfile" ]; then
      ok "daemon removes pidfile on termination"
    else
      fail "daemon removes pidfile on termination" "pidfile still exists"
      kill -KILL "$daemon_pid" 2>/dev/null || true
    fi
  else
    fail "daemon pidfile identifies a live child" "pid='$daemon_pid'"
  fi
else
  fail "daemon creates pidfile before acknowledging readiness" "$out"
fi

out=$(timeout 5 "$VMFILE" -d -w -l -b "$T/no-such-list" 2>&1); rc=$?
rc_is "$rc" 1 "-d -w propagates child's startup failure"
has "$out" "unable to open batch list file" "child startup failure is reported"

if [ "$(uname -s)" != Linux ] || [ ! -r "/proc/$$/task/$$/children" ]; then
  skip "daemon EOF test requires Linux process children in procfs"
elif mkfifo "$T/blocking-list"; then
  "$VMFILE" -d -w -l -b "$T/blocking-list" >"$T/daemon-log" 2>&1 &
  daemon_parent=$!
  daemon_child=
  for ((i=0; i<100; ++i)); do
    children=$(cat "/proc/$daemon_parent/task/$daemon_parent/children" 2>/dev/null) || break
    read -r daemon_child _ <<< "$children"
    [ -z "$daemon_child" ] || break
    sleep 0.05
  done
  if [[ "$daemon_child" =~ ^[1-9][0-9]*$ ]] && kill -KILL "$daemon_child" 2>/dev/null; then
    for ((i=0; i<100; ++i)); do
      kill -0 "$daemon_parent" 2>/dev/null || break
      sleep 0.05
    done
    kill -KILL "$daemon_parent" 2>/dev/null || true
    wait "$daemon_parent" 2>/dev/null; rc=$?
    rc_is "$rc" 1 "-d -w treats status-pipe EOF as child failure"
    has "$(cat "$T/daemon-log")" "closed status pipe" "status-pipe EOF is diagnosed"
  else
    kill -KILL "$daemon_parent" 2>/dev/null || true
    wait "$daemon_parent" 2>/dev/null || true
    fail "-d -w EOF fixture" "could not identify and stop the child"
  fi
else
  fail "-d -w EOF fixture" "could not create the blocking FIFO"
fi

# --- lock-all retains and locks the actual file mapping ----------------------

VMF=$(plain_vmfile)
if [ "$(uname -s)" != Linux ] || [ ! -r "/proc/$$/smaps" ]; then
  skip "-L mapping check needs Linux procfs smaps"
elif [ -z "$VMF" ]; then
  skip "-L mapping check needs a non-sanitizer binary (set VMFILE_PLAIN)"
else
  mk "$T/lockall-target" 1
  pidfile="$T/lockall.pid"
  out=$(timeout 5 "$VMF" -d -w -q -L -P "$pidfile" "$T/lockall-target" 2>&1); rc=$?
  if [ "$rc" = 1 ] && [[ "$out" =~ unable\ to\ mlockall.*(Cannot\ allocate\ memory|Operation\ not\ permitted) ]]; then
    skip "-L mapping check: host cannot mlockall within its memory lock limit"
  else
    rc_is "$rc" 0 "-L daemon acknowledges mlockall success"
    lockall_pid=
    if [ -r "$pidfile" ]; then
      read -r lockall_pid < "$pidfile"
    fi
    if [[ "$lockall_pid" =~ ^[1-9][0-9]*$ ]]; then
      if awk -v target="$T/lockall-target" '
          /^[[:xdigit:]]+-[[:xdigit:]]+ / { mapping = ($NF == target) }
          mapping && /^VmFlags:/ && /(^|[[:space:]])lo([[:space:]]|$)/ { locked = 1 }
          END { exit !locked }' "/proc/$lockall_pid/smaps"; then
        ok "-L keeps the target file mapping present and locked"
      else
        fail "-L keeps the target file mapping present and locked" "target missing or not locked"
      fi
      if kill -TERM "$lockall_pid" 2>/dev/null; then
        for ((i=0; i<100; ++i)); do
          [ -e "$pidfile" ] || break
          sleep 0.05
        done
      fi
      if [ -e "$pidfile" ]; then
        fail "-L daemon stopped" "pidfile still exists"
        kill -KILL "$lockall_pid" 2>/dev/null || true
      fi
    else
      fail "-L daemon created a pidfile" "$out"
    fi
  fi
fi

# --- mlock failure = per-file warning (DEV-1, 2026-10-05) ---------------------

# lock a file far above the memlock rlimit: mlock fails non-retryably (ENOMEM)
# -> Failed{MlockFailed} -> "vmtouch: WARNING: mlock: PATH (ENOMEM)". Owner
# ruling (2026-10-05): upstream semantics KEPT — a -l run still ends in the
# blocking select() ("stay resident") even when some files failed, so the
# exit code cannot be asserted; timeout kills the run and asserts only the
# rendered text (no FATAL line anymore).
# NOTE: the sanitizer build's ASan mlock interceptor succeeds where the real
# syscall fails (documented since A1) — run this against a non-sanitizer
# binary: VMFILE_PLAIN env, a sibling build-plain/, or $VMFILE itself when it
# is not sanitized.
if [ "$(id -u)" != 0 ]; then
  if [ -n "$VMF" ]; then
    mk "$T/lockme" 25600   # 100 MB
    out=$(timeout 10 bash -c "ulimit -l 8; '$VMF' -l '$T/lockme'" 2>&1); rc=$?
    has "$out" "vmtouch: WARNING: mlock: " "mlock failure renders the upstream warning text"
    lacks "$out" "FATAL" "no FATAL line on mlock failure"
    rc_is "$rc" 124 "-l run blocks in select() as upstream (timeout kill)"
    rm -f "$T/lockme"
  else
    skip "mlock-warning test (no non-sanitizer binary; set VMFILE_PLAIN)"
  fi
else
  skip "mlock-warning test (running as root)"
fi

echo
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
