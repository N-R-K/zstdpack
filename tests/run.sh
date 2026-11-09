#!/bin/sh
# End-to-end check of the built stub (`make test`).

here=$(cd "$(dirname "$0")/.." && pwd)
stub=$here/zstdpack
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fails=0
# Silent unless something fails: VERBOSE=1 shows the PASS lines too.
ok() { [ -n "$VERBOSE" ] && printf 'PASS  %s\n' "$1"; return 0; }
no() { printf 'FAIL  %s\n' "$1" >&2; fails=$((fails + 1)); }

[ -x "$stub" ] || { echo "no stub: run make first" >&2; exit 1; }
[ -x "$(command -v zstd)" ] || { echo "no zstd on PATH" >&2; exit 1; }
CC=${CC:-cc}
# Cross testing: QEMU prefixes every stub run (e.g. qemu-aarch64), STRIP names
# a stripper for the stub's architecture. Both empty/default for a native run.
QEMU=${QEMU:-}
STRIP=${STRIP:-strip}

# ---------------------------------------------------------------- happy path
$CC -Os -o $tmp/payload $here/tests/payload.c || { no "compile payload"; exit 1; }
zstd -q -f -19 -o $tmp/p.zst $tmp/payload
cat $stub $tmp/p.zst > $tmp/p.zstdpack
chmod +x $tmp/p.zstdpack

out=$($QEMU $tmp/p.zstdpack one two); rc=$?
case "$out" in
"one two | exe=/memfd:zstdpack (deleted) | fds=0") [ $rc -eq 42 ] && ok "small payload: args, exit status, memfd exec" \
	|| no "small payload: exit status $rc (want 42)" ;;
*) no "small payload: unexpected output '$out'" ;;
esac

# One-step packing form from the README: must match the two-step form exactly.
zstd -q -19 -vc $tmp/payload | cat $stub /dev/stdin > $tmp/p.zpack
cmp -s $tmp/p.zstdpack $tmp/p.zpack && ok "one-step pack: identical to cat of a .zst" \
	|| no "one-step pack: differs from the two-step form"
# A redirect creates 644, so the documented chmod +x is load-bearing.
[ -x $tmp/p.zpack ] && no "one-step pack: unexpectedly executable already" \
	|| { chmod +x $tmp/p.zpack
	     out=$($QEMU $tmp/p.zpack one two); rc=$?
	     [ $rc -eq 42 ] && ok "one-step pack: runs after chmod +x" \
		|| no "one-step pack: exit $rc after chmod, '$out'"; }

# 3 MB payload: forces the inflate buffer to grow past its 1 MB start.
{ echo 'static const unsigned char pad[3<<20] __attribute__((used)) = {1};'
  echo 'int main(void){return pad[1] + pad[(3<<20)-1];}'; } > $tmp/big.c
$CC -Os -o $tmp/big $tmp/big.c
zstd -q -f -19 -o $tmp/big.zst $tmp/big
cat $stub $tmp/big.zst > $tmp/big.zstdpack
chmod +x $tmp/big.zstdpack
out=$($QEMU $tmp/big.zstdpack); rc=$?
[ $rc -eq 0 ] && [ ! -s "$out" ] && ok "3 MB payload: exit status 0, no output" \
	|| no "3 MB payload: exit status $rc, output '$out'"

# A payload split over several zstd frames: cat of frames, not just one.
S=$(stat -c%s $tmp/big); H=$((S / 2))
head -c $H $tmp/big > $tmp/big1
tail -c +$((H + 1)) $tmp/big > $tmp/big2
zstd -q -f -19 -o $tmp/big1.zst $tmp/big1
zstd -q -f -19 -o $tmp/big2.zst $tmp/big2
cat $tmp/big1.zst $tmp/big2.zst > $tmp/big_multi.zst
cat $stub $tmp/big_multi.zst > $tmp/big_multi.zstdpack
chmod +x $tmp/big_multi.zstdpack
out=$($QEMU $tmp/big_multi.zstdpack); rc=$?
[ $rc -eq 0 ] && ok "multi-frame payload: exit status 0" \
	|| no "multi-frame payload: exit status $rc, output '$out'"

# Nothing is written: run from an empty cwd with an empty TMPDIR.
mkdir -p $tmp/scratch $tmp/tmpdir
(cd $tmp/scratch && TMPDIR=$tmp/tmpdir $QEMU $tmp/p.zstdpack >/dev/null 2>&1)
left=$(ls -A $tmp/scratch; ls -A $tmp/tmpdir)
[ -z "$left" ] && ok "no files created in cwd or TMPDIR" || no "left something behind: $left"

# ------------------------------------------------------------- broken inputs
# The stub on its own: no payload appended.
out=$($QEMU $stub 2>&1); rc=$?
case "$out" in
*"no zstd payload"*) [ $rc -ne 0 ] && ok "stub without payload: fails ($out)" \
	|| no "stub without payload: unexpectedly succeeded" ;;
*) no "stub without payload: got '$out'" ;;
esac

# A payload that stops halfway: must refuse, not run half a program.
head -c 8 $tmp/p.zst > $tmp/cut.zst
cat $stub $tmp/cut.zst > $tmp/cut.zstdpack
chmod +x $tmp/cut.zstdpack
out=$($QEMU $tmp/cut.zstdpack 2>&1); rc=$?
case "$out" in
*"truncated or corrupt"*) [ $rc -ne 0 ] && ok "truncated payload: fails ($out)" \
	|| no "truncated payload: unexpectedly succeeded" ;;
*) no "truncated payload: got '$out'" ;;
esac

# A payload cut off at the tail: the frame header still parses, but the frame
# never finishes, so the stub must refuse it instead of running a partial image.
S=$(stat -c%s $tmp/p.zst)
head -c $((S - 8)) $tmp/p.zst > $tmp/tail.zst
cat $stub $tmp/tail.zst > $tmp/tail.zstdpack
chmod +x $tmp/tail.zstdpack
out=$($QEMU $tmp/tail.zstdpack 2>&1); rc=$?
case "$out" in
*"truncated or corrupt"*) [ $rc -ne 0 ] && ok "payload truncated at the tail: refused" \
	|| no "payload truncated at the tail: exit $rc (want failure)" ;;
*) no "payload truncated at the tail: ran a partial image ('$out')" ;;
esac

# A payload whose checksum is missing: the frame data is whole, but the
# decoder never sees the end of the frame, so the stub must refuse it.
zstd -q -f -19 --check -o $tmp/check.zst $tmp/payload
S=$(stat -c%s $tmp/check.zst)
head -c $((S - 4)) $tmp/check.zst > $tmp/check_cut.zst
cat $stub $tmp/check_cut.zst > $tmp/check_cut.zstdpack
chmod +x $tmp/check_cut.zstdpack
out=$($QEMU $tmp/check_cut.zstdpack 2>&1); rc=$?
case "$out" in
*"truncated or corrupt"*) [ $rc -ne 0 ] && ok "payload with a missing checksum: refused ($out)" \
	|| no "payload with a missing checksum: unexpectedly succeeded" ;;
*) no "payload with a missing checksum: ran a partial image ('$out')" ;;
esac

# A payload with one byte corrupted in the middle of the frame: the header
# still parses and the frame is whole, so only the decoder can tell. The
# frame's checksum is on, so the corruption cannot go unnoticed.
zstd -q -f -19 --check -o $tmp/corrupt.zst $tmp/payload
S=$(stat -c%s $tmp/corrupt.zst)
MID=$((S / 2))
old=$(dd if=$tmp/corrupt.zst bs=1 skip=$MID count=1 2>/dev/null | od -An -tu1 | tr -d ' \n')
printf "\\$(printf '%03o' $(( (old + 1) % 256 )))" |
	dd of=$tmp/corrupt.zst bs=1 seek=$MID conv=notrunc 2>/dev/null
cat $stub $tmp/corrupt.zst > $tmp/corrupt.zstdpack
chmod +x $tmp/corrupt.zstdpack
out=$($QEMU $tmp/corrupt.zstdpack 2>&1); rc=$?
case "$out" in
*"truncated or corrupt"*) [ $rc -ne 0 ] && ok "payload with a corrupted byte: refused ($out)" \
	|| no "payload with a corrupted byte: unexpectedly succeeded" ;;
*) no "payload with a corrupted byte: ran a corrupted image ('$out')" ;;
esac

# Random bytes where the stream should be.
head -c 4096 /dev/urandom > $tmp/noise
cat $stub $tmp/noise > $tmp/noise.zstdpack
chmod +x $tmp/noise.zstdpack
out=$($QEMU $tmp/noise.zstdpack 2>&1); rc=$?
[ $rc -ne 0 ] && ok "garbage payload: fails ($out)" || no "garbage payload: unexpectedly succeeded"

# A file made of magic bytes must not cost a decode attempt per hit: the
# probe is capped, so the stub gives up at once instead of scanning for
# minutes.
cat > $tmp/magic.c <<'EOF'
#include <stdio.h>
int main(int argc, char **argv)
{
	unsigned char m[4] = { 0x28, 0xb5, 0x2f, 0xfd };
	long i;
	FILE *f = fopen(argv[1], "wb");

	if (!f)
		return 1;
	for (i = 0; i < 1024 * 1024; i++) /* 4 MB of magic */
		fwrite(m, 1, 4, f);
	return fclose(f);
}
EOF
$CC -Os -o $tmp/magic $tmp/magic.c && $tmp/magic $tmp/magic.bin
cat $stub $tmp/magic.bin > $tmp/magic.zstdpack
chmod +x $tmp/magic.zstdpack
out=$(timeout 10 $QEMU $tmp/magic.zstdpack 2>&1); rc=$?
[ $rc -ne 0 ] && [ $rc -ne 124 ] && ok "file of magic bytes: refused quickly ($out)" \
	|| no "file of magic bytes: exit $rc (want a quick failure)"

# An ELF header that does not add up: the scan is the fallback, and it still
# finds the payload.
cp $tmp/p.zstdpack $tmp/badelf.zstdpack
printf '\377\377\377\377\377\377\377\377' |
	dd of=$tmp/badelf.zstdpack bs=1 seek=40 conv=notrunc 2>/dev/null
chmod +x $tmp/badelf.zstdpack
out=$($QEMU $tmp/badelf.zstdpack one two); rc=$?
[ $rc -eq 42 ] && ok "broken ELF header: the scan still finds the payload" \
	|| no "broken ELF header: exit $rc, '$out'"

# A stripped stub: the payload offset comes from the ELF header, and strip
# keeps the header, so the packed file still runs.
if command -v "$STRIP" >/dev/null 2>&1; then
	cp $stub $tmp/stub.stripped
	$STRIP $tmp/stub.stripped
	cat $tmp/stub.stripped $tmp/p.zst > $tmp/stripped.zstdpack
	chmod +x $tmp/stripped.zstdpack
	out=$($QEMU $tmp/stripped.zstdpack one two); rc=$?
	[ $rc -eq 42 ] && ok "stripped stub: payload found and run" \
		|| no "stripped stub: exit $rc, '$out'"
fi

# Without /proc the stub cannot find its own bytes: the errno needs a name,
# not "error 2". Needs user namespaces to mount over /proc. Skipped under
# QEMU, which serves /proc/self/exe itself instead of the kernel's.
if [ -z "$QEMU" ] && command -v unshare >/dev/null 2>&1 && unshare -rm sh -c 'mount -t tmpfs none /proc' 2>/dev/null; then
	out=$(unshare -rm sh -c "mount -t tmpfs none /proc && exec $QEMU $tmp/p.zstdpack" 2>&1); rc=$?
	case "$out" in
	*"no such file or directory"*) [ $rc -ne 0 ] && ok "no /proc: the errno has a name ($out)" \
		|| no "no /proc: unexpectedly succeeded" ;;
	*) no "no /proc: got '$out'" ;;
	esac
fi

# --------------------------------------------------------- invoked as a link
# /proc/self/exe has to resolve through a symlink to the real file.
ln -sf $tmp/p.zstdpack $tmp/link.zstdpack
out=$($QEMU $tmp/link.zstdpack 2>&1); rc=$?
[ $rc -eq 42 ] && ok "invoked through a symlink" || no "symlink invocation: exit status $rc, '$out'"

[ $fails -eq 0 ] || { echo "$fails test(s) failed" >&2; exit 1; }
[ -z "$VERBOSE" ] || echo "stub tests passed"

# Decoder coverage is about the vendored zstd build flags, not the stub's logic.
sh $here/tests/zstd-coverage.sh || exit 1
