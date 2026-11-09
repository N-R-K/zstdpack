/*
 * Unity build: the stub, its platform layer and the vendored zstd decoder in
 * one translation unit, so zstd's public API can be static and the compiler
 * drops the parts the stub never calls. See doc/design.md.
 */

/*
 * zstd's allocator, hooked before any zstd header is seen: the platform
 * layer's mmap allocator, so the stub has no malloc, calloc or free symbol at
 * all. Anonymous pages arrive zeroed, which is what calloc promises.
 */
#define ZSTD_DEPS_MALLOC
#define ZSTD_malloc(s) zp_alloc(s)
#define ZSTD_free(p) zp_free(p)
#define ZSTD_calloc(n, s) zp_alloc((n) * (s))

#include "zstdpack.c" /* the stub's logic, and the strings the messages print */
#include ZSTDPACK_PLATFORM /* platform_linux_$(ARCH).c, picked by ARCH */
#include "platform_linux.c" /* messages, os_*, allocator, entrypoint */

#include "third_party/zstd/common/debug.c"
#include "third_party/zstd/common/entropy_common.c"
#include "third_party/zstd/common/error_private.c"
#include "third_party/zstd/common/fse_decompress.c"
#include "third_party/zstd/common/pool.c"
#include "third_party/zstd/common/threading.c"
#include "third_party/zstd/common/xxhash.c"
#include "third_party/zstd/common/zstd_common.c"

#include "third_party/zstd/decompress/huf_decompress.c"
#include "third_party/zstd/decompress/zstd_ddict.c"
#include "third_party/zstd/decompress/zstd_decompress.c"
#include "third_party/zstd/decompress/zstd_decompress_block.c"
