# PlateMask - DaVinci Resolve OpenFX plugin. `make` builds the plugin bundle, `make test` runs the core tests.
CXX      ?= clang++
STD      := -std=c++17
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
OPT      ?= -O2
INC      := -Isrc -Ithird_party/openfx/include -Ithird_party/openfx/Support/include
CXXFLAGS := $(STD) $(WARN) $(OPT) $(INC) -fvisibility=hidden -fPIC
UNAME    := $(shell uname -s)

BUILD    := build
CORE_SRC := $(wildcard src/core/*.cpp)
CORE_OBJ := $(patsubst src/core/%.cpp,$(BUILD)/core/%.o,$(CORE_SRC))
SUPPORT_SRC := ofxsCore.cpp ofxsImageEffect.cpp ofxsInteract.cpp ofxsLog.cpp ofxsMultiThread.cpp ofxsParams.cpp ofxsProperty.cpp ofxsPropertyValidation.cpp
SUPPORT_OBJ := $(patsubst %.cpp,$(BUILD)/support/%.o,$(SUPPORT_SRC))
PLUGIN_SRC := $(wildcard src/ofx/*.cpp)
PLUGIN_OBJ := $(patsubst src/ofx/%.cpp,$(BUILD)/ofx/%.o,$(PLUGIN_SRC))
PLUGIN_MM  := $(wildcard src/ofx/*.mm)

NAME     := PlateMask
BUNDLE   := $(BUILD)/$(NAME).ofx.bundle

ifeq ($(UNAME),Darwin)
  ARCH_FLAGS ?= -arch arm64 -arch x86_64
  PLUGIN_CXXFLAGS := $(CXXFLAGS) $(ARCH_FLAGS) -mmacosx-version-min=11.0
  PLUGIN_LDFLAGS  := -bundle $(ARCH_FLAGS) -mmacosx-version-min=11.0 -fvisibility=hidden -framework Cocoa
  PLUGIN_OBJ += $(patsubst src/ofx/%.mm,$(BUILD)/ofx/%.o,$(PLUGIN_MM))
  BIN_DIR  := $(BUNDLE)/Contents/MacOS
  INSTALL_DIR := /Library/OFX/Plugins
else
  PLUGIN_CXXFLAGS := $(CXXFLAGS)
  PLUGIN_LDFLAGS  := -shared -fvisibility=hidden -lpthread
  BIN_DIR  := $(BUNDLE)/Contents/Linux-x86-64
  INSTALL_DIR := /usr/OFX/Plugins
endif

.PHONY: all test plugin install clean cli dist

all: plugin

# ---- core (host arch, for tests and cli)
$(BUILD)/core/%.o: src/core/%.cpp src/core/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test_core: tests/test_core.cpp tests/Synth.h $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) tests/test_core.cpp $(CORE_OBJ) -o $@

test: $(BUILD)/test_core
	./$(BUILD)/test_core

$(BUILD)/platemask_cli: tools/platemask_cli.cpp tests/Synth.h $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) -Itests tools/platemask_cli.cpp $(CORE_OBJ) -o $@

cli: $(BUILD)/platemask_cli

# ---- plugin (universal on macOS)
$(BUILD)/pcore/%.o: src/core/%.cpp src/core/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(PLUGIN_CXXFLAGS) -c $< -o $@

$(BUILD)/support/%.o: third_party/openfx/Support/Library/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PLUGIN_CXXFLAGS) -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -c $< -o $@

$(BUILD)/ofx/%.o: src/ofx/%.cpp src/ofx/*.h src/core/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(PLUGIN_CXXFLAGS) -c $< -o $@

$(BUILD)/ofx/%.o: src/ofx/%.mm src/ofx/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(PLUGIN_CXXFLAGS) -fobjc-arc -c $< -o $@

PCORE_OBJ := $(patsubst src/core/%.cpp,$(BUILD)/pcore/%.o,$(CORE_SRC))

$(BIN_DIR)/$(NAME).ofx: $(PLUGIN_OBJ) $(PCORE_OBJ) $(SUPPORT_OBJ)
	@mkdir -p $(BIN_DIR)
	$(CXX) $^ -o $@ $(PLUGIN_LDFLAGS)

plugin: $(BIN_DIR)/$(NAME).ofx
	cp packaging/Info.plist $(BUNDLE)/Contents/Info.plist
	@echo "Built $(BUNDLE)"

install: plugin
	sudo rm -rf "$(INSTALL_DIR)/$(NAME).ofx.bundle"
	sudo cp -R "$(BUNDLE)" "$(INSTALL_DIR)/"
	@echo "Installed to $(INSTALL_DIR)/$(NAME).ofx.bundle - restart DaVinci Resolve"

VERSION := $(shell /usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' packaging/Info.plist 2>/dev/null || echo 1.0.0)
DIST := $(BUILD)/dist/PlateMask-$(VERSION)-macOS

# Release artefacts: a zip (bundle + install script) and an unsigned .pkg installer.
dist: plugin
	rm -rf $(DIST) $(DIST).zip $(DIST).pkg
	mkdir -p $(DIST)
	cp -R $(BUNDLE) $(DIST)/
	cp packaging/install.sh packaging/uninstall.sh README.md $(DIST)/
	cd $(BUILD)/dist && zip -qry PlateMask-$(VERSION)-macOS.zip PlateMask-$(VERSION)-macOS
	mkdir -p $(BUILD)/pkgroot && rm -rf $(BUILD)/pkgroot/* && cp -R $(BUNDLE) $(BUILD)/pkgroot/
	pkgbuild --analyze --root $(BUILD)/pkgroot $(BUILD)/pkgroot.plist >/dev/null
	/usr/libexec/PlistBuddy -c 'Set :0:BundleIsRelocatable false' $(BUILD)/pkgroot.plist 2>/dev/null || true
	pkgbuild --root $(BUILD)/pkgroot --component-plist $(BUILD)/pkgroot.plist --install-location /Library/OFX/Plugins --identifier ch.platemask.ofx --version $(VERSION) $(DIST).pkg >/dev/null
	@echo "Release files:"; ls -la $(DIST).zip $(DIST).pkg

clean:
	rm -rf $(BUILD)
