# Build Mode: release or debug
MODE ?= release

ifeq ($(MODE),release)
	SWIFT_FLAGS = -c release
	BUILD_DIR = release
else
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
SWIFT_SRCS := $(shell find Sources -type f -name "*.swift")

.PHONY: all build app run clean install help test bench

# Default target
all: app

# Build Swift application
$(EXECUTABLE): $(SWIFT_SRCS) Package.swift
	@echo "🍎 Building Swift application ($(MODE))..."
	$(SWIFT) build $(SWIFT_FLAGS)

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

## test: Run the Swift test suite
test:
	@echo "🧪 Running Swift tests..."
	$(SWIFT) test --skip FilterBenchmarkTests

## bench: Run the resampler benchmark suite in release mode
bench:
	@echo "⏱️  Running Filter benchmarks in release mode..."
	$(SWIFT) test -c release --filter FilterBenchmarkTests

## clean: Remove all build artifacts
clean:
	@echo "🧹 Cleaning up..."
	rm -rf .build
	rm -rf $(APP_BUNDLE)
	@echo "✨ Cleaned!"

## help: Show this help message
help:
	@echo "Usage: make [target] [MODE=release|debug]"
	@echo ""
	@echo "Targets:"
	@sed -n 's/^##//p' Makefile | column -t -s ':' |  sed -e 's/^/ /'
