# Makefile for CamillaDSP-Monitor (Library Mode)

# App Metadata
APP_NAME = CamillaDSPMonitor
APP_BUNDLE = $(APP_NAME).app
CONTENTS = $(APP_BUNDLE)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources
EXECUTABLE = .build/release/$(APP_NAME)
INSTALL_BIN_PATH = /usr/local/bin/$(APP_NAME)

# Paths
ROOT_DIR := $(shell pwd)
RUST_BRIDGE_DIR := $(ROOT_DIR)/RustBridge
SWIFT_APP_DIR := $(ROOT_DIR)

# Tools
CARGO := cargo
SWIFT := swift
UNIFFI_BINDGEN := $(CARGO) run --bin uniffi-bindgen --

# Source Files
RUST_SRCS := $(shell find $(RUST_BRIDGE_DIR)/src -type f) $(RUST_BRIDGE_DIR)/Cargo.toml
SWIFT_SRCS := $(shell find $(SWIFT_APP_DIR)/Sources -type f -not -name "camilladsp_ffi.swift")
UDL_FILE := $(RUST_BRIDGE_DIR)/src/api.udl

.PHONY: all build app clean install help

# Default target
all: app

# 1. Build Rust library
$(RUST_BRIDGE_DIR)/target/release/libcamilladsp_ffi.a: $(RUST_SRCS)
	@echo "🦀 Building Rust bridge..."
	cd $(RUST_BRIDGE_DIR) && RUSTFLAGS='-C target-cpu=native' $(CARGO) build --release

# 2. Generate UniFFI bindings
$(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffi.swift: $(UDL_FILE)
	@echo "🧬 Generating UniFFI bindings..."
	@mkdir -p $(RUST_BRIDGE_DIR)/generated/swift
	cd $(RUST_BRIDGE_DIR) && $(UNIFFI_BINDGEN) generate src/api.udl --language swift --out-dir generated/swift

# 3. Sync artifacts to Swift project (Only if changed to preserve timestamps)
lib/libcamilladsp_ffi.a: $(RUST_BRIDGE_DIR)/target/release/libcamilladsp_ffi.a
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

Sources/CamillaDSPLib/camilladsp_ffi.swift: $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffi.swift
	@mkdir -p Sources/CamillaDSPLib
	@cp $< $@.tmp
	@echo "🔧 Patching Swift code for concurrency safety..."
	@sed -i '' 's/private var initializationResult: InitializationResult/private nonisolated(unsafe) var initializationResult: InitializationResult/g' $@.tmp
	@if ! cmp -s $@.tmp $@; then \
		echo "📂 Updating Swift bindings..."; \
		mv $@.tmp $@; \
	else \
		rm $@.tmp; \
	fi

# 4. Build Swift application
$(EXECUTABLE): lib/libcamilladsp_ffi.a Sources/CamillaDSPLib/camilladsp_ffi.swift Sources/CamillaDSPFFI/include/camilladsp_ffiFFI.h Sources/CamillaDSPFFI/include/module.modulemap $(SWIFT_SRCS)
	@echo "🍎 Building Swift application..."
	cd $(SWIFT_APP_DIR) && $(SWIFT) build -c release

## build: Build everything with incremental tracking
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

## clean: Remove all build artifacts
clean:
	@echo "🧹 Cleaning up..."
	cd $(RUST_BRIDGE_DIR) && $(CARGO) clean
	cd $(RUST_BRIDGE_DIR) && rm -rf generated
	cd $(SWIFT_APP_DIR) && rm -rf .build
	rm -rf $(SWIFT_APP_DIR)/lib
	rm -rf $(SWIFT_APP_DIR)/Sources/CamillaDSPFFI/include
	rm -f $(SWIFT_APP_DIR)/Sources/CamillaDSPLib/camilladsp_ffi.swift
	rm -rf $(APP_BUNDLE)
	@echo "✨ Cleaned!"

## help: Show this help message
help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@sed -n 's/^##//p' Makefile | column -t -s ':' |  sed -e 's/^/ /'
