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

.PHONY: all build app clean install help

# Default target
all: app

## build: Build everything in release mode
build:
	@echo "🚀 Starting CamillaDSP-Monitor Release Build..."
	
	@echo "🦀 Building Rust bridge in release mode (optimized for native CPU)..."
	cd $(RUST_BRIDGE_DIR) && RUSTFLAGS='-C target-cpu=native' $(CARGO) build --release
	
	@echo "🧬 Generating UniFFI bindings..."
	cd $(RUST_BRIDGE_DIR) && $(UNIFFI_BINDGEN) generate src/api.udl --language swift --out-dir generated/swift
	
	@echo "📂 Syncing artifacts to Swift project..."
	mkdir -p $(SWIFT_APP_DIR)/lib
	mkdir -p $(SWIFT_APP_DIR)/Sources/CamillaDSPFFI/include
	cp $(RUST_BRIDGE_DIR)/target/release/libcamilladsp_ffi.a $(SWIFT_APP_DIR)/lib/
	cp $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffiFFI.h $(SWIFT_APP_DIR)/Sources/CamillaDSPFFI/include/
	cp $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffiFFI.modulemap $(SWIFT_APP_DIR)/Sources/CamillaDSPFFI/include/module.modulemap
	cp $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffi.swift $(SWIFT_APP_DIR)/Sources/CamillaDSPLib/
	
	@echo "🔧 Patching generated Swift code for concurrency safety..."
	sed -i '' 's/private var initializationResult: InitializationResult/private nonisolated(unsafe) var initializationResult: InitializationResult/g' $(SWIFT_APP_DIR)/Sources/CamillaDSPLib/camilladsp_ffi.swift
	
	@echo "🔨 Running ranlib on static library..."
	ranlib $(SWIFT_APP_DIR)/lib/libcamilladsp_ffi.a
	
	@echo "🍎 Building Swift application in release mode..."
	cd $(SWIFT_APP_DIR) && $(SWIFT) build -c release
	
	@echo "\n✅ Build Complete!"
	@echo "📍 Binary location: $(EXECUTABLE)"

## app: Build and package as a macOS Application (.app)
app: build
	@echo "📦 Packaging as $(APP_BUNDLE)..."
	mkdir -p $(MACOS)
	mkdir -p $(RESOURCES)
	cp $(EXECUTABLE) $(MACOS)/
	@echo "📄 Copying Info.plist..."
	cp Info.plist $(CONTENTS)/
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
