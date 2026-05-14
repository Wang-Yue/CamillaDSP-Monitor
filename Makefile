# Build Mode: release or debug
MODE ?= release

# Engine: swift or rust
ENGINE ?= swift

ifeq ($(MODE),release)
	CARGO_FLAGS = --release
	SWIFT_FLAGS = -c release
	BUILD_DIR = release
else
	CARGO_FLAGS =
	SWIFT_FLAGS = -c debug
	BUILD_DIR = debug
endif

# App Metadata
APP_NAME = CamillaDSPMonitor
APP_BUNDLE = $(APP_NAME).app
CONTENTS = $(APP_BUNDLE)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources
EXECUTABLE = .build/$(BUILD_DIR)/$(APP_NAME)

# Tools
SWIFT := swift

ifeq ($(GITHUB_ACTIONS),true)
	CARGO_CMD := MACOSX_DEPLOYMENT_TARGET=15.0 cargo
else
	CARGO_CMD := MACOSX_DEPLOYMENT_TARGET=15.0 RUSTFLAGS='-C target-cpu=native' cargo
endif


ifeq ($(ENGINE),swift)
	export USE_PURE_SWIFT=1
	SWIFT_SRCS := $(shell find Sources -type f -name "*.swift" -not -name "CamillaDSP.swift" -not -name "camilladsp_ffi.swift")
else
	export USE_PURE_SWIFT=0
	# Rust FFI path
	ROOT_DIR := $(shell pwd)
	RUST_BRIDGE_DIR := $(ROOT_DIR)/RustBridge
	UDL_FILE := $(RUST_BRIDGE_DIR)/src/api.udl
	RUST_SRCS := $(shell find $(RUST_BRIDGE_DIR)/src -type f) $(RUST_BRIDGE_DIR)/Cargo.toml
	SWIFT_SRCS := $(shell find Sources -type f -name "*.swift")
	

	UNIFFI_BINDGEN := $(CARGO_CMD) run $(CARGO_FLAGS) --bin uniffi-bindgen --
endif

# Rust harness layout (tests against rubato + camilladsp upstream).
RUST_HARNESS_DIR := Tests/RustHarnesses
RUST_HARNESS_BINS := \
	$(RUST_HARNESS_DIR)/target/release/cdsp_resampler_compare \
	$(RUST_HARNESS_DIR)/target/release/cdsp_filter_compare
RUST_HARNESS_SRCS := $(shell find $(RUST_HARNESS_DIR) -type f \
	\( -name "*.rs" -o -name "Cargo.toml" \) 2>/dev/null)

.PHONY: all build app clean install help test test-swift test-rust-build bench

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

Sources/CamillaDSPLib/camilladsp_ffi.swift: $(RUST_BRIDGE_DIR)/generated/swift/camilladsp_ffi.swift
	@mkdir -p Sources/CamillaDSPLib
	@if ! cmp -s $< $@; then \
		echo "📂 Updating Swift bindings..."; \
		cp $< $@; \
	fi

# 4. Build Swift application (Rust path)
$(EXECUTABLE): lib/libcamilladsp_ffi.a Sources/CamillaDSPLib/camilladsp_ffi.swift Sources/CamillaDSPFFI/include/camilladsp_ffiFFI.h Sources/CamillaDSPFFI/include/module.modulemap $(SWIFT_SRCS) Package.swift
	@echo "🍎 Building Swift application with Rust library ($(MODE))..."
	$(SWIFT) build $(SWIFT_FLAGS)

else
# Build Swift application (Swift path)
$(EXECUTABLE): $(SWIFT_SRCS) Package.swift
	@echo "🍎 Building Swift application with pure Swift library ($(MODE))..."
	$(SWIFT) build $(SWIFT_FLAGS)
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

## test-rust-build: Build the Rust harness binaries used by Swift tests
##                  (rubato + camilladsp upstream). Pure Swift only.
test-rust-build:
ifeq ($(ENGINE),rust)
	$(error Tests are only supported for the pure Swift engine (ENGINE=swift))
else
	@$(MAKE) $(RUST_HARNESS_BINS)
endif

$(RUST_HARNESS_BINS): $(RUST_HARNESS_SRCS)
	@echo "🦀 Building Rust harness binaries..."
	cd $(RUST_HARNESS_DIR) && $(CARGO_CMD) build --release
	@touch $(RUST_HARNESS_BINS)

## test-swift: Run only the Swift test suite (pure Swift path only)
test-swift:
ifeq ($(ENGINE),rust)
	$(error Tests are only supported for the pure Swift engine (ENGINE=swift))
else
	@echo "🧪 Running Swift tests..."
	$(SWIFT) test --skip ResamplerComparisonMatrix --skip FilterBenchmarkTests
endif

## test: Build the Rust harnesses and run the full Swift test suite (pure Swift path only)
test:
ifeq ($(ENGINE),rust)
	$(error Tests are only supported for the pure Swift engine (ENGINE=swift))
else
	@$(MAKE) test-rust-build
	@echo "🧪 Running Swift tests (with Rust harness comparison tests enabled)..."
	$(SWIFT) test -c release --skip ResamplerComparisonMatrix --skip FilterBenchmarkTests
endif

## bench: Run the resampler benchmark suite in release mode (pure Swift path only)
bench:
ifeq ($(ENGINE),rust)
	$(error Benchmarks are only supported for the pure Swift engine (ENGINE=swift))
else
	@$(MAKE) test-rust-build
	@echo "⏱️  Running Filter benchmarks in release mode..."
	$(SWIFT) test -c release --filter FilterBenchmarkTests
	@echo "⏱️  Running Resampler benchmarks in release mode..."
	$(SWIFT) test -c release --filter ResamplerComparisonMatrix
endif

## clean: Remove all build artifacts
clean:
	@echo "🧹 Cleaning up..."
	rm -rf .build
	rm -rf $(APP_BUNDLE)
	rm -rf $(RUST_HARNESS_DIR)/target
	rm -rf lib
	rm -rf Sources/CamillaDSPFFI/include
	rm -f Sources/CamillaDSPLib/camilladsp_ffi.swift
	@if [ -d RustBridge ]; then \
		echo "🧹 Cleaning Rust bridge..."; \
		cd RustBridge && $(CARGO_CMD) clean && rm -rf generated; \
	fi
	@echo "✨ Cleaned!"

## help: Show this help message
help:
	@echo "Usage: make [target] [ENGINE=swift|rust] [MODE=release|debug]"
	@echo ""
	@echo "Targets:"
	@sed -n 's/^##//p' Makefile | column -t -s ':' |  sed -e 's/^/ /'
