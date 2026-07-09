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
    # Linux
    CFLAGS += -march=native -D_GNU_SOURCE
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
    ifeq ($(ENABLE_JACK),1)
        CFLAGS += -DENABLE_JACK
        LDFLAGS += -ljack
    endif
    ifeq ($(ENABLE_BLUEZ),1)
        CFLAGS += -DENABLE_BLUEZ
        LDFLAGS += $(shell pkg-config --libs dbus-1 2>/dev/null || echo "-ldbus-1")
        CFLAGS += $(shell pkg-config --cflags dbus-1 2>/dev/null || echo "")
    endif
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
$(ROOT_DIR)/Tests/CLibTests/bin/test_%: $(ROOT_DIR)/Tests/CLibTests/test_%.c $(LIB_TARGET)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LIB_TARGET) $(LDFLAGS) -o $@

BENCH_NAMES := test_filter_benchmark test_dop_benchmark test_resampler_matrix test_pipeline_benchmark
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

# Windows cross-compilation settings
WIN_CC := x86_64-w64-mingw32-gcc
WIN_AR := x86_64-w64-mingw32-ar
WIN_OBJ_DIR := $(ROOT_DIR)/.build/win_obj

# Filter Windows source files (exclude Linux/Apple specific backends)
WIN_SRCS := $(filter-out %/alsa_capture.c %/alsa_playback.c %/alsa_device.c %/alsa_capabilities.c \
                        %/pulse_backend.c \
                        %/pipewire_backend.c \
                        %/core_audio_capture.c %/core_audio_playback.c %/core_audio_device.c %/core_audio_capabilities.c \
                        %/jack_backend.c %/bluez_backend.c, $(ALL_SRCS))

WIN_OBJS := $(patsubst $(ROOT_DIR)/%.c, $(WIN_OBJ_DIR)/%.o, $(WIN_SRCS))
WIN_LIB_TARGET := $(ROOT_DIR)/Sources/CDSP/libdsp_win.a

$(WIN_OBJ_DIR)/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(WIN_CC) -O3 -Wall -Wextra -std=c11 -D_WIN32 -DUNICODE -D_UNICODE -DCOBJMACROS -DENABLE_WASAPI -DENABLE_ASIO -DENABLE_FFTW -I/tmp/win_fftw -I$(ROOT_DIR) -I$(ROOT_DIR)/Sources/CDSP -I$(ROOT_DIR)/Sources/CDSP/Filters -I$(ROOT_DIR)/Sources/CDSP/Audio -I$(ROOT_DIR)/Sources/CDSP/Config -I$(ROOT_DIR)/Sources/CDSP/FFT -I$(ROOT_DIR)/Sources/CDSP/Mixer -I$(ROOT_DIR)/Sources/CDSP/Resampler -I$(ROOT_DIR)/Sources/CDSP/Processors -I$(ROOT_DIR)/Sources/CDSP/DoP -I$(ROOT_DIR)/Sources/CDSP/Pipeline -I$(ROOT_DIR)/Sources/CDSP/Engine -I$(ROOT_DIR)/Sources/CDSP/Server -I$(ROOT_DIR)/Sources/CDSP/Backend -I$(ROOT_DIR)/Sources/CDSP/Logging -c $< -o $@

WIN_CLI_BIN := $(ROOT_DIR)/Sources/CDSP/bin/dsp-cli.exe

.PHONY: windows windows-lib windows-cli
windows: windows-lib windows-cli

windows-lib: $(WIN_OBJS)
	@rm -f $(WIN_LIB_TARGET)
	$(WIN_AR) rcs $(WIN_LIB_TARGET) $(WIN_OBJS)

windows-cli: $(WIN_LIB_TARGET)
	@mkdir -p $(dir $(WIN_CLI_BIN))
	$(WIN_CC) -O3 -Wall -Wextra -std=c11 -D_WIN32 -DUNICODE -D_UNICODE -DCOBJMACROS -DENABLE_WASAPI -DENABLE_ASIO -DENABLE_FFTW -I/tmp/win_fftw -I$(ROOT_DIR) -I$(ROOT_DIR)/Sources/CDSP -I$(ROOT_DIR)/Sources/CDSP/Filters -I$(ROOT_DIR)/Sources/CDSP/Audio -I$(ROOT_DIR)/Sources/CDSP/Config -I$(ROOT_DIR)/Sources/CDSP/FFT -I$(ROOT_DIR)/Sources/CDSP/Mixer -I$(ROOT_DIR)/Sources/CDSP/Resampler -I$(ROOT_DIR)/Sources/CDSP/Processors -I$(ROOT_DIR)/Sources/CDSP/DoP -I$(ROOT_DIR)/Sources/CDSP/Pipeline -I$(ROOT_DIR)/Sources/CDSP/Engine -I$(ROOT_DIR)/Sources/CDSP/Server -I$(ROOT_DIR)/Sources/CDSP/Backend -I$(ROOT_DIR)/Sources/CDSP/Logging $(CLI_SRC) $(WIN_LIB_TARGET) /tmp/win_fftw/libfftw3-3.dll /tmp/win_fftw/libfftw3f-3.dll -pthread -lws2_32 -lwinmm -lole32 -luuid -lksuser -lksguid -o $(WIN_CLI_BIN)
# macOS cross-compilation settings
MAC_CC := clang
MAC_SDK_PATH ?= /usr/local/google/home/wangyue/.gemini/jetski/scratch/mac_sdk/MacOSX13.3.sdk
MAC_LDFLAGS := -F$(MAC_SDK_PATH)/System/Library/Frameworks -framework CoreAudio -framework AudioToolbox -framework Accelerate -framework CoreFoundation -fuse-ld=lld

MAC_OBJ_DIR_X86_64 := $(ROOT_DIR)/.build/mac_obj_x86_64
MAC_OBJ_DIR_ARM64 := $(ROOT_DIR)/.build/mac_obj_arm64

MAC_SRCS := $(filter-out %/alsa_capture.c %/alsa_playback.c %/alsa_device.c %/alsa_capabilities.c \
                         %/pulse_backend.c \
                         %/pipewire_backend.c \
                         %/wasapi_backend.c %/wasapi_capabilities.c \
                         %/asio_backend.c %/asio_capabilities.c \
                         %/jack_backend.c %/bluez_backend.c, $(ALL_SRCS))

MAC_OBJS_X86_64 := $(patsubst $(ROOT_DIR)/%.c, $(MAC_OBJ_DIR_X86_64)/%.o, $(MAC_SRCS))
MAC_OBJS_ARM64 := $(patsubst $(ROOT_DIR)/%.c, $(MAC_OBJ_DIR_ARM64)/%.o, $(MAC_SRCS))

MAC_LIB_X86_64 := $(ROOT_DIR)/Sources/CDSP/libdsp_mac_x86_64.a
MAC_LIB_ARM64 := $(ROOT_DIR)/Sources/CDSP/libdsp_mac_arm64.a

MAC_CLI_X86_64 := $(ROOT_DIR)/Sources/CDSP/bin/dsp-cli-mac-x86_64
MAC_CLI_ARM64 := $(ROOT_DIR)/Sources/CDSP/bin/dsp-cli-mac-arm64
MAC_CLI_UNIVERSAL := $(ROOT_DIR)/Sources/CDSP/bin/dsp-cli-mac

MAC_CFLAGS_COMMON := -O3 -Wall -Wextra -std=c11 -DUNICODE -D_UNICODE -DENABLE_COREAUDIO -DENABLE_ACCELERATE -isysroot $(MAC_SDK_PATH) -I$(ROOT_DIR) -I$(ROOT_DIR)/Sources/CDSP -I$(ROOT_DIR)/Sources/CDSP/Filters -I$(ROOT_DIR)/Sources/CDSP/Audio -I$(ROOT_DIR)/Sources/CDSP/Config -I$(ROOT_DIR)/Sources/CDSP/FFT -I$(ROOT_DIR)/Sources/CDSP/Mixer -I$(ROOT_DIR)/Sources/CDSP/Resampler -I$(ROOT_DIR)/Sources/CDSP/Processors -I$(ROOT_DIR)/Sources/CDSP/DoP -I$(ROOT_DIR)/Sources/CDSP/Pipeline -I$(ROOT_DIR)/Sources/CDSP/Engine -I$(ROOT_DIR)/Sources/CDSP/Server -I$(ROOT_DIR)/Sources/CDSP/Backend -I$(ROOT_DIR)/Sources/CDSP/Logging

$(MAC_OBJ_DIR_X86_64)/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(MAC_CC) -target x86_64-apple-darwin $(MAC_CFLAGS_COMMON) -c $< -o $@

$(MAC_OBJ_DIR_ARM64)/%.o: $(ROOT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(MAC_CC) -target arm64-apple-darwin $(MAC_CFLAGS_COMMON) -c $< -o $@

.PHONY: macos macos-lib-x86_64 macos-lib-arm64 macos-cli-x86_64 macos-cli-arm64 macos-universal

macos: macos-universal

macos-lib-x86_64: $(MAC_OBJS_X86_64)
	@rm -f $(MAC_LIB_X86_64)
	$(AR) rcs $(MAC_LIB_X86_64) $(MAC_OBJS_X86_64)

macos-lib-arm64: $(MAC_OBJS_ARM64)
	@rm -f $(MAC_LIB_ARM64)
	$(AR) rcs $(MAC_LIB_ARM64) $(MAC_OBJS_ARM64)

macos-cli-x86_64: macos-lib-x86_64
	@mkdir -p $(dir $(MAC_CLI_X86_64))
	$(MAC_CC) -target x86_64-apple-darwin $(MAC_CFLAGS_COMMON) $(CLI_SRC) $(MAC_LIB_X86_64) $(MAC_LDFLAGS) -o $(MAC_CLI_X86_64)

macos-cli-arm64: macos-lib-arm64
	@mkdir -p $(dir $(MAC_CLI_ARM64))
	$(MAC_CC) -target arm64-apple-darwin $(MAC_CFLAGS_COMMON) $(CLI_SRC) $(MAC_LIB_ARM64) $(MAC_LDFLAGS) -o $(MAC_CLI_ARM64)

macos-universal: macos-cli-x86_64 macos-cli-arm64
	@mkdir -p $(dir $(MAC_CLI_UNIVERSAL))
	llvm-lipo -create -output $(MAC_CLI_UNIVERSAL) $(MAC_CLI_X86_64) $(MAC_CLI_ARM64)

clean:
	rm -rf $(OBJ_DIR) $(WIN_OBJ_DIR) $(MAC_OBJ_DIR_X86_64) $(MAC_OBJ_DIR_ARM64) $(LIB_TARGET) $(WIN_LIB_TARGET) $(MAC_LIB_X86_64) $(MAC_LIB_ARM64) $(ROOT_DIR)/Tests/CLibTests/bin $(ROOT_DIR)/Sources/CDSP/bin

