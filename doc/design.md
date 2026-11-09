# Design notes

The reasoning behind the build. What zstdpack does, how to use it and the
benchmark against upx are in [../README.md](../README.md).

## The build

The stub is built at `-O2` plus gcc's size flags above `-O2`
(`-fno-align-*`, `-fira-region=one`, ...), not `-Os`: `-Os` also hard-disables
optimizations such as division strength reduction, which no flag can restore,
for no size gain here. This is what [musl's
configure](https://www.openwall.com/lists/musl/2023-05-21/1) does with its own
`-Os`-equivalent.

`-g0` is not cosmetic: Alpine's gcc defaults to `-g`, and debug info was half
the file. `-U_FORTIFY_SOURCE` is needed on the same system, where gcc defines
`_FORTIFY_SOURCE=2` from `-O2` on and the fortify `<string.h>` then defines
`memcpy` and `memset` as inline functions, which collides with `mem.c`. Any
libc-free build there hits this.

### clang

`make -f mk.clang` builds the same stub with clang, which needs its own
`CFLAGS`: it rejects `-fira-region=one`, `-fira-hoist-pressure`,
`-freorder-blocks-algorithm=simple` and `-fno-tree-ch` outright, and ignores
`-fno-align-jumps`, `-fno-align-loops`, `-fno-align-labels` and
`-fno-prefetch-loop-arrays`; `-fno-align-functions` is accepted but a no-op. So
`-march` is the only size flag of gcc's set it keeps, and it builds at `-Os`
instead. That is the smaller stub, not a slower one:

| | `.text` | stripped stub |
|---|---|---|
| gcc, `-O2` + size flags (the shipped build) | 42,832 B | 54,128 B |
| clang, `-Os` (mk.clang) | 39,922 B | 51,056 B |

End to end on `zstd -19` payloads — a 41 MB real program, 106 MB of source text,
32 MB of random bytes — clang's stub decodes 3.8%, 2.1% and 3.1% faster than the
gcc one (medians of 61 interleaved runs each). gcc `-Os` is smaller still,
49,032 B, but 10% slower on the same three, which is why gcc keeps the size
flags above `-O2`.

It is clang's `-O2` stub that is the large one, 75,880 B stripped, 40% over
gcc's. 3.6 KB of that is the flags clang cannot take (gcc built from clang's
subset of them has 46,458 B of `.text`); the rest is code generation that `-Os`
declines — `out_str` inlined into every branch of `out_errno` (`os_fail`
6,649 B against gcc's 804 B), `mem.c`'s loops vectorized with AVX2 (`memmove`
1,748 B against 342 B, `memcpy` 868 against 132, `memset` 539 against 144) — or
leaves out of line where gcc folds it in: `ZSTD_decompressSequences` (3,816 B)
and `ZSTD_decompressSequencesSplitLitBuffer` (8,215 B) against
`ZSTD_decompressBlock_internal` underneath gcc.

## The decoder: vendored, decompress-only

`third_party/zstd` holds zstd v1.5.7's `common/` and `decompress/` directories
unmodified, plus its two public headers (see
`third_party/zstd/VENDORED.md` for provenance and how to update).
`compress/`, `dictBuilder/` and `legacy/` are not vendored: the stub only
decompresses. The previous approach linked a system `-l:libzstd.a`, which was a
portability problem rather than a smaller build — Alpine's `zstd-dev` ships no
static archive, so every build began by building zstd from a tarball by hand.

Frames are portable across versions: the format is fixed by
[RFC 8878](https://www.rfc-editor.org/rfc/rfc8878), so a 1.x decoder reads
frames from any 1.x CLI.

Four knobs shrink the result, each row measured against the one above it:

| | stripped stub | decode speed |
|---|---|---|
| system `libzstd.a`, `-O3` (the old build) | 210 KB | ~775 MB/s |
| vendored, `-O2` + size flags, `-g0` | 165 KB | ~740 MB/s |
| + `-ffunction-sections -fdata-sections -Wl,--gc-sections` | 153 KB | ~760 MB/s |
| + `HUF_FORCE_DECOMPRESS_X1`, `ZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT` | **108 KB** | ~750 MB/s |
| + libc-free platform layer (below) | **71 KB** | no measurable change |
| + `-march=x86-64-v3` (what `MARCH.x86-64` defaults to) | **54 KB** | no measurable change |

The last row's 71 KB is the build that leaves zstd's BMI2 run-time check on, as
it was then. With the check off — as the build does by default now — that row
reads **50 KB**, and `-march=x86-64-v3` costs 4 KB to buy the BMI2 loop rather
than saving 17 KB: see the `-march` note below.

The libc-free row is not a decoder knob: it drops the statically linked C
library. Its decode speed was measured end-to-end on a 172 MB payload, fifteen
runs each way: 0.09–0.10 s for both builds, so the allocator and the `memcpy`
replacement do not show up above run-to-run noise.

- **Linker garbage collection.** libzstd's public API has many entry points the
  stub never calls; with every function in its own section the linker drops
  them and the tables they keep alive. The compiler has already dropped every
  unreferenced `static` function, so this is the leftovers only: 6.4 KB of
  stripped stub, 2.5 KB of it `.text`, the rest tables and the padding they
  force. `make SPLIT=` builds without it, that 6.4 KB larger, at the same
  decode speed (8 MB payloads of three shapes, 21 interleaved runs each: within
  2%, in both directions). `mem.c` is split as well; all three of its functions
  are always used, so there it changes nothing but 48 B of section padding.
- **`-march=x86-64-v3`, and `DYNAMIC_BMI2` against it.** zstd compiles two
  copies of its hot decoder loops, a BMI2 one and a plain one, and picks per
  CPU when the target lacks BMI2. That run-time check is what carries a
  duplicate decoder; the target and the default are separate questions. Four
  builds, measured on this tree, stripped stub:

  | | stub | BMI2 loop | runs on |
  |---|---|---|---|
  | default: `-march=x86-64-v3`, `DYNAMIC_BMI2=0` | 54,128 B | always | AVX2: Haswell 2013 / Excavator 2015 or later |
  | `make MARCH=x86-64` | 49,968 B | never | any x86-64 |
  | `make MARCH=x86-64 DYNAMIC_BMI2=1` | 71,024 B | where the CPU has it | any x86-64 |
  | `make DYNAMIC_BMI2=1` | 75,184 B | always, plus a spare copy | AVX2 |

  Against the dispatch build the target removes 18,288 B of `.text`, 16,896 B
  off the stripped stub, and on a BMI2 CPU the check was already picking that
  copy, so decode speed there is unchanged by naming the target (measured end
  to end on 8 MB payloads of three shapes — incompressible, long-match and
  text-like — 41 interleaved runs each: within the ±1-3% that relinking the
  same code costs). Against the plain baseline it is the other way round:
  naming the target *costs* 4,160 B and buys the BMI2 loop, which is a speed
  knob (below). So the build sets `DYNAMIC_BMI2=0`: with `-march` naming BMI2
  already, the check can only ever keep a second copy alive, never pick the
  other one. `make DYNAMIC_BMI2=1` is the build for one stub packed once and
  run on a family of machines.

  That loop is worth what the literals cost to decode: measured end to end
  against a BMI2 build on 8 MB payloads (41 interleaved runs, medians), generic
  is +0.7% on source text, +3.4% on real programs and +11% on a stream of
  literals only — the shape where the decoder is not limited by `memcpy`, and
  where upstream reports the BMI2 Huffman loop at 27% over generic on x86-64
  (i9-9900K, gcc 12.2, literals-only corpus). Incompressible and long-match
  payloads are level either way, so the BMI2 code only pays on literal-heavy
  input, which is why the default build keeps it and the escape hatch is
  measured rather than free.
- **`HUF_FORCE_DECOMPRESS_X1` + `ZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT`.**
  zstd ships two implementations of the same decoder in two places and picks
  between them at runtime: the Huffman decoder has a one-symbol-per-iteration
  and a two-symbols variant, and `ZSTD_decompressSequences` has bodies tuned for
  many-short vs few-long sequences. Which one runs is a speed heuristic, never a
  property of the stream, so compiling only one costs a little speed on some
  inputs and no coverage. Both are zstd's own size knobs, implied by
  `ZSTD_LIB_MINIFY`.

That claim is about build flags, and flags get edited, so it is tested rather
than asserted: `tests/zstd-coverage.sh` compresses a payload a dozen ways with
the stock CLI and runs each stream through the real stub.

Similar knobs deliberately **not** used: `ZSTD_NO_INLINE` buys another 8 KB for
42% of decode speed (742 → 430 MB/s); `ZSTD_LIB_COMPRESSION=0` and
`ZSTD_LEGACY_SUPPORT=0` switch off files this tree does not contain.

## One translation unit

`zstdpack_unity.c` compiles the stub, its platform layer and the decoder as one
translation unit — three compiler runs instead of thirteen, and no `libzstd.a`.
That lets zstd's public API be made private with `-DZSTDLIB_VISIBLE=static`:
zstd.h declares it all with `ZSTDLIB_API`, which expands to `ZSTDLIB_VISIBLE`,
so the compiler itself drops the entry points the stub never calls, and the
stub's calls can be inlined.

Measured on the same tree:

| | stripped stub | .text | compiler runs |
|---|---|---|---|
| separate objects, `libzstd.a` | 108 KB | 98,716 B | 13 + link |
| one translation unit, static API | 108 KB | 96,148 B | 1 + link |

The file size does not move — the ELF is padded to the same alignment — but
2.5 KB leave `.text`, all of it zstd entry points the stub never calls. With
the libc-free platform layer on top, the same two columns read **71 KB** and
65,391 B; with the default `-march=x86-64-v3`, **54 KB** and 42,832 B. Both
rows are with zstd's BMI2 run-time check on, as it was then: the same
comparison today reads 50 KB → 54 KB, because the build now turns that check
off and the target is a 4 KB *addition* (see `-march` below).

`-Wno-unused-function` comes with the trick: zstd.h declares the compress-side
API, which this tree does not vendor, so those functions end up declared
`static` and never defined.

The one source outside that C translation unit is `huf_decompress_amd64.S`,
zstd's hand-written BMI2 Huffman loop. It is assembled as its own translation
unit in the same gcc run, and only when `ARCH` is `x86-64`: the loop is x86-64
machine code, and zstd calls it only on that target.

## No libc

The stub is built `-nostdlib` with its own `_start`, syscall wrappers and
messages. That is [u-config's](https://github.com/skeeto/u-config) trick, and
worth 37 KB. It buys more than size: a statically linked stub carried its libc
with it, so the binary was whatever libc built it; now the same ELF comes out of
Alpine and of a glibc distribution and runs on either.

The file split follows u-config too. Flags and errnos are the same on every
Linux architecture but the `SYS_*` numbers are not, which is what puts the
constants in `platform_linux.c` and the numbers in `platform_linux_$(ARCH).c`
(`ARCH` in the Makefile picks it), whose `_start` hands the kernel's stack to
`entrypoint` in two instructions.

Three other choices worth their reasons:

- **Doubling the decode buffer is `mremap(MREMAP_MAYMOVE)`,** not `realloc`: the
  kernel moves page table entries instead of copying, so growing from 1 MB to
  4 MB copies nothing.
- **`memmove` compares the two *addresses,* not the pointers:** comparing
  pointers that do not point into the same object is undefined.
- **`-DXXH_NO_STDLIB`** makes xxhash's allocator hooks return `NULL`/nothing.
  It is the library's own switch for a freestanding build, and the clean way to
  keep its `XXH*_createState` and `XXH*_freeState` from dragging in `malloc` and
  `free`; the stub never calls them.

### The libc shim (`mem.c`)

The bodies are portable C that moves sixteen bytes per iteration in unaligned
four-byte words. Measured against the x86-64 `rep movsb`/`rep stosb` bodies it
replaced, on the same tree, end to end (`zstd -19` payloads packed into the
real stub, medians of interleaved runs), the portable C is level to a few
percent *ahead* of them at the same size. Two other pure-C forms were tried and
lost: byte-at-a-time loops are 18–76% slower (most of it in `memset` for
compressible payloads, in `memmove` and `memcpy` for incompressible ones), and
`#pragma GCC unroll` on those loops is honoured but recovers almost none of it.

## Ports

The platform layer splits so another Linux architecture is one file:
`platform_linux.c` holds the flags, errnos, messages and the syscall-neutral
code, and `platform_linux_$(ARCH).c` holds the `SYS_*` numbers, the syscall
instruction and `_start`. `mem.c`, zstd and the stub logic are portable C and
unchanged.

aarch64 is the second port. Its syscall numbers are the generic ones in
`include/uapi/asm-generic/unistd.h` (arm64 has no table of its own), and it
reaches the kernel with `svc #0`: number in x8, arguments in x0-x5, result in
x0, negative errno on failure — the convention `platform_linux.c` already
assumes. The Makefile names the `-march` per platform, `MARCH.x86-64`, because
`aarch64` is not a valid `-march` value: `x86-64` takes a level
(`x86-64-v3`), the 64-bit ARM one being `armv8-a`. Measured on the aarch64
port, though, every level builds the same bytes — `armv8-a`, `armv8.2-a`,
`armv8-a+sve2` and `MARCH=` all give 66,072 B, since nothing here needs a knob
beyond the baseline — so only the x86-64 entry exists, and any new `ARCH` builds
at the compiler's own `-march` default until a size measurement argues
otherwise. `DYNAMIC_BMI2` is off by default for the same reason: with `-march`
already naming BMI2, zstd's run-time check keeps a second copy of its decoder
loops alive for 21 KB. `make DYNAMIC_BMI2=1` turns it back on for a stub packed
for one machine and run on a family of them. `huf_decompress_amd64.S` is x86-64
machine code and is already left out of any other `ARCH`.

### Testing a cross stub under qemu-user

qemu-user translates the guest's syscalls, so the aarch64 stub's platform layer
runs under it; `tests/run.sh` takes `QEMU` and `STRIP` for that. Two behaviours
differ from a real kernel and shape the test:

- A guest `execve`/`execveat` is handed to the host kernel rather than
  re-executed under qemu. Without binfmt_misc the host cannot exec a
  foreign-arch payload, so the qemu run packs an x86-64 payload: the aarch64
  stub decodes and execs it and the host runs it, which is the stub's contract
  anyway.
- qemu serves `/proc/self/exe` itself, so masking `/proc` does not stop the
  stub finding its own bytes; that one test is skipped under qemu.

## Where the payload starts

**What it does: the ELF header says where the stub ends, the magic probe
checks, and a full decode decides.** The payload is appended behind the stub's
own bytes, so the offset to start at is the stub's own length. On GNU `ld`
output the section header table ends the file, so that length is
`e_shoff + e_shnum * e_shentsize`, read from the ELF header at the front of the
mapped file. Verified on this tree: the stub's own length, about 57 KB
unstripped and 54 KB after `strip`, and in a packed file the payload starts at
exactly that offset.

**The hint is checked, not trusted.** The [ELF spec says so
explicitly](https://refspecs.linuxfoundation.org/elf/gabi4+/ch4.intro.html)
("Only the ELF header has a fixed position in the file") and allows ["inactive
space" of unspecified
content](https://docs.oracle.com/cd/E36784_01/html/E36857/chapter6-94076.html)
anywhere, including after the section header table, so a linker that emits
trailing bytes would shift the offset. The stub therefore uses the hint only to
pick the first candidate, and only if the frame magic is really there; a file
whose ELF header does not add up falls back to scanning the whole file. The
scan is capped at 32 candidates, so a file made of magic bytes cannot turn into
one decode attempt per four bytes: measured on this machine, the stub plus 1 MB
of `28 b5 2f fd` went from 5.2 s (4.7 s of it in mmap and munmap) to under a
millisecond.

**The payload offset also decides the message.** The ELF header says where the
stub ends, so a frame there that does not decode is a broken payload ("the
appended payload is truncated or corrupt"), while a file with no frame at that
offset has no payload at all ("no zstd payload appended to this file"). When
the header does not add up and the scan is used instead, the stub cannot tell
the two apart and says the payload is missing.

**The acceptance test is still a full decode.** The magic is only a place to
start looking; the acceptance test is that the candidate must decompress every
byte to EOF without error. That is a content guarantee: a stray magic would
have to *also* be a valid zstd frame whose output ends exactly at the file. A
payload split across several frames is decoded whole, exactly as `zstd -dc`
would decode the blob.

**The decoder and its buffer are reused across candidates.** A candidate that
fails must not cost an mmap and a fresh decoder: with three stray magics in
the stub's own text, and dozens in a crafted file, that allocation was the
whole cost.

**How UPX does it:** UPX writes the payload offset into the packed file when
it packs — a patched constant in the stub, next to the code that uses it —
and never scans for it at run time. That needs a packer program; zstdpack's
flow is `cat stub payload.zst`, so there is nothing to patch the offset into.

**Rejected: patch the stub size into the stub after compiling.** Works — a
`.zstdpack_off` section, found by name, exactly one enforced — and a clobbered
slot fails loudly rather than mis-executing. But it needs a second program and a
build step, and breaks silently if the stub is stripped *after* patching.

## If there were a packer

The one thing a packer would buy is not needing `/proc`: it could give the
payload its own `PT_LOAD` segment, so the kernel maps it and the stub reads it
from its own address space. The price is a second program, a rewritten ELF (a
new segment, a moved program header table) and a `cat` flow that either breaks
or forks into two paths. Not doing it: the `/proc` need is documented, and the
probe is one candidate now, not a scan.

## Scripts as payloads

A `#!` script cannot be a payload. The kernel runs the interpreter named on the
shebang line and passes it the path of the file, `/memfd:zstdpack (deleted)`,
which cannot be opened, so the exec fails with `ENOENT` ("exec the payload: no
such file or directory"). Getting around it means a second exec path, handing
the interpreter the script text; the interpreter has to be known, and `-c` is
bounded by `ARG_MAX`. The stub only ever execs the decoded image. UPX has a
separate container format for this, Linux/sh386, whose stub passes the script
to the shell as a `-c` argument ([UPX
docs](https://github.com/upx/upx/blob/master/doc/upx-doc.txt)).

## Future directions

**Unpack in place instead of exec, so `/proc/self/exe` survives.** The stub
hands the decoded image to `execveat` from a memfd, so the payload's
`/proc/self/exe` is `/memfd:zstdpack (deleted)` — a path no payload can
re-open. A program that readlink()s it to find its install prefix, or that
re-execs itself from it, gets `ENOENT`; the shebang failure above is that
same dead end. UPX takes the other road, verified against upx 5.2.0 on this
machine: the packed file stays the process, its payload's `exe` link is the
packed file on disk, and only its text page is a memfd (`/memfd:upx
(deleted)`) because Linux will not `PROT_EXEC` a VMA that was ever writable.

The price is a userland ELF loader where the exec path gets the kernel for
free: `PT_LOAD` mapping, the interpreter handoff, a rewritten auxv
(`AT_PHDR`, `AT_BASE`, `AT_ENTRY`), PIE relocation — all correct for every
payload and every port, against 6 KB of stub logic today. A middle option
does not fix it: exec'ing a file from `/dev/shm` would give the payload a
path with no siblings, so a program that loads its config or data from its
own directory would read an empty one and run on defaults — a silent wrong
answer, worse than the loud `ENOENT`. Only unpacking in place keeps the
packed file's location, and whatever the user put next to it. Not building it
now: no payload in the tree needs it. The trigger to revisit is a payload
that re-execs itself from, or reads its data relative to, `/proc/self/exe`.

## Size and limits, on purpose

- **54 KB is now almost all decoder**: 43 KB of `.text`, of which about 6 KB is
  the stub's logic, its platform layer and the libc shim. There is no way to
  drop the rest without taking on a runtime dependency.
- **Payloads over 2 GB are refused** (`MAX_IMAGE`) rather than growing without
  bound.
