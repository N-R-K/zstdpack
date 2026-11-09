# zstdpack

Compress executables into a self extracting zstd archive. The extractor
decompresses on the fly in memory, no temp files, no dependencies, not even
libc. Smaller and faster than `upx` (see [#Benchmark](#benchmark)).

## Quick start

```sh
make                    # builds ./zstdpack, the ~55 KB stub
zstd -19 -vc exe | cat zstdpack /dev/stdin > exe.packed
chmod +x exe.packed     # make it executable
./exe.packed [args...]
```

`exe.packed` is now one self-contained file: copy it anywhere and run it. Its
name and extension are free choice — the stub looks for the payload behind its
own bytes, not at the file's name.

## Requirements

- **To run a packed file:** Linux 3.19 or newer, with `/proc` mounted — the
  stub reads `/proc/self/exe` to find its own bytes.
- **To build:** `gcc`/`clang` and GNU `make`.

To compress the executable using the official `zstd` cli is recommended. This
project doesn't ship it's own compressor.

## Build options

`ARCH` picks the platform layer, `MARCH` the `-march` for it: `x86-64` defaults
to `-march=x86-64-v3`, any other `ARCH` to the compiler's own. `CFLAGS`
replaces the tuning flags, `CPPFLAGS` adds to the preprocessor flags.

The default build targets `-march=x86-64-v3`, so it needs AVX2: Haswell 2013,
Excavator 2015 or later. For an older CPU, drop the level: the stub is a
little smaller (50 KB, not 54 KB) and runs on any x86-64, giving up some decode
speed on literal-heavy payloads — the trade is measured in
[doc/design.md](doc/design.md).

```sh
make MARCH=x86-64
```

You can also use `make DYNAMIC_BMI2=1` to compile both BMI2 and a generic
decoder which will be dispatched at runtime based on the CPU's BMI2 support. It
makes the stub ~21KiB larger for little benefit, not recommended and is disabled
by default.

The default build is tuned for `gcc`; `make -f mk.clang` builds with `clang`.

Cross-building the aarch64 port, for example on Alpine:

```sh
make -f mk.clang ARCH=aarch64 CC='clang --target=aarch64-alpine-linux-musl'
```

## Usage

Compress the program, append it to the stub, mark the result executable:

```sh
zstd -19 -vc exe | cat zstdpack /dev/stdin > exe.packed
chmod +x exe.packed
```

Any `zstd` level works; `-19` is only an example.

Two details in that line are load-bearing:

- the compressed stream has to be appended *after* the stub's bytes, so
  `cat /dev/stdin zstdpack` cannot work;
- the `chmod` is needed since redirect creates a 644 file.

To keep the compressed file around, the two-step form is equivalent:

```sh
zstd -19 -o exe.zst exe && cat zstdpack exe.zst > exe.packed && chmod +x exe.packed
```

## How it works

1. The stub maps `/proc/self/exe`: that is the stub followed by whatever was
   appended to it.
2. It finds the payload behind the stub's own bytes, checking that the frame
   decodes completely to the end of the file.
3. It decompresses that into memory, up to 2 GB.
4. It executes the result from an anonymous `memfd`. The program sees its
   arguments, environment and exit status pass through untouched.

Every kernel call is a raw syscall, the stub doesn't link or depend on anything
other than the kernel interfaces.

## Benchmark

Against `upx` 5.2.0 on this machine (AMD Ryzen 7 3700X), every file in tmpfs
so the numbers are the decompressor's and not the disk's. Each payload packed
and run as `--version`; the times are medians, and the unpacked binary's own
run is the floor.

A large binary, `llvm-exegesis` 22.1.3, 77.5 MiB:

| packed with | size | `--version` |
|---|---|---|
| nothing | 77.5 MiB | 3.8 ms |
| zstdpack + `zstd -1` | 31.2 MiB | 119 ms |
| `upx -1` | 32.4 MiB | 286 ms |
| zstdpack + `zstd -9` | 24.9 MiB | 134 ms |
| `upx -5` | 28.7 MiB | 278 ms |
| zstdpack + `zstd -19` | 21.6 MiB | 156 ms |
| `upx -9` | 27.5 MiB | 262 ms |

A smaller one, `qemu-aarch64` 11.0.3, 6.3 MiB:

| packed with | size | `--version` |
|---|---|---|
| nothing | 6.3 MiB | 0.59 ms |
| zstdpack + `zstd -1` | 2.3 MiB | 9.5 ms |
| `upx -1` | 2.2 MiB | 30.4 ms |
| zstdpack + `zstd -9` | 2.0 MiB | 10.7 ms |
| `upx -5` | 1.9 MiB | 30.0 ms |
| zstdpack + `zstd -19` | 1.8 MiB | 13.0 ms |
| `upx -9` | 1.9 MiB | 29.9 ms |

Every zstdpack size includes its 54 KB stub. It unpacks 1.7–2.5× faster than
upx on the 77.5 MiB payload and 2.4–3.3× on the 6.3 MiB one. In terms of
compression ratio zstd wins bigly too: 13% smaller than `upx -5` at `-9` and
21% smaller than `upx -9` at `-19` on the large binary, while staying similar
in size to upx on the smaller one.

## Limitations

- **The decompressed image is capped at 2 GB.** The compressed file that is
  appended has no size limit.
- **A packed file must not be rewritten.** `strip` or `objcopy` on it moves
  the section header table and truncates the appended stream, after which the
  stub reports "no zstd payload appended to this file". Strip the stub before
  packing, not the packed file.
- **The payload runs from a memfd, not from the packed file.** It appears as
  `/memfd:zstdpack (deleted)`, so a program that readlink()s
  `/proc/self/exe` to find its own directory — to load a config or data file
  from it, or to re-exec itself — finds nothing there: `ENOENT` for the path,
  an empty directory for its siblings. Pack the program together with its data
  and point it at them another way.

## Documentation

- [doc/design.md](doc/design.md) — implementation details and design decisions.
- [third_party/zstd/VENDORED.md](third_party/zstd/VENDORED.md) — the vendored
  zstd decoder: what is in there and how to update it.

## License

This project is released under [the Unlicense](LICENSE). The vendored zstd
decoder keeps its own BSD-2-Clause/GPLv2 dual license.
