# ods2v2 - from-scratch ODS2 implementation
#
# v1.3
#
# Sanitizers are ON by default for dev builds to catch my bad bugs.
# `make release` builds without them for a normal, fast binary
# once things are stable.
#
# DEPENDENCY NOTE: the main `ods2` tool uses a BSD copy of
# linenoise (third_party/linenoise/, BSD licensed, single .c/.h file)
# for interactive-mode command history and line editing (up/down
# arrows, Ctrl-A/E/W). Vendored rather than linked as a system
# library specifically so `make` keeps working with zero external
# dependencies everywhere - no separate install step on any platform.

CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -Iinclude -g
SANFLAGS = -fsanitize=address,undefined
BUILD    = build

# Shared sources for any tool that mounts/reads/writes a volume
# through the full ods2_volume.h API - most tools need this complete
# set even if they don't exercise every piece of it.
VOLUME_SRCS = src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c \
              src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c \
              src/ods2_header_build.c src/ods2_directory_write.c

TOOLS = ods2 read_home_block read_file_header ods2_ls ods2_cat ods2_mkdir \
        ods2_put ods2_check_headers ods2_dump_root ods2_dump_header ods2_rm

# `ods2` is the actual, intended day-to-day interface (DIR,
# CREATE/DIRECTORY, COPY, TYPE, DELETE, SET DEFAULT - everything a
# normal user needs, all in one binary). Built by default via
#  make test / make all / make tools.
MAIN_TOOLS = ods2

# These ten tools are single-purpose tools used while building this
# project - each testing or inspecting one specific piece in
# isolation. See README.md - not built by default - `make dev-tools` builds them.
DEV_TOOLS = read_home_block read_file_header ods2_ls ods2_cat ods2_mkdir \
            ods2_put ods2_check_headers ods2_dump_root ods2_dump_header ods2_rm

TESTS = $(BUILD)/ods2_ondisk_selftest $(BUILD)/ods2_checksum_selftest $(BUILD)/ods2_validate_selftest $(BUILD)/ods2_bitmap_selftest $(BUILD)/ods2_indexf_selftest $(BUILD)/ods2_retrieval_selftest $(BUILD)/ods2_root_header_selftest $(BUILD)/ods2_directory_selftest $(BUILD)/ods2_volume_selftest $(BUILD)/ods2_wildcard_selftest $(BUILD)/ods2_read_file_selftest $(BUILD)/ods2_header_build_selftest $(BUILD)/ods2_directory_write_selftest $(BUILD)/ods2_path_selftest

.PHONY: all test tools dev-tools release clean starter-volume

all: test tools starter-volume

# STARTER VOLUME: a genuine, empty ODS-2 volume, real VMS 7.1's own
# INITIALIZE (RA92 geometry, 2,940,951 blocks)
#
# `make starter-volume` decompresses it to ./transfer.dsk in the
# current directory. Won't overwrite an existing transfer.dsk

starter-volume: samples/empty_ods2_volume.dsk.gz
	@if [ -e transfer.dsk ]; then \
		echo "transfer.dsk already exists - not overwriting it. Move it aside" \
			"first, or gunzip -c samples/empty_ods2_volume.dsk.gz > some_other_name.dsk"; \
		exit 1; \
	fi
	gunzip -c samples/empty_ods2_volume.dsk.gz > transfer.dsk
	@echo "Created transfer.dsk - a fresh, empty, real-VMS-INITIALIZE'd ODS-2 volume."
	@echo "Try: ./ods2 transfer.dsk DIR"

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ods2_ondisk_selftest: tests/ods2_ondisk_selftest.c include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) $< -o $@

$(BUILD)/ods2_checksum_selftest: tests/ods2_checksum_selftest.c src/ods2_checksum.c include/ods2_checksum.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_checksum_selftest.c src/ods2_checksum.c -o $@

$(BUILD)/ods2_validate_selftest: tests/ods2_validate_selftest.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c include/ods2_validate.h include/ods2_ondisk.h include/ods2_checksum.h include/ods2_retrieval.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_validate_selftest.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_bitmap_selftest: tests/ods2_bitmap_selftest.c src/ods2_bitmap.c include/ods2_bitmap.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_bitmap_selftest.c src/ods2_bitmap.c -o $@

$(BUILD)/ods2_indexf_selftest: tests/ods2_indexf_selftest.c src/ods2_indexf.c include/ods2_indexf.h include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_indexf_selftest.c src/ods2_indexf.c -o $@

$(BUILD)/ods2_retrieval_selftest: tests/ods2_retrieval_selftest.c src/ods2_retrieval.c include/ods2_retrieval.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_retrieval_selftest.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_root_header_selftest: tests/ods2_root_header_selftest.c src/ods2_retrieval.c include/ods2_retrieval.h include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_root_header_selftest.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_directory_selftest: tests/ods2_directory_selftest.c src/ods2_directory.c include/ods2_directory.h include/ods2_ondisk.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_directory_selftest.c src/ods2_directory.c -o $@

samples/synthetic_disk.img: samples/sample.bin samples/indexf_headers.bin samples/root_dir.bin tools/build_synthetic_disk.py
	python3 tools/build_synthetic_disk.py

$(BUILD)/ods2_volume_selftest: tests/ods2_volume_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c include/ods2_volume.h samples/synthetic_disk.img | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_volume_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c -o $@

$(BUILD)/ods2_wildcard_selftest: tests/ods2_wildcard_selftest.c src/ods2_wildcard.c include/ods2_wildcard.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_wildcard_selftest.c src/ods2_wildcard.c -o $@

$(BUILD)/ods2_read_file_selftest: tests/ods2_read_file_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c include/ods2_volume.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_read_file_selftest.c src/ods2_volume.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c src/ods2_directory.c src/ods2_bitmap.c src/ods2_header_build.c src/ods2_directory_write.c -o $@

$(BUILD)/ods2_header_build_selftest: tests/ods2_header_build_selftest.c src/ods2_header_build.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c include/ods2_header_build.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_header_build_selftest.c src/ods2_header_build.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c -o $@

$(BUILD)/ods2_directory_write_selftest: tests/ods2_directory_write_selftest.c src/ods2_directory_write.c src/ods2_directory.c include/ods2_directory_write.h include/ods2_directory.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_directory_write_selftest.c src/ods2_directory_write.c src/ods2_directory.c -o $@

$(BUILD)/ods2_path_selftest: tests/ods2_path_selftest.c src/ods2_path.c include/ods2_path.h | $(BUILD)
	$(CC) $(CFLAGS) $(SANFLAGS) tests/ods2_path_selftest.c src/ods2_path.c -o $@

# --- CLI tools (built directly into the project root, e.g. ./ods2_ls,
#     not into build/, since that's where they're actually run from) ---

tools: $(MAIN_TOOLS)

dev-tools: $(DEV_TOOLS)

read_home_block: tools/read_home_block.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c include/ods2_validate.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/read_home_block.c src/ods2_validate.c src/ods2_checksum.c src/ods2_retrieval.c -o $@

read_file_header: tools/read_file_header.c include/ods2_ondisk.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/read_file_header.c -o $@

ods2_ls: tools/ods2_ls.c $(VOLUME_SRCS) src/ods2_wildcard.c include/ods2_volume.h include/ods2_wildcard.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_ls.c $(VOLUME_SRCS) src/ods2_wildcard.c -o $@

ods2_cat: tools/ods2_cat.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_cat.c $(VOLUME_SRCS) -o $@

ods2_mkdir: tools/ods2_mkdir.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_mkdir.c $(VOLUME_SRCS) -o $@

ods2_put: tools/ods2_put.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_put.c $(VOLUME_SRCS) -o $@

ods2_check_headers: tools/ods2_check_headers.c $(VOLUME_SRCS) include/ods2_volume.h include/ods2_validate.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_check_headers.c $(VOLUME_SRCS) -o $@

ods2_dump_root: tools/ods2_dump_root.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_dump_root.c $(VOLUME_SRCS) -o $@

ods2_dump_header: tools/ods2_dump_header.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_dump_header.c $(VOLUME_SRCS) -o $@

ods2_rm: tools/ods2_rm.c $(VOLUME_SRCS) include/ods2_volume.h
	$(CC) $(CFLAGS) $(SANFLAGS) tools/ods2_rm.c $(VOLUME_SRCS) -o $@

ods2: tools/ods2.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c third_party/linenoise/linenoise.c include/ods2_volume.h include/ods2_path.h include/ods2_wildcard.h third_party/linenoise/linenoise.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -Ithird_party/linenoise $(SANFLAGS) tools/ods2.c $(VOLUME_SRCS) src/ods2_path.c src/ods2_wildcard.c third_party/linenoise/linenoise.c -o $@

test: $(TESTS) tools
	@echo "--- ods2_ondisk_selftest ---"
	@$(BUILD)/ods2_ondisk_selftest
	@echo "--- ods2_checksum_selftest ---"
	@$(BUILD)/ods2_checksum_selftest
	@echo "--- ods2_validate_selftest ---"
	@$(BUILD)/ods2_validate_selftest
	@echo "--- ods2_bitmap_selftest ---"
	@$(BUILD)/ods2_bitmap_selftest
	@echo "--- ods2_indexf_selftest ---"
	@$(BUILD)/ods2_indexf_selftest
	@echo "--- ods2_retrieval_selftest ---"
	@$(BUILD)/ods2_retrieval_selftest
	@echo "--- ods2_root_header_selftest ---"
	@$(BUILD)/ods2_root_header_selftest
	@echo "--- ods2_directory_selftest ---"
	@$(BUILD)/ods2_directory_selftest
	@echo "--- ods2_volume_selftest ---"
	@$(BUILD)/ods2_volume_selftest
	@echo "--- ods2_wildcard_selftest ---"
	@$(BUILD)/ods2_wildcard_selftest
	@echo "--- ods2_read_file_selftest ---"
	@$(BUILD)/ods2_read_file_selftest
	@echo "--- ods2_header_build_selftest ---"
	@$(BUILD)/ods2_header_build_selftest
	@echo "--- ods2_directory_write_selftest ---"
	@$(BUILD)/ods2_directory_write_selftest
	@echo "--- ods2_path_selftest ---"
	@$(BUILD)/ods2_path_selftest
	@echo "--- CLI recursive DIR regression check ---"
	@if command -v timeout >/dev/null 2>&1; then \
		rm -f samples/cli_regression_disk.img; \
		cp samples/synthetic_disk.img samples/cli_regression_disk.img; \
		output=$$(timeout 10 ./ods2 samples/cli_regression_disk.img DIR "[...]*.*" 2>&1); \
		rc=$$?; \
		if [ $$rc -eq 124 ]; then \
			echo "FAIL: recursive DIR timed out - likely an infinite recursion regression" \
				"(root's self-referential 000000.DIR entry being treated as a real" \
				"subdirectory - see list_directory_recursive() in tools/ods2.c)"; \
			exit 1; \
		fi; \
		count=$$(echo "$$output" | grep -c "^Directory \[000000\]$$"); \
		if [ "$$count" -ne 1 ]; then \
			echo "FAIL: root listed $$count times during recursive DIR (expected exactly 1) -" \
				"likely the same self-reference regression"; \
			exit 1; \
		fi; \
		rm -f samples/cli_regression_disk.img; \
		echo "PASS: recursive DIR on root terminates correctly, root listed exactly once" \
			"(regression check for a real infinite-recursion bug found via real testing)"; \
	else \
		echo "SKIP: 'timeout' command not available on this platform - this check" \
			"needs it to safely test for infinite recursion without hanging the build"; \
	fi
	@echo "--- all tests passed ---"
	@echo ""
	@echo "Tools built: $(MAIN_TOOLS)"
	@echo "(Additional single-purpose/diagnostic tools available via 'make dev-tools':"
	@echo " $(DEV_TOOLS))"

release: CFLAGS += -O2
release: SANFLAGS =
release: test

clean:
	rm -rf $(BUILD)
	rm -f samples/synthetic_disk.img samples/synthetic_disk_working.img
	rm -f $(TOOLS)
