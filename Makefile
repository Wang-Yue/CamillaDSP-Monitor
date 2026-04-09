APP_NAME = CamillaDSPMonitor
BUNDLE_ID = com.example.CamillaDSPMonitor
EXECUTABLE = .build/release/$(APP_NAME)
APP_BUNDLE = $(APP_NAME).app
CONTENTS = $(APP_BUNDLE)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources

.PHONY: all build app clean install

all: app

build:
	swift build -c release --disable-sandbox

app: build
	mkdir -p $(MACOS)
	mkdir -p $(RESOURCES)
	cp $(EXECUTABLE) $(MACOS)/
	@echo "Copying Info.plist..."
	cp Info.plist $(CONTENTS)/
	@if [ -f "AppIcon.icns" ]; then 		echo "Copying AppIcon.icns..."; 		cp AppIcon.icns $(RESOURCES)/; 	fi
	@echo "Signing application..."
	@if [ -f "entitlements.plist" ]; then 		codesign --force --options runtime --entitlements entitlements.plist --sign - $(APP_BUNDLE); 	else 		codesign --force --sign - $(APP_BUNDLE); 	fi

clean:
	rm -rf .build
	rm -rf $(APP_BUNDLE)

install: app
	cp -R $(APP_BUNDLE) /Applications/
