# src/xlat11 - host build and tests for the gfx10.3 -> gfx11 translator.
#   make test        unit + golden tests (ASan/UBSan) and the bounds fuzz (ASan/UBSan)
#   make fuzz        bounds fuzz only, 200000 iterations
#   make kextcheck   compile the library as x86_64 kernel code (C and C++), prove no
#                    undefined symbols and no SSE/x87 use
#   make gen         regenerate the table from the re/ clones (README)
CC      := clang
CXX     := clang++
BUILD   := build
OBJDUMP := objdump
WARN    := -Wall -Wextra -Werror -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wvla
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CSTD    := -std=c11
LIBSRC  := xlat11.c
HDRS    := xlat11.h xlat11_tables.h xlat11_reused.h xlat11_repack.h xlat11_provenance.h

.PHONY: all test fuzz kextcheck gen clean

all: $(BUILD)/test_xlat11 $(BUILD)/fuzz_xlat11

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_xlat11: tests/test_xlat11.c tests/golden_vectors.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(SAN) -O1 -g -Isrc -I. tests/test_xlat11.c $(LIBSRC) -o $@

$(BUILD)/fuzz_xlat11: tests/fuzz_xlat11.c tests/golden_vectors.h $(LIBSRC) $(HDRS) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(SAN) -O1 -g -I. tests/fuzz_xlat11.c $(LIBSRC) -o $@

test: $(BUILD)/test_xlat11 $(BUILD)/fuzz_xlat11
	$(BUILD)/test_xlat11
	$(BUILD)/fuzz_xlat11 200000

fuzz: $(BUILD)/fuzz_xlat11
	$(BUILD)/fuzz_xlat11 200000
kextcheck: | $(BUILD)
	$(CC) -std=c11 -mkernel -ffreestanding -fno-builtin -fno-common \
	    -fno-stack-protector -Wall -Wextra -Werror -O2 -c $(LIBSRC) -o $(BUILD)/xlat11_kext_c.o
	$(CXX) -x c++ -std=c++17 -mkernel -fno-exceptions -fno-rtti \
	    -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -O2 -c $(LIBSRC) -o $(BUILD)/xlat11_kext_cxx.o
	@echo "undefined symbols (C):";   nm -u $(BUILD)/xlat11_kext_c.o;   test -z "$$(nm -u $(BUILD)/xlat11_kext_c.o)"
	@echo "undefined symbols (C++):"; nm -u $(BUILD)/xlat11_kext_cxx.o; test -z "$$(nm -u $(BUILD)/xlat11_kext_cxx.o)"
	@n=$$($(OBJDUMP) -d $(BUILD)/xlat11_kext_c.o | grep -E -c '\b(xmm|ymm|zmm)[0-9]+\b|\b(fld|fst|fstp|fadd|fsub|fmul|fdiv|fabs|fsqrt|fucom|fcom|fxch|fild|fist)[a-z]*\b'); \
	    echo "SSE/x87 instructions: $$n"; test "$$n" = 0

gen:
	python3 gen/gen_tables.py

clean:
	rm -rf $(BUILD)
