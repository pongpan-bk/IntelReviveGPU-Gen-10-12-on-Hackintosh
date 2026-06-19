# Makefile สำหรับ MyIntelGPU.kext
#
#  Build บน macOS เท่านั้น! ต้องการ:
#    - Xcode Command Line Tools (xcode-select --install)
#    - MacOSKernelSDK (https://github.com/acidanthera/MacOSKernelSDK)
#      หรือ Xcode เก่า (< 14) ที่ยังมี Kernel.framework
#
#  วิธี build:
#    make
#    sudo chown -R root:wheel MyIntelGPU.kext
#    sudo kextutil -v MyIntelGPU.kext
#
#  ดู log:
#    sudo dmesg | grep MyIntelGPU

TARGET  = MyIntelGPU
CLASS   = MyIntelGPU
SDK_DIR ?= /opt/MacKernelSDK
ifneq ($(KERNEL_SDK_DIR),)
    SDK_DIR := $(KERNEL_SDK_DIR)
endif

# ─── ค้นหา SDK path ──────────────────────────────────────────────
SDK_PATH = $(shell xcrun --show-sdk-path 2>/dev/null)
ifeq ($(SDK_PATH),)
    $(error ERROR: Xcode SDK not found. Run xcode-select --install)
endif

# ─── Kernel Headers ──────────────────────────────────────────────
# MacKernelSDK structure: /Headers/IOKit/, /Headers/libkern/, etc.
# 1st: MacKernelSDK Headers/
# 2nd: MacKernelSDK Kernel.framework (older layout)
# 3rd: Built-in Xcode SDK fallback
KERNEL_HDRS = $(SDK_DIR)
ifneq ($(wildcard $(SDK_DIR)/Headers),)
    KERNEL_HDRS := $(SDK_DIR)/Headers
else ifneq ($(wildcard $(SDK_DIR)/Kernel.framework/Headers),)
    KERNEL_HDRS := $(SDK_DIR)/Kernel.framework/Headers
else
    KERNEL_HDRS := $(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers
endif

# ─── Compiler Flags ──────────────────────────────────────────────
CXXFLAGS = -std=c++11 \
           -mkernel \
           -arch x86_64 \
           -nostdlib \
           -fno-builtin \
           -fno-exceptions \
           -fno-rtti \
           -DKERNEL \
           -D__STRICT_BSD__ \
           -I$(KERNEL_HDRS)

# ─── Linker Flags ────────────────────────────────────────────────
# -r              : relocatable object (kext = kernel module)
# -keep_private_extern : keep private symbols for kext
LDFLAGS = -r -keep_private_externs

# ─── Sources ─────────────────────────────────────────────────────
SRC = $(CLASS).cpp
OBJ = $(CLASS).o

.PHONY: all clean install load unload

all: $(TARGET).kext/Contents/MacOS/$(TARGET)

# ─── Build kext bundle ──────────────────────────────────────────
$(TARGET).kext/Contents/MacOS/$(TARGET): $(SRC) $(CLASS).hpp Info.plist
	@mkdir -p $(TARGET).kext/Contents/MacOS
	cp Info.plist $(TARGET).kext/Contents/Info.plist
	$(CC) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRC)
	@echo "─── Build complete: $(TARGET).kext ───"

# ─── Utilities ───────────────────────────────────────────────────
clean:
	rm -rf $(TARGET).kext $(OBJ)

install: all
	sudo chown -R root:wheel $(TARGET).kext
	sudo cp -R $(TARGET).kext /Library/Extensions/
	sudo kextutil -v /Library/Extensions/$(TARGET).kext

load: all
	sudo chown -R root:wheel $(TARGET).kext
	sudo kextutil -v $(TARGET).kext

unload:
	sudo kextunload -b com.myintelgpu.driver || true

log:
	sudo dmesg | grep -i "MyIntelGPU" | tail -50
