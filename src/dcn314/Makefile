# src/dcn314 - host build and tests for the DCN 3.1.4 read-only layer.
#   make test   unit tests (ASan/UBSan) and the planted-break evidence
#   make clean  remove build artifacts
CC := clang
BUILD := build
CSTD := -std=c11
WARN := -Wall -Wextra -Werror
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS := $(CSTD) $(WARN) $(SAN) -O1 -g -I.
HDRS := $(wildcard *.h)

.PHONY: all test clean

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_regs: tests/test_dcn314.c dcn314_core.c dcn314_ro.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) tests/test_dcn314.c dcn314_core.c dcn314_ro.c -o $@

$(BUILD)/test_allow: tests/test_dcn314_allow.c dcn314_core.c dcn314_allow.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) tests/test_dcn314_allow.c dcn314_core.c dcn314_allow.c -o $@

$(BUILD)/test_aux: tests/test_dcn314_aux.c dcn314_core.c dcn314_aux_min.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) tests/test_dcn314_aux.c dcn314_core.c dcn314_aux_min.c -o $@

test: $(BUILD)/test_regs $(BUILD)/test_allow $(BUILD)/test_aux
	$(BUILD)/test_regs
	$(BUILD)/test_allow
	$(BUILD)/test_aux
	python3 tests/run_planted_breaks.py

clean:
	rm -rf $(BUILD)
