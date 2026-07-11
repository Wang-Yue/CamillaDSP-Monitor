# Detect root dir: if Sources/CDSP directory exists in current working dir, we are in root (.), else we are in CDSP (../..)
ROOT_DIR := $(shell [ -d Sources/CDSP ] && echo "." || echo "../..")

CC ?= clang
AR ?= ar
CFLAGS ?= -O3 -ffp-contract=fast -fno-math-errno -funroll-loops -Wall -Wextra -std=c11 -I$(ROOT_DIR) -I$(ROOT_DIR)/Sources/CDSP -I$(ROOT_DIR)/Sources/CDSP/Filters -I$(ROOT_DIR)/Sources/CDSP/Audio -I$(ROOT_DIR)/Sources/CDSP/Config -I$(ROOT_DIR)/Sources/CDSP/FFT -I$(ROOT_DIR)/Sources/CDSP/Mixer -I$(ROOT_DIR)/Sources/CDSP/Resampler -I$(ROOT_DIR)/Sources/CDSP/Processors -I$(ROOT_DIR)/Sources/CDSP/DoP -I$(ROOT_DIR)/Sources/CDSP/Pipeline -I$(ROOT_DIR)/Sources/CDSP/Engine -I$(ROOT_DIR)/Sources/CDSP/Server -I$(ROOT_DIR)/Sources/CDSP/Backend -I$(ROOT_DIR)/Sources/CDSP/Logging
UNAME_S := $(shell uname -s)

# Setup Default Feature Flags
ifeq ($(UNAME_S),Darwin)
    ENABLE_COREAUDIO ?= 1
    ENABLE_ACCELERATE ?= 1
    ENABLE_ALSA ?= 0
    ENABLE_PULSE ?= 0
    ENABLE_PIPEWIRE ?= 0
    ENABLE_JACK ?= 0
    ENABLE_BLUEZ ?= 0
    ENABLE_FFTW ?= 0
    ENABLE_BLAS ?= 0
    ENABLE_WASAPI ?= 0
    ENABLE_ASIO ?= 0
    ENABLE_OPENMP ?= 0
else
    ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
        # Native Windows (MSYS2/MinGW)
        ENABLE_COREAUDIO ?= 0
        ENABLE_ACCELERATE ?= 0
        ENABLE_ALSA ?= 0
        ENABLE_PULSE ?= 0
        ENABLE_PIPEWIRE ?= 0
        ENABLE_JACK ?= 0
        ENABLE_BLUEZ ?= 0
        ENABLE_FFTW ?= 1
        ENABLE_BLAS ?= 0
        ENABLE_WASAPI ?= 1
        ENABLE_ASIO ?= 1
        ENABLE_OPENMP ?= 0
    else
        # Linux (Default)
        ENABLE_COREAUDIO ?= 0
        ENABLE_ACCELERATE ?= 0
        ENABLE_ALSA ?= 1
        ENABLE_PULSE ?= 1
        ENABLE_PIPEWIRE ?= 1
        ENABLE_JACK ?= 1
        ENABLE_BLUEZ ?= 1
        ENABLE_FFTW ?= 1
        ENABLE_BLAS ?= 1
        ENABLE_WASAPI ?= 0
        ENABLE_ASIO ?= 0
        ENABLE_OPENMP ?= 1
    endif
endif

# Map Flags to CFLAGS preprocessor definitions
ifeq ($(ENABLE_COREAUDIO),1)
    CFLAGS += -DENABLE_COREAUDIO
endif
ifeq ($(ENABLE_ACCELERATE),1)
    CFLAGS += -DENABLE_ACCELERATE
endif
ifeq ($(ENABLE_ALSA),1)
    CFLAGS += -DENABLE_ALSA
endif
ifeq ($(ENABLE_PULSE),1)
    CFLAGS += -DENABLE_PULSE
endif
ifeq ($(ENABLE_PIPEWIRE),1)
    CFLAGS += -DENABLE_PIPEWIRE
endif
ifeq ($(ENABLE_FFTW),1)
    CFLAGS += -DENABLE_FFTW
endif
ifeq ($(ENABLE_BLAS),1)
    CFLAGS += -DENABLE_BLAS
endif
ifeq ($(ENABLE_WASAPI),1)
    CFLAGS += -DENABLE_WASAPI
endif
ifeq ($(ENABLE_ASIO),1)
    CFLAGS += -DENABLE_ASIO
endif
ifeq ($(ENABLE_JACK),1)
    CFLAGS += -DENABLE_JACK
endif


# Map Flags to link options (LDFLAGS)
LDFLAGS := -lm -lpthread
ifeq ($(UNAME_S),Darwin)
    ifeq ($(ENABLE_COREAUDIO),1)
        LDFLAGS += -framework CoreAudio -framework AudioToolbox
    endif
    ifeq ($(ENABLE_ACCELERATE),1)
        LDFLAGS += -framework Accelerate
    endif
    LDFLAGS += -framework CoreFoundation
else
    ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
        # Windows Native Setup
        CFLAGS += -DCOBJMACROS -D_WIN32 -DUNICODE -D_UNICODE -D_USE_MATH_DEFINES
        ifeq ($(ENABLE_FFTW),1)
            LDFLAGS += -lfftw3 -lfftw3f
        endif
        ifeq ($(ENABLE_BLAS),1)
            LDFLAGS += -lopenblas
        endif
        LDFLAGS += -lws2_32 -lwinmm -lole32 -luuid -lksuser -lksguid
    else
        # Linux
        CFLAGS += -march=native -D_GNU_SOURCE
        ifeq ($(USE_LIBDISPATCH),1)
            CFLAGS += -DUSE_LIBDISPATCH -I/usr/libexec/swift/lib/swift
            LDFLAGS += -L/usr/libexec/swift/lib/swift/linux -ldispatch -lBlocksRuntime -Wl,-rpath,/usr/libexec/swift/lib/swift/linux
        else
            ifeq ($(ENABLE_OPENMP),1)
                CFLAGS += -fopenmp -DUSE_OPENMP
            endif
        endif
        ifeq ($(ENABLE_ALSA),1)
            LDFLAGS += -lasound
        endif
        ifeq ($(ENABLE_PULSE),1)
            LDFLAGS += -lpulse-simple -lpulse
        endif
        ifeq ($(ENABLE_PIPEWIRE),1)
            LDFLAGS += -lpipewire-0.3
            CFLAGS += $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null || echo "")
        endif
        ifeq ($(ENABLE_FFTW),1)
            LDFLAGS += -lfftw3 -lfftw3f
        endif
        ifeq ($(ENABLE_BLAS),1)
            LDFLAGS += -lopenblas
        endif
        LDFLAGS += -lrt
        ifneq ($(USE_LIBDISPATCH),1)
            ifeq ($(ENABLE_OPENMP),1)
                LDFLAGS += -fopenmp
            endif
        endif
        ifeq ($(ENABLE_BLUEZ),1)
            CFLAGS += -DENABLE_BLUEZ
            LDFLAGS += $(shell pkg-config --libs dbus-1 2>/dev/null || echo "-ldbus-1")
            CFLAGS += $(shell pkg-config --cflags dbus-1 2>/dev/null || echo "")
        endif
    endif
endif

# Global settings for optional backends
ifeq ($(ENABLE_JACK),1)
    CFLAGS += $(shell pkg-config --cflags jack 2>/dev/null || echo "")
    LDFLAGS += $(shell pkg-config --libs jack 2>/dev/null || echo "-ljack")
endif

# All library C source files across subdirectories of CDSP
ALL_SRCS := $(wildcard $(ROOT_DIR)/Sources/CDSP/*/*.c)

# Filter native source files based on enabled backends
SRCS := $(ALL_SRCS)

ifneq ($(ENABLE_ALSA),1)
    SRCS := $(filter-out %/alsa_capture.c %/alsa_playback.c %/alsa_device.c %/alsa_capabilities.c, $(SRCS))
endif

ifneq ($(ENABLE_PULSE),1)
    SRCS := $(filter-out %/pulse_backend.c, $(SRCS))
endif

ifneq ($(ENABLE_PIPEWIRE),1)
    SRCS := $(filter-out %/pipewire_backend.c, $(SRCS))
endif

ifneq ($(ENABLE_COREAUDIO),1)
    SRCS := $(filter-out %/core_audio_capture.c %/core_audio_playback.c %/core_audio_device.c %/core_audio_capabilities.c, $(SRCS))
endif

ifneq ($(ENABLE_WASAPI),1)
    SRCS := $(filter-out %/wasapi_backend.c %/wasapi_capabilities.c, $(SRCS))
endif

ifneq ($(ENABLE_ASIO),1)
    SRCS := $(filter-out %/asio_backend.c %/asio_capabilities.c, $(SRCS))
endif

ifneq ($(ENABLE_JACK),1)
    SRCS := $(filter-out %/jack_backend.c, $(SRCS))
endif

ifneq ($(ENABLE_BLUEZ),1)
    SRCS := $(filter-out %/bluez_backend.c, $(SRCS))
endif

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
	$(CC) $(CFLAGS) -c $< -o $@

# Archive object files into static library
$(LIB_TARGET): $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $(OBJS)

# Build test binaries by linking against libdsp.a
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
$(ROOT_DIR)/Tests/CLibTests/bin/test_hot_path_allocation: $(ROOT_DIR)/Tests/CLibTests/test_hot_path_allocation.c $(LIB_TARGET)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free -o $@
endif

$(ROOT_DIR)/Tests/CLibTests/bin/test_%: $(ROOT_DIR)/Tests/CLibTests/test_%.c $(LIB_TARGET)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -o $@

BENCH_NAMES := test_filter_benchmark test_dop_benchmark test_pipeline_benchmark test_resampler_matrix
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
	$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -o $@




clean:
	rm -rf $(OBJ_DIR) $(LIB_TARGET) $(ROOT_DIR)/Tests/CLibTests/bin $(ROOT_DIR)/Sources/CDSP/bin

