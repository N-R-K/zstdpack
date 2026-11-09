# AGENTS.md

zstdpack is a self-extracting executable: the stub decodes the zstd frame
appended behind its own bytes and execs the payload from a memfd. Usage is in
README.md, design rationale and measurements in doc/design.md.

## Layout

| Path | What it is |
|---|---|
| `zstdpack.c` | the stub: `os_main`, payload discovery, frame check, inflate |
| `platform_linux.c` | arch-free Linux layer: errno table, mmap allocator, stderr output, `entrypoint()` |
| `platform_linux_$(ARCH).c` | per-arch `SYS_*` numbers, syscall wrappers, `_start`; a port is one more file here |
| `mem.c` | the `memcpy`/`memset`/`memmove` symbols libc would have supplied |
| `zstdpack_unity.c` | unity build: stub + platform + vendored decoder as one translation unit |
| `third_party/zstd/` | vendored zstd v1.5.7 `common/` and `decompress/`, unmodified |
| `Makefile`, `mk.clang` | the gcc build and the clang build |
| `tests/run.sh` | end-to-end suite (needs `zstd`; `QEMU=`/`STRIP=` run a cross stub under qemu-user) |
| `tests/zstd-coverage.sh` | sweep over the `zstd` CLI's compression levels |
| `tests/payload.c`, `tests/mem.c` | the payload fixture and the allocator unit test |

## Working rules

- `make`, `make test`, `make clean`.
- `third_party/zstd` is upstream code: fix nothing there, see
  `third_party/zstd/VENDORED.md` for how to update it.
- Docs have one home each: usage in README.md, rationale in doc/design.md.
  Do not restate either in code comments or elsewhere.
