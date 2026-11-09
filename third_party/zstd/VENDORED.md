# Vendored zstd (decompress-only)

Upstream: https://github.com/facebook/zstd — tag `v1.5.7`, commit `f8745da`.
Licensed under the BSD-2-Clause + GPLv2 dual license in `LICENSE`, which the
copied files carry in their headers.

Only the two directories a decoder needs are here: `common/` and `decompress/`,
plus the two public headers. `compress/`, `dictBuilder/`, `legacy/`, `dll/`,
programs and tests are not vendored: this project only ever decompresses.

Nothing in these files is modified; all trimming is done by the root Makefile,
which compiles the C files as one translation unit (`zstdpack_unity.c`) and
zstd's x86-64 assembly loop as another when `ARCH` is x86-64, including the
`-D` flags that trim it: one of each pair of duplicate decoder
implementations, and xxhash's allocator — see `doc/design.md`.

To update: copy `lib/{common,decompress}`, `lib/zstd.h`, `lib/zstd_errors.h`
and `LICENSE` from a new release over these, then `make clean && make test`.
