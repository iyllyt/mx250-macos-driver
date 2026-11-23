SDK      := $(shell xcrun --show-sdk-path)
CXX      := clang++
TARGET   := x86_64-apple-macos15.5

BUILD_DIR := build
KEXT_BUNDLE := $(BUILD_DIR)/GP108Bridge.kext
KEXT_BIN    := $(KEXT_BUNDLE)/Contents/MacOS/GP108Bridge
KEXT_PLIST  := $(KEXT_BUNDLE)/Contents/Info.plist
PROBE_BIN   := $(BUILD_DIR)/gp108-probe

KEXT_CXXFLAGS := \
	-std=gnu++17 \
	-target $(TARGET) \
	-isysroot $(SDK) \
	-isystem $(SDK)/System/Library/Frameworks/Kernel.framework/Headers \
	-Iinclude \
	-mkernel \
	-fno-builtin \
	-fno-common \
	-fno-exceptions \
	-fno-rtti \
	-fno-stack-protector \
	-DKERNEL \
	-DKERNEL_PRIVATE \
	-DDRIVER_PRIVATE \
	-DAPPLE \
	-DNeXT \
	-Wall -Wextra -Werror -Wno-deprecated-declarations -O2

KEXT_LDFLAGS := \
	-target $(TARGET) \
	-isysroot $(SDK) \
	-Wl,-kext \
	-nostdlib \
	-L$(SDK)/usr/lib \
	-lkmodc++ \
	-lkmod

USER_CXXFLAGS := \
	-std=c++17 \
	-target $(TARGET) \
	-isysroot $(SDK) \
	-Iinclude \
	-Wall -Wextra -Werror -O2

USER_LDFLAGS := \
	-framework CoreFoundation \
	-framework IOKit \
	-framework IOSurface

.PHONY: all clean

all: $(KEXT_BIN) $(KEXT_PLIST) $(PROBE_BIN)

$(BUILD_DIR)/GP108Bridge.o: kext/GP108Bridge.cpp kext/GP108Bridge.hpp include/GP108BridgeShared.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

$(KEXT_BIN): $(BUILD_DIR)/GP108Bridge.o
	@mkdir -p $(dir $@)
	$(CXX) $< $(KEXT_LDFLAGS) -o $@

$(KEXT_PLIST): kext/Info.plist
	@mkdir -p $(dir $@)
	cp $< $@

$(PROBE_BIN): tools/gp108_probe.cpp include/GP108BridgeShared.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(USER_CXXFLAGS) $< $(USER_LDFLAGS) -o $@

clean:
	rm -rf $(BUILD_DIR)
