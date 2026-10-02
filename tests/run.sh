#!/usr/bin/env bash

set -u -o pipefail
export LC_ALL=C

VMFILE=${VMFILE:-./vmtouch}
case $VMFILE in
  /*) ;;
  *)  VMFILE=$PWD/${VMFILE#./} ;;
esac
[ -x "$VMFILE" ] || { echo "not executable: $VMFILE" >&2; exit 2; }

# Eviction tests need a real filesystem, not tmpfs.
T=$(mktemp -d "$PWD/vmtouch-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
case $(uname -s) in
  Linux)  fs=$(stat -f -c %T "$T" 2>/dev/null) ;;
  *)      fs=$(df -T "$T" 2>/dev/null | awk 'NR==2{print $2}') ;;
esac
[ "$fs" = tmpfs ] && { echo "$T is on tmpfs" >&2; exit 2; }

PAGESIZE=$(getconf PAGESIZE)

PASS=0 FAIL=0
ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  FAIL %s\n       %s\n' "$1" "$2"; }

eq()     { [ "$1" = "$2" ] && ok "$3" || fail "$3" "got '$1', want '$2'"; }
has()    { grep -qF -- "$2" <<<"$1" && ok "$3" || fail "$3" "missing '$2' in: ${1:0:200}"; }
rc_is()  { [ "$1" = "$2" ] && ok "$3" || fail "$3" "got rc $1, want $2"; }

# vmtouch -v prints the path, then "[chart] N/M", updating with \r.
# We want the last chart seen after the path.
pages() { # $1 out  $2 path  $3 r|t
  tr '\r' '\n' <<<"$1" | awk -v p="$2" -v w="$3" '
    index($0,p) { seen=1 }
    seen && match($0,/\[[^]]*\][ \t]+[0-9]+\/[0-9]+/) {
      s=substr($0,RSTART,RLENGTH); sub(/^.*\][ \t]+/,"",s)
      split(s,a,"/"); v=(w=="r"?a[1]:a[2])
    }
    END { print v }
  '
}
r_of() { pages "$1" "$2" r; }
t_of() { pages "$1" "$2" t; }

# 0 = evicted, 1 = still hot, 2 = parser failed
evict() {
  local i out r
  for i in 1 2 3 4 5; do
    sync 2>/dev/null
    "$VMFILE" -e "$1" >/dev/null 2>&1
    out=$("$VMFILE" -v "$1" 2>&1)
    r=$(r_of "$out" "$1")
    [ -z "$r" ] && return 2
    [ "$r" = 0 ] && return 0
    sleep 0.05
  done
  return 1
}

mk() { dd if=/dev/zero of="$1" bs="$PAGESIZE" count="$2" 2>/dev/null; }

mk "$T/one"   1
mk "$T/ten"  10
mk "$T/big" 1000
: > "$T/empty"
mkdir -p "$T/d/sub"
mk "$T/d/a"     2
mk "$T/d/sub/b" 5

"$VMFILE" -h >/dev/null 2>&1;     rc_is $? 1 "-h exits 1"
"$VMFILE" --help >/dev/null 2>&1; rc_is $? 1 "--help exits 1"

"$VMFILE" >/dev/null 2>&1; rc_is $? 1 "no args prints usage"

"$VMFILE" -t "$T/ten" >/dev/null 2>&1
out=$("$VMFILE" -v "$T/ten" 2>&1)
eq "$(r_of "$out" "$T/ten")" 10 "-t makes all 10 pages resident"
eq "$(t_of "$out" "$T/ten")" 10 "-v reports total 10"

out=$("$VMFILE" -v "$T/empty" 2>&1)
grep -qF -- "$T/empty" <<<"$out" \
  && fail "empty file" "listed by -v" \
  || ok   "empty file not listed"

out=$("$VMFILE" -v "$T/one" 2>/dev/null)
err=$("$VMFILE" -v "$T/one" 2>&1 >/dev/null)
[ -n "$out" ] && [ -z "$err" ] \
  && ok   "-v writes to stdout" \
  || fail "-v stream" "out='${out:0:60}' err='${err:0:60}'"

out=$("$VMFILE" -o kv "$T/one" 2>&1)
has "$out" "Files=1 "        "-o kv Files"
has "$out" "Directories=0 "  "-o kv Directories"
has "$out" "ResidentPages="  "-o kv ResidentPages"
has "$out" "TotalSize="      "-o kv TotalSize"


p5=$((PAGESIZE*5-1))         # last byte of 5 pages
p1=$((PAGESIZE-1))           # last byte of page 0
np1m=$((1048576/PAGESIZE))   # pages in 1 MiB

out=$("$VMFILE" -v -p "0-$p5" "$T/ten" 2>&1)
eq "$(t_of "$out" "$T/ten")" 5 "-p 0-$p5 covers 5 pages"

out=$("$VMFILE" -v -p 0-1M "$T/big" 2>&1)
eq "$(t_of "$out" "$T/big")" "$np1m" "-p 0-1M covers $np1m pages"

out=$("$VMFILE" -v -p "0-$p1" -p "0-$p5" "$T/ten" 2>&1)
eq "$(t_of "$out" "$T/ten")" 5 "multiple -p: last one wins"


out=$("$VMFILE" -v "$T/d" 2>&1)
has "$out" "$T/d/a"     "crawl reaches $T/d/a"
has "$out" "$T/d/sub/b" "crawl reaches nested file"

out=$("$VMFILE" -v "$T/one" "$T/ten" 2>&1)
has "$out" "Files: 2" "two files counted"


ln -s "$T/ten" "$T/link"
out=$("$VMFILE" -v "$T/link" 2>&1)
has "$out" "$T/link" "symlink path is echoed"

out=$("$VMFILE" -v -f "$T/link" 2>&1)
has "$out" "$T/link" "-f still echoes the link path"


mk "$T/d/skip.tmp" 1
mk "$T/d/keep.log" 1
out=$("$VMFILE" -v -i '*.tmp' -i '*.log' "$T/d" 2>&1)
if grep -qF -- "$T/d/skip.tmp" <<<"$out"; then
  fail "-i" "*.tmp not ignored"
elif grep -qF -- "$T/d/keep.log" <<<"$out"; then
  fail "-i" "*.log not ignored"
elif ! grep -qF -- "$T/d/a" <<<"$out"; then
  fail "-i" "non-matching file $T/d/a missing"
else
  ok "-i: both patterns active, others kept"
fi


evict "$T/big"
case $? in
  0)
    mid=$((PAGESIZE * 500))
    "$VMFILE" -t -p "$mid-$((mid + PAGESIZE*5 - 1))" "$T/big" >/dev/null 2>&1

    out=$("$VMFILE" -v -p "0-$p1" "$T/big" 2>&1)
    eq "$(r_of "$out" "$T/big")" 0 "-t -p: page 0 left cold"

    out=$("$VMFILE" -v "$T/big" 2>&1)
    n=$(r_of "$out" "$T/big")
    # 6 pages touched + kernel read-ahead; nowhere near 1000.
    [ "$n" -ge 6 ] && [ "$n" -lt 100 ] \
      && ok   "-t -p: $n pages resident (range+readahead)" \
      || fail "-t -p" "unexpected resident count: $n"
    ;;
  2)
    fail "-t -p" "output parser broke (vmtouch -v format changed?)"
    ;;
  *)
    echo "  skip -t -p (cannot evict $T/big)"
    ;;
esac


"$VMFILE" -t "$T/ten" >/dev/null 2>&1
"$VMFILE"    "$T/ten" >/dev/null 2>&1; rc_is $? 0 "analysis exits 0 when pages hot"

"$VMFILE" -e "$T/ten" >/dev/null 2>&1
"$VMFILE"    "$T/ten" >/dev/null 2>&1; rc_is $? 0 "analysis exits 0 after -e"

out=$("$VMFILE" "$T/nonexistent" 2>&1); rc=$?
rc_is "$rc" 0 "missing path exits 0"
has "$out" "unable to stat" "missing path warns"


mk "$T/with space" 3
out=$("$VMFILE" -v "$T/with space" 2>&1)
eq "$(t_of "$out" "$T/with space")" 3 "path with space"

cp "$T/one" "$T/-dash"
out=$(cd "$T" && "$VMFILE" -v -- -dash 2>&1)
has "$out" "-dash" "leading-dash filename via --"

dd if=/dev/zero of="$T/short" bs=$((PAGESIZE-1)) count=1 2>/dev/null
dd if=/dev/zero of="$T/long"  bs="$PAGESIZE"     count=1 2>/dev/null
printf x >> "$T/long"
out=$("$VMFILE" -v "$T/short" 2>&1); eq "$(t_of "$out" "$T/short")" 1 "sub-page file: 1 page"
out=$("$VMFILE" -v "$T/long"  2>&1); eq "$(t_of "$out" "$T/long")"  2 "page+1 byte: 2 pages"

# --- extended characterization coverage (pinned before core extraction) ---

# hardlink dedup: default counts once, -h counts each path
mk "$T/hl" 3
ln "$T/hl" "$T/hl2"
out=$("$VMFILE" -v "$T/hl" "$T/hl2" 2>&1)
has "$out" "Files: 1" "hardlink dedup: two hardlinks counted as one file"
out=$("$VMFILE" -v -h "$T/hl" "$T/hl2" 2>&1)
has "$out" "Files: 2" "-h counts hardlinked copies separately"

# -I: only files matching the filter are processed (directories still traversed)
out=$("$VMFILE" -v -I '*.log' "$T/d" 2>&1)
if grep -qF -- "$T/d/a" <<<"$out"; then
  fail "-I" "non-matching file $T/d/a was processed"
elif ! grep -qF -- "$T/d/keep.log" <<<"$out"; then
  fail "-I" "matching file $T/d/keep.log missing"
else
  ok "-I: only matching files processed"
fi

# -m: oversized file counted but its pages skipped, with a warning
out=$("$VMFILE" -m 1M "$T/big" 2>&1)
has "$out" "too large" "-m: too-large file warns"
out=$("$VMFILE" -m 1M -o kv "$T/big" 2>&1)
has "$out" "Files=1 "      "-m: file still counted"
has "$out" "TotalPages=0 " "-m: pages not counted"

# -b: batch list file; -0: NUL-delimited batch input
printf '%s\n' "$T/ten" > "$T/list"
out=$("$VMFILE" -v -b "$T/list" 2>&1)
eq "$(t_of "$out" "$T/ten")" 10 "-b reads paths from list file"
printf '%s\0' "$T/one" "$T/ten" > "$T/list0"
out=$("$VMFILE" -v -0 -b "$T/list0" 2>&1)
eq "$(t_of "$out" "$T/ten")" 10 "-b -0 handles NUL-delimited paths"

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
  out=$("$VMFILE" -v "$T/noread" 2>&1); rc_is $? 0 "unreadable file exits 0"
  has "$out" "unable to open" "unreadable file warns and is skipped"
  chmod 644 "$T/noread"
else
  echo "  skip unreadable-file test (running as root)"
fi

# unreadable directory: warns with the upstream opendir text (not "unable to
# open"); pins the A1-drift fix for the opendir failure site
if [ "$(id -u)" != 0 ]; then
  mkdir -p "$T/nodir"
  mk "$T/nodir/f" 1
  chmod 000 "$T/nodir"
  out=$("$VMFILE" -v "$T/nodir" 2>&1); rc_is $? 0 "unreadable dir exits 0"
  has "$out" "unable to opendir $T/nodir" "unreadable dir warns with opendir text"
  chmod 755 "$T/nodir"
else
  echo "  skip unreadable-dir test (running as root)"
fi

# -p offset beyond EOF: warning, file skipped
out=$("$VMFILE" -v -p "100M-200M" "$T/ten" 2>&1); rc_is $? 0 "-p beyond EOF exits 0"
has "$out" "smaller than offset" "-p beyond EOF warns"

# trailing slash: dir still crawled, no double slash in echoed paths
out=$("$VMFILE" -v "$T/d/" 2>&1); rc_is $? 0 "trailing slash exits 0"
has "$out" "$T/d/a" "trailing slash: directory still crawled"
if grep -qF -- "$T/d//" <<<"$out"; then
  fail "trailing slash" "double slash in echoed path"
else
  ok "trailing slash: no double slash"
fi

# symlink loop with -f: detected, terminates with exit 0.
# Note: without -h the hardlink-dedup check (dirs have st_nlink>=2) returns
# silently before the loop check fires — pinned behavior; -h disables dedup
# so the explicit loop warning is reachable.
ln -s "$T/d" "$T/d/loop"
out=$("$VMFILE" -v -f -h "$T/d" 2>&1); rc_is $? 0 "symlink loop exits 0"
has "$out" "symbolic link loop detected" "symlink loop detected under -f -h"
ln -s "$T/d" "$T/d/loop2"
out=$("$VMFILE" -v -f "$T/d/loop2" 2>&1); rc_is $? 0 "symlink-to-dir with -f exits 0"
has "$out" "$T/d/loop2/a" "-f on symlink-to-dir: follows and lists contents"
# revisit of d is silent here (dedup, no -h); warning path is pinned by the
# -f -h case above. Only termination is asserted:
has "$out" "$T/d/loop2/sub/b" "-f nested traversal terminates"
out=$("$VMFILE" -v "$T/d/loop2" 2>&1)
has "$out" "not following symbolic link" "symlink without -f: not followed"
grep -qF -- "$T/d/loop" <<<"$("$VMFILE" -v -f "$T/d" 2>&1)" \
  && fail "loop under -f" "infinite traversal" \
  || ok "loop under -f terminates"

echo
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
