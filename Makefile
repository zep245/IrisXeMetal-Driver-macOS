# Builds two kexts with plain clang (no Xcode project needed). Run on macOS.
#   IrisXeCore.kext - GPU core, depends only on IOPCIFamily -> injected by OpenCore (early boot)
#   IrisXeFB.kext   - display (IOGraphicsFamily)           -> installed in /Library/Extensions
#   IrisXeAccel.kext - Metal accelerator (IOAcceleratorFamily2), milestone 1 -> /Library/Extensions
#   IrisXeAGDC.kext  - AppleGraphicsDeviceControl for WindowServer (v0.52.0), idle until sysctl -> /Library/Extensions
VERSION ?= 0.69.2
SDK     ?= $(shell xcrun --sdk macosx --show-sdk-path)
KERNHDR := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CLANGINC := $(shell xcrun clang -print-resource-dir)/include

BUILD := build
CORE  := $(BUILD)/IrisXeCore.kext
FB    := $(BUILD)/IrisXeFB.kext
ACCEL := $(BUILD)/IrisXeAccel.kext
AGDC  := $(BUILD)/IrisXeAGDC.kext
AGDCTEST := $(BUILD)/irisxe-agdctest

CC  := xcrun clang
CXX := xcrun clang++

COMMON := -arch x86_64 -target x86_64-apple-macos12.0 -mkernel -nostdinc \
          -isystem $(CLANGINC) -I$(KERNHDR) -isysroot $(SDK) \
          -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
          -fno-builtin -fno-common -fno-stack-protector -O2 -Wall \
          -DKEXT_VERSION='"$(VERSION)"'
CXXFLAGS := $(COMMON) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti
CFLAGS   := $(COMMON) -std=gnu11
LDFLAGS  := -arch x86_64 -target x86_64-apple-macos12.0 -mkernel -nostdlib \
            -isysroot $(SDK) -L$(SDK)/usr/lib -Xlinker -kext -lkmodc++ -lkmod

CORE_OBJS := $(BUILD)/IrisXeProbe.o $(BUILD)/IrisXeGT.o $(BUILD)/IrisXeGPU.o $(BUILD)/kmod_core.o
ACCEL_OBJS := $(BUILD)/accel/IrisXeAccel.o $(BUILD)/accel/IrisXeAccelClasses.o $(BUILD)/accel/IrisXeAccelMachines.o $(BUILD)/accel/IrisXeAccelTest.o $(BUILD)/accel/IrisXeDisplayPipe.o $(BUILD)/accel/IrisXeVideo.o $(BUILD)/accel/kmod_accel.o
AGDC_OBJS := $(BUILD)/agdc/IrisXeAGDC.o $(BUILD)/agdc/kmod_agdc.o
FB_OBJS   := $(BUILD)/IrisXeFB.o $(BUILD)/IrisXeCursor.o $(BUILD)/IrisXeBacklight.o $(BUILD)/kmod_fb.o

MTL     := $(BUILD)/IrisXeMTLDriver.bundle
MTLTEST := $(BUILD)/irisxe-metaltest
OBJCFLAGS := -arch x86_64 -mmacosx-version-min=13.0 -fno-objc-arc -O2 -Wall

KERNELS := $(BUILD)/irisxe-kernels.metallib
MTKDEMO := $(BUILD)/irisxe-mtkdemo
VTDECODE := $(BUILD)/irisxe-vtdecode

all: $(CORE) $(FB) $(ACCEL) $(AGDC) $(AGDCTEST) $(MTL) $(MTLTEST) $(KERNELS) $(MTKDEMO) $(VTDECODE)

# user-space Metal driver bundle (milestone 1 step 2) and its test tool.
# Milestone 3c: the bundle contains our shader compiler (src/compiler), which
# reads Metal's AIR with LLVM's bitcode reader. LLVM is linked in statically
# (LLVM_CONFIG = an x86_64 LLVM, e.g. Homebrew's on an Intel Mac); without it
# the bundle is built without the compiler.
LLVM_CONFIG ?= $(shell brew --prefix llvm 2>/dev/null)/bin/llvm-config
LLVM_OK     := $(shell $(LLVM_CONFIG) --version 2>/dev/null)
# v0.25.1: the compiler is its own dylib inside the bundle, loaded only when a
# pipeline is compiled - every process that uses Metal loads the bundle itself
# (WindowServer, Finder, Safari, ...), and they must not get LLVM.
MTL_COMPILER_DYLIB := $(MTL)/Contents/Frameworks/libIrisXeCompiler.dylib
ifneq ($(LLVM_OK),)
  LLVM_INC  := -I$(shell $(LLVM_CONFIG) --includedir)
  LLVM_LIBS := $(shell $(LLVM_CONFIG) --link-static --ldflags --libs bitreader core transformutils) $(shell tools/llvm-syslibs.sh $(LLVM_CONFIG))
  MTL_COMPILER := $(BUILD)/xe_compile.o
else
  LLVM_INC  :=
  LLVM_LIBS :=
  MTL_COMPILER :=
  OBJCFLAGS += -DIRISXE_NO_COMPILER
endif

$(BUILD)/xe_compile.o: src/compiler/xe_compile.cpp src/compiler/*.h | $(BUILD)
	xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -std=c++17 -O2 -Wall -fvisibility=hidden $(LLVM_INC) -c $< -o $@

$(MTL): src/metal/IrisXeMTLDriver.m src/metal/IrisXeMTLTexture.inc src/metal/IrisXeMTLRender.inc src/IrisXeDrawABI.h src/compiler/xe_compile.h src/metal/Info-MTL.plist src/metal/IrisXeBuiltins.metal $(MTL_COMPILER) | $(BUILD)
	mkdir -p $@/Contents/MacOS $@/Contents/Resources
	# v0.37.0: the driver's own shaders (generateMipmaps), compiled by our compiler at run time
	xcrun -sdk macosx metal -c src/metal/IrisXeBuiltins.metal -o $(BUILD)/irisxe-builtins.air
	xcrun -sdk macosx metallib $(BUILD)/irisxe-builtins.air -o $@/Contents/Resources/irisxe-builtins.metallib
	xcrun clang $(OBJCFLAGS) -bundle -framework Foundation -framework Metal -framework IOSurface src/metal/IrisXeMTLDriver.m -o $@/Contents/MacOS/IrisXeMTLDriver
ifneq ($(LLVM_OK),)
	mkdir -p $@/Contents/Frameworks
	xcrun clang++ -arch x86_64 -mmacosx-version-min=13.0 -dynamiclib $(MTL_COMPILER) $(LLVM_LIBS) \
	    -Wl,-dead_strip -Wl,-dead_strip_dylibs -Wl,-exported_symbol,_irisxe_compile_air -Wl,-exported_symbol,_irisxe_compile_air_render \
	    -install_name @loader_path/libIrisXeCompiler.dylib -o $(MTL_COMPILER_DYLIB)
	codesign -f -s - $(MTL_COMPILER_DYLIB)
endif
	sed 's/__VERSION__/$(VERSION)/g' src/metal/Info-MTL.plist > $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist
	codesign -f -s - $@

# test shaders for irisxe-metaltest [12]: Apple's offline Metal compiler -> AIR
$(KERNELS): tools/metaltest/kernels.metal tools/metaltest/big_straight.inc | $(BUILD)
	xcrun -sdk macosx metal -c $< -o $(BUILD)/irisxe-kernels.air
	xcrun -sdk macosx metallib $(BUILD)/irisxe-kernels.air -o $@
	# the same shaders as readable AIR text, for compiler debugging (in the release zip)
	xcrun -sdk macosx metal -S -emit-llvm $< -o $(BUILD)/irisxe-kernels.ll

# v0.39.0: a small MetalKit app (MTKView, MTLCreateSystemDefaultDevice) and its shaders
$(MTKDEMO): tools/mtkdemo/irisxe-mtkdemo.m tools/mtkdemo/demo.metal | $(BUILD)
	xcrun -sdk macosx metal -c tools/mtkdemo/demo.metal -o $(BUILD)/irisxe-mtkdemo.air
	xcrun -sdk macosx metallib $(BUILD)/irisxe-mtkdemo.air -o $(BUILD)/irisxe-mtkdemo.metallib
	xcrun clang $(OBJCFLAGS) -framework Foundation -framework AppKit -framework Metal -framework MetalKit -framework QuartzCore -framework IOSurface -framework IOKit tools/mtkdemo/irisxe-mtkdemo.m -o $@
	codesign -f -s - $@

$(MTLTEST): tools/metaltest/irisxe-metaltest.m tools/metaltest/xe_kernels.h tools/metaltest/big_straight.inc src/IrisXeDrawTest.h src/compiler/xe_compile.h | $(BUILD)
	xcrun clang $(OBJCFLAGS) -framework Foundation -framework Metal -framework IOKit -framework AppKit -framework QuartzCore -framework CoreVideo -framework CoreGraphics -framework IOSurface $< -o $@
	codesign -f -s - $@

$(BUILD)/%.o: src/%.cpp src/*.hpp src/*.h src/*.inc src/accel/*.hpp src/accel/apple/*.h src/agdc/*.h src/agdc/*.hpp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc/accel -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(CORE): $(CORE_OBJS) Info-Core.plist
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $(CORE_OBJS) -o $@/Contents/MacOS/IrisXeCore
	sed 's/__VERSION__/$(VERSION)/g' Info-Core.plist > $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist

$(FB): $(FB_OBJS) Info-FB.plist
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $(FB_OBJS) -o $@/Contents/MacOS/IrisXeFB
	sed 's/__VERSION__/$(VERSION)/g' Info-FB.plist > $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist

$(ACCEL): $(ACCEL_OBJS) Info-Accel.plist
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $(ACCEL_OBJS) -o $@/Contents/MacOS/IrisXeAccel
	@test -n "$(IOSURFACE_VER)" || { echo "ERROR: IOSurface.kext not found on this Mac"; exit 1; }
	sed -e 's/__VERSION__/$(VERSION)/g' -e 's/__IOSURFACE_VER__/$(IOSURFACE_VER)/g' Info-Accel.plist > $@/Contents/Info.plist
	@echo "IrisXeAccel links com.apple.iokit.IOSurface $(IOSURFACE_VER)"
	plutil -lint $@/Contents/Info.plist

# v0.56.1: IrisXeAccel links against the kernel IOSurface (IrisXeDisplayPipe reads the screen images); version read
# from the build Mac like AGDC's
IOSURFACE_PLIST := /System/Library/Extensions/IOSurface.kext/Contents/Info.plist
IOSURFACE_VER   := $(shell /usr/libexec/PlistBuddy -c "Print :OSBundleCompatibleVersion" "$(IOSURFACE_PLIST)" 2>/dev/null || /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$(IOSURFACE_PLIST)" 2>/dev/null)

# v0.52.0: the AGDC kext links against Apple's AppleGraphicsDeviceControl. Its library version is read
# from the build Mac (macOS 15): OSBundleCompatibleVersion is the lowest version we may ask for.
AGDC_PLIST := $(shell find /System/Library/Extensions -path "*AppleGraphicsDeviceControl.kext/Contents/Info.plist" 2>/dev/null | head -1)
AGDC_VER   := $(shell /usr/libexec/PlistBuddy -c "Print :OSBundleCompatibleVersion" "$(AGDC_PLIST)" 2>/dev/null || /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$(AGDC_PLIST)" 2>/dev/null)

$(AGDC): $(AGDC_OBJS) Info-AGDC.plist
	@test -n "$(AGDC_VER)" || { echo "ERROR: AppleGraphicsDeviceControl.kext not found on this Mac"; exit 1; }
	mkdir -p $@/Contents/MacOS
	$(CXX) $(LDFLAGS) $(AGDC_OBJS) -o $@/Contents/MacOS/IrisXeAGDC
	sed -e 's/__VERSION__/$(VERSION)/g' -e 's/__AGDC_VER__/$(AGDC_VER)/g' Info-AGDC.plist > $@/Contents/Info.plist
	plutil -lint $@/Contents/Info.plist
	@echo "IrisXeAGDC links com.apple.AppleGraphicsDeviceControl $(AGDC_VER) ($(AGDC_PLIST))"

# v0.52.0: asks every AGDC the two questions WindowServer asks (selectors 1 and 0x980)
$(AGDCTEST): tools/agdctest/irisxe-agdctest.c | $(BUILD)
	xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -O2 -Wall -framework IOKit -framework CoreFoundation $< -o $@
	codesign -f -s - $@

# v0.68.0: hardware video decoding test (makes an H.264 clip with the software encoder, decodes it in hardware)
$(VTDECODE): tools/vtdecode/irisxe-vtdecode.m | $(BUILD)
	xcrun clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -O2 -Wall -framework Foundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo $< -o $@
	codesign -f -s - $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
