# Build Mode: release or debug
MODE ?= release

# # Engine: rust or c
ENGINE ?= c

ifeq ($(MODE),release)
	CARGO_FLAGS = --release
	SWIFT_FLAGS = -c release -Xcc -O3 -Xcc -mcpu=native -Xcc -flto=thin -Xcc -fvisibility=hidden -Xcc -ffp-contract=fast -Xcc -fno-math-errno -Xcc -funroll-loops -Xswiftc -cross-module-optimization -Xlinker -object_path_lto -Xlinker .build/$(BUILD_DIR)/lto.o
	BUILD_DIR = release
else
	CARGO_FLAGS =
	SWIFT_FLAGS = -c debug
	BUILD_DIR = debug
endif

# App Metadata
APP_NAME = DSPMonitor
APP_BUNDLE = $(APP_NAME).app
CONTENTS = $(APP_BUNDLE)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources
EXECUTABLE = .build/$(BUILD_DIR)/$(APP_NAME)

# Tools
SWIFT := swift

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(GITHUB_ACTIONS),true)
	CARGO_CMD := MACOSX_DEPLOYMENT_TARGET=15.0 cargo
else
	ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
		CARGO_BIN := $(shell which cargo 2>/dev/null || echo "$(USERPROFILE)/.cargo/bin/cargo.exe")
		CARGO_CMD := RUSTFLAGS='-C target-cpu=native' "$(CARGO_BIN)"
	else
		CARGO_CMD := MACOSX_DEPLOYMENT_TARGET=15.0 RUSTFLAGS='-C target-cpu=native' cargo
	endif
endif

export ENGINE

ifeq ($(ENGINE),rust)
	# Rust FFI path
	ROOT_DIR := $(shell pwd)
	RUST_BRIDGE_DIR := $(ROOT_DIR)/RustBridge
	UDL_FILE := $(RUST_BRIDGE_DIR)/src/api.udl
	RUST_SRCS := $(shell find $(RUST_BRIDGE_DIR)/src -type f) $(RUST_BRIDGE_DIR)/Cargo.toml
	SWIFT_SRCS := $(shell find Sources -type f -name "*.swift")
	UNIFFI_BINDGEN := $(CARGO_CMD) run $(CARGO_FLAGS) --bin uniffi-bindgen --
else
	# C path
	SWIFT_SRCS := $(shell find Sources -type f -name "*.swift" -not -name "CamillaDSP.swift" -not -name "camilladsp_ffi.swift")
endif

.PHONY: all build app run clean install help test test-swift

# Default target
all: app

ifeq ($(ENGINE),rust)
# 1. Build Rust library
$(RUST_BRIDGE_DIR)/target/$(BUILD_DIR)/libcamilladsp_ffi.a: $(RUST_SRCS)
	@echo "🦀 Building Rust bridge ($(MODE))..."
	cd $(RUST_BRIDGE_DIR) && $(CARGO_CMD) build $(CARGO_FLAGS)

# 2. Generate UniFFI bindings
$(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffi.swift: $(UDL_FILE)
	@echo "🧬 Generating UniFFI bindings..."
	@mkdir -p $(RUST_BRIDGE_DIR)/generated/swift
	cd $(RUST_BRIDGE_DIR) && $(UNIFFI_BINDGEN) generate src/api.udl --language swift --out-dir generated/swift

# 3. Sync artifacts to Swift project (Only if changed to preserve timestamps)
lib/libcamilladsp_ffi.a: $(RUST_BRIDGE_DIR)/target/$(BUILD_DIR)/libcamilladsp_ffi.a
	@mkdir -p lib
	@if ! cmp -s $< $@; then \
		echo "📂 Updating library artifact..."; \
		cp $< $@; \
		ranlib $@; \
	fi

Sources/CamillaDSPFFI/include/camilladsp_ffiFFI.h: $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffiFFI.h
	@mkdir -p Sources/CamillaDSPFFI/include
	@if ! cmp -s $< $@; then \
		echo "📂 Updating C header..."; \
		cp $< $@; \
	fi

Sources/CamillaDSPFFI/include/module.modulemap: $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffiFFI.modulemap
	@mkdir -p Sources/CamillaDSPFFI/include
	@if ! cmp -s $< $@; then \
		echo "📂 Updating module map..."; \
		cp $< $@; \
	fi

Sources/DSPLib/camilladsp_ffi.swift: $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffi.swift
	@mkdir -p Sources/DSPLib
	@if ! cmp -s $< $@; then \
		echo "📂 Updating Swift bindings..."; \
		cp $< $@; \
	fi

# 4. Build Swift application (Rust path)
$(EXECUTABLE): lib/libcamilladsp_ffi.a Sources/DSPLib/camilladsp_ffi.swift Sources/CamillaDSPFFI/include/camilladsp_ffiFFI.h Sources/CamillaDSPFFI/include/module.modulemap $(SWIFT_SRCS) Package.swift
	@echo "🍎 Building Swift application with Rust library ($(MODE))..."
	$(SWIFT) build $(SWIFT_FLAGS) --product DSPMonitor

else
# Build Swift application (C path)
$(EXECUTABLE): $(SWIFT_SRCS) Package.swift
	@echo "🍎 Building Swift application with pure C library ($(MODE))..."
	$(SWIFT) build $(SWIFT_FLAGS) --product DSPMonitor
endif

## build: Build the binary with incremental tracking
build: $(EXECUTABLE)
	@echo "\n✅ Build Complete!"
	@echo "📍 Binary location: $(EXECUTABLE)"

## app: Build and package as a macOS Application (.app)
app: build
	@echo "📦 Packaging as $(APP_BUNDLE)..."
	@mkdir -p $(MACOS)
	@mkdir -p $(RESOURCES)
	@cp $(EXECUTABLE) $(MACOS)/
	@echo "📄 Copying Info.plist..."
	@cp Info.plist $(CONTENTS)/
	@if [ -f "AppIcon.icns" ]; then \
		echo "🖼️  Copying AppIcon.icns..."; \
		cp AppIcon.icns $(RESOURCES)/; \
	fi
	@echo "✍️  Signing application..."
	@if [ -f "entitlements.plist" ]; then \
		codesign --force --options runtime --entitlements entitlements.plist --sign - $(APP_BUNDLE); \
	else \
		codesign --force --sign - $(APP_BUNDLE); \
	fi
	@echo "✅ App bundle created at $(APP_BUNDLE)"

## install: Install the app to /Applications/
install: app
	@echo "📦 Installing $(APP_BUNDLE) to /Applications/..."
	cp -R $(APP_BUNDLE) /Applications/
	@echo "✅ Installed!"

## run: Build the application package and run it
run: app
	@echo "🚀 Running $(APP_NAME)..."
	open $(APP_BUNDLE)

## test: Run the Swift RoomCorrection test suite
test:
	@echo "🧪 Running Swift tests..."
	$(SWIFT) test

## clean: Remove all build artifacts
clean:
	@echo "🧹 Cleaning up..."
	rm -rf .build
	rm -rf $(APP_BUNDLE)
	rm -rf lib
	rm -rf Sources/CamillaDSPFFI/include
	rm -f Sources/DSPLib/camilladsp_ffi.swift
	@if [ -d RustBridge ]; then \
		echo "🧹 Cleaning Rust bridge..."; \
		cd RustBridge && $(CARGO_CMD) clean && rm -rf generated; \
	fi
	@echo "✨ Cleaned!"

## help: Show this help message
help:
	@echo "Usage: make [target] [ENGINE=rust|c] [MODE=release|debug]"
	@echo ""
	@echo "Targets:"
	@sed -n 's/^##//p' Makefile | column -t -s ':' |  sed -e 's/^/ /'
