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

echo
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
