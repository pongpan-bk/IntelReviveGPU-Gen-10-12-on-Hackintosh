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
# MacKernelSDK (acidanthera) structure: Headers/ directly
# ถ้าไม่พบ → fallback ใช้ SDK built-in (Xcode 14-15)
KERNEL_HDRS = $(SDK_DIR)
ifneq ($(wildcard $(SDK_DIR)/Headers),)
    KERNEL_HDRS := $(SDK_DIR)
else ifneq ($(wildcard $(SDK_DIR)/Kernel.framework/Headers),)
    KERNEL_HDRS := $(SDK_DIR)
else
    KERNEL_HDRS := $(SDK_PATH)
endif

# ─── Compiler Flags ──────────────────────────────────────────────
# -mkernel          : kernel ABI (no mxcsr, etc.)
# -arch x86_64      : Intel 64-bit เท่านั้น (ARM64 สำหรับ Apple Silicon)
# -nostdlib         : ไม่ลิงก์ libc (ใช้ kernel libs แทน)
# -fno-exceptions   : IOKit ห้ามใช้ C++ exceptions
# -fno-rtti         : IOKit ห้ามใช้ RTTI (ใช้ OSDynamicCast แทน)
# -DKERNEL          : 定义 kernel build
# -D__STRICT_BSD__  : strict POSIX/BSD namespaces
CXXFLAGS = -std=c++11 \
           -mkernel \
           -arch x86_64 \
           -nostdlib \
           -fno-builtin \
           -fno-exceptions \
           -fno-rtti \
           -DKERNEL \
           -D__STRICT_BSD__ \
           -I$(KERNEL_HDRS) \
           -I$(SDK_PATH)/System/Library/Frameworks/Kernel.framework/Headers \
           -I$(SDK_PATH)/usr/include

# ─── Linker Flags ────────────────────────────────────────────────
# -r              : relocatable object (kext = kernel module)
# -keep_private_extern : keep private symbols for kext
LDFLAGS = -r -keep_private_externs

# ─── Sources ─────────────────────────────────────────────────────
SRC = $(CLASS).cpp IntelFramebuffer.cpp
OBJ = $(CLASS).o IntelFramebuffer.o
HEADERS = $(CLASS).hpp IntelFramebuffer.hpp

.PHONY: all clean install load unload

all: $(TARGET).kext/Contents/MacOS/$(TARGET)

# ─── Compile each .cpp to .o ──────────────────────────────────
$(CLASS).o: $(CLASS).cpp $(HEADERS) Info.plist
	$(CC) $(CXXFLAGS) -c -o $@ $(CLASS).cpp

IntelFramebuffer.o: IntelFramebuffer.cpp $(HEADERS) Info.plist
	$(CC) $(CXXFLAGS) -c -o $@ IntelFramebuffer.cpp

# ─── Link .o files into kext binary ──────────────────────────
$(TARGET).kext/Contents/MacOS/$(TARGET): $(OBJ) Info.plist
	@mkdir -p $(TARGET).kext/Contents/MacOS
	cp Info.plist $(TARGET).kext/Contents/Info.plist
	$(CC) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJ)
	@echo "─── Build complete: $(TARGET).kext ───"

# ─── Utilities ───────────────────────────────────────────────────
clean:
	rm -rf $(TARGET).kext $(OBJ) IntelFramebuffer.o

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
