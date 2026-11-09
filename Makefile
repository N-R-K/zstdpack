# zstdpack: a self-extracting zstd executable. See README.md.
#
#   make / make test / make clean
#   make -f mk.clang            (build with clang instead)
#
# Nothing is linked in from outside this tree: the zstd decoder is vendored in
# third_party/zstd, the platform layer is platform_linux.c plus its
# platform_linux_$(ARCH).c, and the libc symbols come from mem.c. ARCH and
# DYNAMIC_BMI2 are the build's own settings; CFLAGS and CPPFLAGS are the
# overrides: `make CFLAGS=...` replaces the tuning flags, `-march` included,
# while `make CPPFLAGS=...` adds to the preprocessor flags.

# ARCH picks the platform layer, platform_linux_$(ARCH).c, and its default
# -march, set per arch below; an ARCH with no entry builds at the compiler's
# own. A cross build names its compiler too, e.g.
#   make ARCH=aarch64 CC='clang --target=aarch64-alpine-linux-musl'
CC   = gcc
ARCH = x86-64

MARCH.x86-64 = x86-64-v3
MARCH        = $(MARCH.$(ARCH))

# -DZSTDLIB_VISIBLE=static makes zstd's public API private, so the compiler
# drops the entry points the stub never calls. -Wno-unused-function silences
# the compress-side API that zstd.h declares but this tree does not vendor.
# DYNAMIC_BMI2 must stay a variable: the recipe expands $(CPPFLAGS) before
# $(ZSTD_CPPFLAGS), so a literal here would silently beat a user's override.
DYNAMIC_BMI2 ?= 0
ZSTD_CPPFLAGS := -DXXH_NAMESPACE=ZSTD_ -DDEBUGLEVEL=0 -DNDEBUG \
                 -DXXH_NO_STDLIB -DHUF_FORCE_DECOMPRESS_X1 \
                 -DZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT \
                 -DDYNAMIC_BMI2=$(DYNAMIC_BMI2) \
                 -DZSTDLIB_VISIBLE=static \
                 -DZSTDPACK_PLATFORM='"platform_linux_$(ARCH).c"'

# Minimal tuning flags for the stub. `-march` is applied here, but users can
# replace the whole set with `make CFLAGS=...` or extend the preprocessor flags
# with `make CPPFLAGS=...`.
CFLAGS = -O2 -g0 -Wall -Wextra \
         -U_FORTIFY_SOURCE \
         -fno-builtin -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
         -fcf-protection=none \
         -fno-align-jumps -fno-align-functions -fno-align-loops -fno-align-labels \
         -fira-region=one -fira-hoist-pressure -freorder-blocks-algorithm=simple \
         -fno-prefetch-loop-arrays -fno-tree-ch \
         $(if $(MARCH),-march=$(MARCH))

ZSTD_DIR := third_party/zstd

# gc-sections needs every function split into its own section.
SPLIT := -ffunction-sections -fdata-sections

ZSTD_SRC  := zstdpack_unity.c
# huf_decompress_amd64.S is x86-64 BMI2 assembly: another ARCH has no use for
# it, and some assemblers cannot even read it.
ifeq ($(ARCH),x86-64)
ZSTD_SRC += $(ZSTD_DIR)/decompress/huf_decompress_amd64.S
endif
SHIM_OBJ := mem.o
ZSTD_DEPS := zstdpack.c platform_linux.c platform_linux_$(ARCH).c mem.c \
             $(ZSTD_DIR)/zstd.h $(ZSTD_DIR)/zstd_errors.h \
             $(wildcard $(ZSTD_DIR)/common/*.c) $(wildcard $(ZSTD_DIR)/decompress/*.c)

# -Wl,-no-pie, not -no-pie: with -static clang calls the driver form unused and
# warns, while gcc still needs it to not emit a PIE.
zstdpack: $(ZSTD_SRC) $(ZSTD_DEPS) $(SHIM_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SPLIT) -Wno-unused-function -I$(ZSTD_DIR) $(ZSTD_CPPFLAGS) \
	    -static -Wl,-no-pie -nostdlib -Wl,--gc-sections -o $@ $(ZSTD_SRC) \
	    $(SHIM_OBJ)

# The libc-shaped symbols zstd and the compiler call by name get their own
# object, built -fno-builtin; see mem.c for why.
$(SHIM_OBJ): mem.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SPLIT) -fno-builtin -c -o $@ $<

test: zstdpack
	CC="$(CC)" sh tests/run.sh
	$(CC) $(CFLAGS) $(CPPFLAGS) -no-pie -o memtest tests/mem.c && ./memtest

clean:
	rm -f zstdpack mem.o memtest

.PHONY: test clean
