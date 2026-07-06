# Detect root dir: if CLib directory exists in current working dir, we are in root (.), else we are in CLib (..)
ROOT_DIR := $(shell [ -d CLib ] && echo "." || echo "..")

CC ?= clang
AR ?= ar
CFLAGS ?= -O3 -ffp-contract=fast -fno-math-errno -funroll-loops -Wall -Wextra -std=c11 -I$(ROOT_DIR) -I$(ROOT_DIR)/CLib -I$(ROOT_DIR)/CLib/Filters -I$(ROOT_DIR)/CLib/Audio -I$(ROOT_DIR)/CLib/Config -I$(ROOT_DIR)/CLib/FFT -I$(ROOT_DIR)/CLib/Mixer -I$(ROOT_DIR)/CLib/Resampler -I$(ROOT_DIR)/CLib/Processors -I$(ROOT_DIR)/CLib/DoP -I$(ROOT_DIR)/CLib/Pipeline -I$(ROOT_DIR)/CLib/Engine -I$(ROOT_DIR)/CLib/Server -I$(ROOT_DIR)/CLib/Backend -I$(ROOT_DIR)/CLib/Logging
LDFLAGS ?= -framework Accelerate -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

# All library C source files across subdirectories of CLib
SRCS := $(wildcard $(ROOT_DIR)/CLib/*/*.c)

# Map source files to object files in .build/obj/
# e.g. ./CLib/Audio/audio_buffers.c -> ./build/obj/CLib/Audio/audio_buffers.o
OBJ_DIR := $(ROOT_DIR)/.build/obj
OBJS := $(patsubst $(ROOT_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

LIB_TARGET := $(ROOT_DIR)/CLib/libdsp.a
CLI_SRC := $(ROOT_DIR)/CLib/main.c
CLI_BIN := $(ROOT_DIR)/CLib/bin/dsp-cli

TEST_SRCS := $(wildcard $(ROOT_DIR)/CTests/test_*.c)
TEST_BINS := $(patsubst $(ROOT_DIR)/CTests/test_%.c, $(ROOT_DIR)/CTests/bin/test_%, $(TEST_SRCS))

.PHONY: all lib test bench cli clean

all: cli

lib: $(LIB_TARGET)

# Compile library source files into object files (once per file)
$(OBJ_DIR)/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Archive object files into static library
$(LIB_TARGET): $(OBJS)
	@echo "AR $@"
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)

# Build test binaries by linking against libdsp.a
$(ROOT_DIR)/CTests/bin/test_%: $(ROOT_DIR)/CTests/test_%.c $(LIB_TARGET)
	@mkdir -p $(dir $@)
	@echo "LINK $@"
	@$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -o $@

BENCH_NAMES := test_filter_benchmark test_dop_benchmark test_resampler_matrix
BENCH_BINS := $(patsubst %, $(ROOT_DIR)/CTests/bin/%, $(BENCH_NAMES))
UNIT_TEST_BINS := $(filter-out $(BENCH_BINS), $(TEST_BINS))

test: $(UNIT_TEST_BINS)
	@echo "\n=== Running C Unit Tests ==="
	@for bin in $(UNIT_TEST_BINS); do \
		echo ""; \
		$$bin || exit 1; \
	done
	@echo "\n✅ All C unit tests passed!"

bench: $(BENCH_BINS)
	@echo "\n=== Running C Benchmark Tests ==="
	@for bin in $(BENCH_BINS); do \
		echo ""; \
		$$bin || exit 1; \
	done
	@echo "\n✅ All C benchmarks passed!"

cli: $(CLI_BIN)

$(CLI_BIN): $(CLI_SRC) $(LIB_TARGET)
	@mkdir -p $(dir $@)
	@echo "LINK $@"
	@$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -o $@

clean:
	rm -rf $(OBJ_DIR) $(LIB_TARGET) $(ROOT_DIR)/CTests/bin $(ROOT_DIR)/CLib/bin
