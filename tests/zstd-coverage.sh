#!/bin/sh
# Guards the decoder size knobs in the Makefile: compress a payload a dozen
# ways with the stock CLI, pack each stream into the real stub and run it;
# exit 42 is the payload reporting that it decoded and executed.
# Run by `make test`; needs zstd on PATH.

here=$(cd "$(dirname "$0")/.." && pwd)
stub=$here/zstdpack
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

CC=${CC:-cc}
# Cross testing: QEMU prefixes every stub run (e.g. qemu-aarch64).
QEMU=${QEMU:-}
fails=0
# Silent unless something fails: VERBOSE=1 shows the PASS lines too.
ok() { [ -n "$VERBOSE" ] && printf 'PASS  %s\n' "$1"; return 0; }
no() { printf 'FAIL  %s\n' "$1" >&2; fails=$((fails + 1)); }

[ -x "$stub" ] || { echo "no stub: run make first"; exit 1; }
command -v zstd >/dev/null || { echo "no zstd on PATH"; exit 1; }

$CC -Os -o $tmp/payload $here/tests/payload.c || { echo "FAIL  compile payload"; exit 1; }

# pack_and_run NAME ZSTDSTREAM: run the packed stub; 42 is the pass signal.
pack_and_run() {
	cat $stub $2 > $tmp/$1.zstdpack || return 1
	chmod +x $tmp/$1.zstdpack
	out=$($QEMU $tmp/$1.zstdpack one two 2>&1); rc=$?
	if [ $rc -ne 42 ]; then
		no "$1: exit $rc: $out"
	else
		ok "$1"
	fi
}

# Frame options that change what the decoder has to handle: no content size,
# checksum, --long, hand-set window sizes.
for spec in \
	'l1:-1' 'l3:-3' 'l9:-9' 'l12:-12' 'l19:-19' 'l22:--ultra -22' \
	'long27:-19 --ultra --long=27' 'nocs:-1 --no-content-size' \
	'nocs19:-19 --no-content-size' 'check:-5 --check' \
	'tinywin:-7 --zstd=windowLog=10,chainLog=12,searchLog=8,targetLength=3' \
	'params:-9 --zstd=windowLog=12,hashLog=8,minMatch=6,targetLength=8'
do
	name=${spec%%:*}; flags=${spec#*:}
	# shellcheck disable=SC2086
	if ! zstd -q -f $flags -o $tmp/$name.zst $tmp/payload; then
		no "$name: zstd rejected '$flags' (stale test, not a stub bug)"
		continue
	fi
	pack_and_run "$name" $tmp/$name.zst
done

# Multi-frame: `cat` of frames, which is how a payload can arrive.
zstd -q -f -1  -o $tmp/m1.zst $tmp/payload
zstd -q -f -19 -o $tmp/m2.zst $tmp/payload
head -c 3 $tmp/payload > $tmp/m3
zstd -q -f -3  -o $tmp/m3.zst $tmp/m3
cat $tmp/m1.zst $tmp/m2.zst $tmp/m3.zst > $tmp/multi.zst
pack_and_run multi $tmp/multi.zst

# The stub must decode an empty stream and then refuse to exec nothing, not
# report "no payload".
: > $tmp/zero           # zstd refuses /dev/null: it wants a regular file
zstd -q -f -3 -o $tmp/zero.zst $tmp/zero
cat $stub $tmp/zero.zst > $tmp/zero.zstdpack
chmod +x $tmp/zero.zstdpack
out=$($QEMU $tmp/zero.zstdpack 2>&1); rc=$?
case "$out" in
*"no zstd payload"*) no "empty content: not decoded ($out)" ;;
*)		     [ $rc -ne 0 ] \
			 && ok "empty content: decoded, refused to exec ($out)" \
			 || no "empty content: unexpectedly succeeded" ;;
esac

[ $fails -eq 0 ] || { echo "coverage: $fails stream(s) failed" >&2; exit 1; }
[ -z "$VERBOSE" ] || echo "coverage: every stream decoded"
