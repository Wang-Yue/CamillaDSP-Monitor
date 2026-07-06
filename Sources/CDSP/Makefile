# Detect root dir: if Sources/CDSP directory exists in current working dir, we are in root (.), else we are in CDSP (../..)
ROOT_DIR := $(shell [ -d Sources/CDSP ] && echo "." || echo "../..")

CC ?= clang
AR ?= ar
CFLAGS ?= -O3 -ffp-contract=fast -fno-math-errno -funroll-loops -Wall -Wextra -std=c11 -I$(ROOT_DIR) -I$(ROOT_DIR)/Sources/CDSP -I$(ROOT_DIR)/Sources/CDSP/Filters -I$(ROOT_DIR)/Sources/CDSP/Audio -I$(ROOT_DIR)/Sources/CDSP/Config -I$(ROOT_DIR)/Sources/CDSP/FFT -I$(ROOT_DIR)/Sources/CDSP/Mixer -I$(ROOT_DIR)/Sources/CDSP/Resampler -I$(ROOT_DIR)/Sources/CDSP/Processors -I$(ROOT_DIR)/Sources/CDSP/DoP -I$(ROOT_DIR)/Sources/CDSP/Pipeline -I$(ROOT_DIR)/Sources/CDSP/Engine -I$(ROOT_DIR)/Sources/CDSP/Server -I$(ROOT_DIR)/Sources/CDSP/Backend -I$(ROOT_DIR)/Sources/CDSP/Logging
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    LDFLAGS ?= -framework Accelerate -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
else
    LDFLAGS ?= -lm -lpthread -lasound -lrt -lfftw3 -lopenblas -lpulse-simple -lpulse
    CFLAGS += -march=native
endif

# All library C source files across subdirectories of CDSP
SRCS := $(wildcard $(ROOT_DIR)/Sources/CDSP/*/*.c)

# Map source files to object files in .build/obj/
# e.g. ./Sources/CDSP/Audio/audio_buffers.c -> ./build/obj/Sources/CDSP/Audio/audio_buffers.o
OBJ_DIR := $(ROOT_DIR)/.build/obj
OBJS := $(patsubst $(ROOT_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

LIB_TARGET := $(ROOT_DIR)/Sources/CDSP/libdsp.a
CLI_SRC := $(ROOT_DIR)/Sources/CDSP/main.c
CLI_BIN := $(ROOT_DIR)/Sources/CDSP/bin/dsp-cli

TEST_SRCS := $(wildcard $(ROOT_DIR)/Tests/CLibTests/test_*.c)
TEST_BINS := $(patsubst $(ROOT_DIR)/Tests/CLibTests/test_%.c, $(ROOT_DIR)/Tests/CLibTests/bin/test_%, $(TEST_SRCS))

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
$(ROOT_DIR)/Tests/CLibTests/bin/test_%: $(ROOT_DIR)/Tests/CLibTests/test_%.c $(LIB_TARGET)
	@mkdir -p $(dir $@)
	@echo "LINK $@"
	@$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -o $@

BENCH_NAMES := test_filter_benchmark test_dop_benchmark test_resampler_matrix
BENCH_BINS := $(patsubst %, $(ROOT_DIR)/Tests/CLibTests/bin/%, $(BENCH_NAMES))
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
	rm -rf $(OBJ_DIR) $(LIB_TARGET) $(ROOT_DIR)/Tests/CLibTests/bin $(ROOT_DIR)/Sources/CDSP/bin
