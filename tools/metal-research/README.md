# metal-research

GitHub Actions job (`irisxe-metal-research.yml`) that runs on Intel macOS
runners and collects how Metal finds and talks to an Intel GPU driver:

- kext / bundle Info.plists (how the accelerator announces its Metal plugin)
- Objective-C classes, ivars, methods and protocols of Metal.framework,
  IOGPU.framework and Apple's Intel ICL/KBL Metal driver bundles
  (runtime introspection, `objc_dump.m`)
- exported / imported symbols (`dyld_info`)
- strings and load commands of the binaries inside the shared cache
  (extracted on the runner with Xcode's dsc_extractor; the binaries
  themselves are NOT uploaded)
- AIR output of a tiny Metal shader (`test.metal`)

Purpose: interoperability - writing our own Metal driver bundle for Iris Xe.

## Round 39 (hardware video decoding, part 7)
`run.sh`: every IntelAccelerator function, filtered to those that write the hardware caps / WA / SKU / GT system info range (+0xf36..+0x11c0), and the hardware-info functions in full. Python + capstone only. Text output only. Round 38 is kept as `run38.sh`.

## Round 38 (hardware video decoding, part 6)
`run.sh`: the method tables decoded as data (`kcdis.py` now also dumps `*Descs` / `*Methods`) and who fills Apple's hardware caps / workaround / SKU / GT system info data (initHardwareCaps, Init*WaTable, IntelAccelerator::start). Python + capstone only: runs on a Mac with just the Command Line Tools. Text output only. Round 37 is kept as `run37.sh`.

## Round 37 (hardware video decoding, part 5)
`run.sh`: Apple's Intel device user client (IGAccelDevice): its method table and get_hw_caps / get_hw_info / get_wa_table (the Intel media driver's platform, SKU and workaround data), where IntelAccelerator fills them, and the family's IOAccelDevice2. Text output only. Round 36 is kept as `run36.sh`.

## Round 36 (hardware video decoding, part 4)
`run.sh`: IntelAccelerator::newContext(type) (who makes the video contexts), every IGAccelVideoContext(-Media) function and the family's IOAccelContext2 command-stream machinery they rely on, and the vtables (which slots Apple overrides). Text output only. Round 35 is kept as `run35.sh`.

## Round 35 (hardware video decoding, part 3)
`run.sh`: the family's IOAccelVideoContext2 (base class for our video context), the ICL kext's method / token tables decoded as data (`kcdis.py` gained data-table dumps), its token handlers, and IOAccelerator.framework's video context functions. Text output only. Round 34 is kept as `run34.sh`.

## Round 34 (hardware video decoding, part 2)
`run.sh`: Apple's Ice Lake video context in the kext (method table, command-stream tokens, address patching, media ring), how its user clients are made, the ICL VA driver's imports / strings / kernel calls, and how AppleGVA loads it. Text output only. Round 33 is kept as `run33.sh`.

## Round 33 (hardware video decoding)
`run.sh` + `vtprobe.m`: how VideoToolbox reaches an Intel GPU's media engine on an Intel Mac (AppleGVA, the per-GPU VA driver bundles, the Intel kexts' video user clients) and what VideoToolbox reports. Text output only. Round 32 is kept as `run32.sh`.

## Round 10 (milestone 3b)
`run.sh` + `pipeline_probe.m`: how Metal compiles a compute function for a GPU driver (function key -> compiler service -> compiler output -> variant -> pipeline), where the vendor compiler lives, and how the ICL driver encodes a dispatch. Text output only. Round 9 is kept as `run9.sh`.

## Round 16 (display chain, complete map)
`run.sh`: everything still unknown between WindowServer and the screen: AGDC (AppleGraphicsDeviceControl and Apple's Intel AGDC nub), IOPresentment's device map, and the flip / transaction path of Apple's Intel display driver. Text output only. Round 15 is kept as `run15.sh`.

## Round 15 (display pipe, user space 3)
`run.sh`: the CoreDisplay functions that fill the display pipe table (round 14 stopped after the first), and IOPresentment's functions that find a display / framebuffer / accelerator. Text output only. Round 14 is kept as `run14.sh`.

## Round 14 (display pipe, user space 2)
`run.sh`: who fills CoreDisplay's display pipe table (gDisplayPipes) that FindDisplayPipeForService searches by framebuffer registry ID. `udis.py` gained `y:` (symbols by name) and `r:` (functions using a global). Text output only. Round 13 is kept as `run13.sh`.

## Round 13 (display pipe, user space)
`run.sh`: which checks in CoreDisplay / SkyLight lead to "Unable to find display pipe for IOFB service" and to the CPU compositor fallback, now that the kernel has a display pipe for IrisXeFB. `udis.py` gained `x:<image>:<text>` (functions that load a string). Text output only. Round 12 is kept as `run12.sh`.

## Round 12 (display pipe)
`run.sh`: why WindowServer reports "Unable to find display pipe" and what a framebuffer must provide before Apple's display machine builds a pipe (IOAccelDisplayMachine / IOAccelDisplayPipe, Apple's Ice Lake display machine and its framebuffer calls). Text output only. Round 11 is kept as `run11.sh`.

## Round 11 (milestone 3c)
`run.sh` + `air_probe.m` + `kernels.metal`: does a stock (Homebrew) LLVM read the AIR bitcode of our own test kernels, both from the offline compiler and from MTLFunction at run time? Round 10 is kept as `run10.sh`.

## Round 17 (v0.52.1): what WindowServer needs after the AGDC

With IrisXeAGDC (v0.52.0) WindowServer finds our AGDC; with a display pipe that declares transactions it asks
attribute 0x921 and then crashes in IOPresentment every 10 s. `run.sh` now reads Apple's Intel answers for 0x921,
0x2001, 0x941, 0x3 (with switch jump tables, new in kcdis.py), the AGDC family filter, and every IOPresentment
function that uses those numbers or the pipe/interface errors (new `i:IMG:VALUE` constant search in udis.py).
Round 16 is kept as `run16.sh`.

## Round 18 (v0.52.2): 0x921 = link config, and the CoreDisplay assert

Round 17 found IOPresentment asks 0x921 (0xB0 bytes) for its logical device capabilities; Apple's Intel driver answers
0x921 with GetLinkConfig, 0x941 with GetPortCapability and 0x2001 through processCmd. WindowServer then aborts on an
assert in CoreDisplay_CreateDisplayForCGXDisplayDevice. `run.sh` reads those three Intel functions, that CoreDisplay
function (udis now reads up to 12000 bytes per function) and IOPresentmentCapabilities. Round 17 is `run17.sh`.

## Round 19 (v0.54.1): why WindowServer still composites with the CPU

With IrisXeAGDC 0.54 every IOPresentment/CoreDisplay error is gone, yet SkyLight still composites the screen with
CompositorSW into the old framebuffer. `run.sh` reads how SkyLight and CoreDisplay choose the compositor and the display
destination. Round 18 is `run18.sh`.

## Round 20 (v0.54.2): the per-display Metal compositor

Round 19: SkyLight picks the compositor per display GPU (WSGetCompositorForAccelerator -> WSGetCompositorMetal ->
CompositorMetal::new_compositor, keyed by a 32-bit accelerator id); without one the display gets CompositorSW.
`run.sh` reads those functions and where the display's accelerator id comes from. Round 19 is `run19.sh`.

## Round 21 (v0.54.6): where a display's compositor is chosen

v0.54.6 showed new_compositor(83235) in WindowServer finds our device (same acceleratorPort), so a CompositorMetal for
our GPU exists; the screen still gets CompositorSW. `run.sh` lists every caller of WSGetCompositorForAccelerator and
WSGetCompositorMetal (new udis.py `k:` mode), every user of the CompositorSW singleton, and CDDisplay's compositor /
accelerator / framebuffer functions. Round 20 is `run20.sh`.

## Round 22 (v0.54.6): old framebuffer or display surface

Round 21: CDDisplay::compositor() = WSGetCompositorForAccelerator(CGXGLDisplayDeviceAccelerator), which is our GPU, but
CDDisplay::render_update draws into the old framebuffer (CGXAccessDisplayDeviceSurface, CPU only) when the update's
surface object has byte +0x40 = 1, and into a CoreDisplay DisplaySurface (Metal texture / IOSurface) otherwise. All five
WindowServer samples took the old-framebuffer path. `run.sh` reads where that surface is chosen in SkyLight and when
CoreDisplay makes a DisplaySurface. Round 21 is `run21.sh`.

## Round 23 (v0.54.6): CoreDisplay's next display surface

Round 22: SkyLight's CDSurface::prepare_for_update takes CoreDisplay_Display_GetNextSurface; a surface with an IOSurface
means GPU drawing, one without means the old framebuffer and CompositorSW (begin_update). With the display pipe on,
WindowServer called CoreDisplay::Display::GetNextSurface and still got the old framebuffer. `run.sh` reads
GetNextSurface and who makes / uses a DisplaySurfacePool. Round 22 is `run22.sh`.

## Round 24 (v0.54.6): CoreDisplay's IOPresentment switch

Round 23: Display::GetNextSurface uses a DisplaySurfacePool (GPU images) only when UseIOPresentment() and
displaySurfaceUsesIOSurface are both true; else CGXAcquireDisplayDeviceSurface (old framebuffer, CPU). UseIOPresentment()
= useIOP and every vendor of GetSystemGPUVendors() enabled (IG 2, AMD 4, NVIDIA 8, APV 0x10). `run.sh` reads where these
flags are set and how GPU vendors are classified. Round 23 is `run23.sh`.

## Round 25 (v0.54.6): GPU vendor by framebuffer class name

Round 24: GetSystemGPUVendors() ORs GetGPUVendorForFramebufferService() over every IOFramebuffer; that function looks
for four case-insensitive substrings in the framebuffer's class name (IG 2, AMD 4, APV 0x10, NVIDIA 8; no match = 1,
which is then dropped). With only IrisXeFB the set is 0, so UseIOPresentment() switches itself off. `run.sh` decodes the
four substrings (new `cfs:` mode) and the start values of the useIOP flags (new `v:` mode). Round 24 is `run24.sh`.

## Round 26 (v0.55.1): display pipe transactions

v0.55.0 test: with the framebuffer named IrisXeIntelFB and the display pipe made, CoreDisplay gives the screen
DisplaySurfaces (2560x1600 BGRA) and sends them to the pipe, but logs "Fallback GPU Display Pipe transaction submission
failed" and the picture freezes. `run.sh` reads the transaction path in IOAcceleratorFamily2 (user client ->
IOAccelDisplayPipe -> IOAccelDisplayPipeTransaction2), Apple's Intel IGAccelDisplayPipe for comparison, CoreDisplay's
RunGPUDisplayPipe and IOPresentment's submit. Round 25 is `run25.sh`.

## Round 27 (v0.55.1): what our own display pipe can call

Round 26: Apple's base IOAccelDisplayPipe cannot show a transaction (submitTransaction returns 0xe0014042 when the GPU
driver adds nothing; performTransaction returns kIOReturnUnsupported). Apple's Intel driver overrides validate / submit /
perform / isTransactionComplete / begin / signalTransactionComplete. `run.sh` lists the kernel IOSurface methods,
IOAccelResource2 accessors and Apple's Intel performTransaction / generateFlip, for IrisXeDisplayPipe. Round 26 is
`run26.sh`.

## Round 28 (v0.55.1): how a transaction becomes live, and Apple's simplest display pipe

Round 27: the kernel IOSurface methods (getMemoryDescriptor, getWidth/Height/BytesPerRow: 64-bit, getPixelFormat: 32-bit)
are exported. `run.sh` reads IOAccelDisplayPipe::event_interrupt_gated and the current-plane accessors, and every
method of AppleParavirtDisplayPipe (the virtual-machine GPU, the simplest display pipe Apple ships). Round 27 is
`run27.sh`.

## Round 29 (v0.56.1): why the transaction submission fails

v0.56.0 test: CoreDisplay still logs "Fallback GPU Display Pipe transaction submission failed" (DisplayPipe::Commit ->
IOPTransaction::Submit() false) and the kernel saw no display-pipe call. `run.sh` reads IOPTransaction::Submit,
IOPresentment's display-pipe transaction functions and IOAccelerator.framework's IOAccelDisplayPipe* calls. v0.56.1
counts every step our display pipe reaches (IrisXe-DisplayPipe). Round 28 is `run28.sh`.

Round 29 result: CoreDisplay::IOPTransaction::Submit reports its failures ("Failed to submit transaction: %s" after
IOPresentmentEnd, "Failed to set transaction plane state: %s", ...) at Info level, which macOS does not keep; the
v0.56.1 test showed 345 IOPresentment transactions begun in the kernel but none ended (transaction_end always reaches
submitTransaction). The v0.56.2 test script keeps CoreDisplay's Info/Debug messages during the test.

## Round 30 (v0.56.2): who answers "not ready"

v0.56.2 test: every transaction ends with IOReturn 0xe00002d8 (kIOReturnNotReady) from WSDisplayPipeTransactionEnd,
and the kernel never reaches IOAccelDisplayPipe::transaction_end. `run.sh` reads
IOAccelDisplayPipeUserClient2::transactionEnd and the checks it makes. Round 29 is `run29.sh`.

## Round 31 (v0.56.4): why the display pipe's frames get no GPU address

v0.56.3 showed WindowServer's frames, but each transaction was answered 0xe00002be: `IOAccelResource2::prepare()`
could not get a GPU address for the frame's memory ("Needed 0 bytes ... vram=1"). This round reads
`IOAccelTask::freeToAllocGPUAddress`, `IOAccelResource2::map/prepare`, `IOAccelMemoryMap::init`, the video-memory
classes and Apple's paravirtual accelerator, to find where the size and the vram flag come from.
Result: `freeToAllocGPUAddress` logs `IOAccelMemoryMap::getLength()` = the memory's +0x40 = the size given to
`IOAccelVidMemory::init`; vram=1 = the memory is video memory (flag 4). `IOAccelResource2::map()` maps the resource's
+0x88 memory. Apple's paravirtual `calculateIOSurfaceDeviceCacheVRAMBytes` returns the IOSurface's alloc size and
bytes per row (IOSurface at resource +0xe0 -> +0x10); ours returned nothing -> size 0. Fixed in v0.56.5.

## Round 32 (v0.62.2): asynchronous command buffers

IrisXeAccel runs a command buffer's GPU work inside `processCommandBuffer` and returns only when the GPU has finished,
so WindowServer's main thread waits ~5 ms per command buffer and cannot prepare the next frame meanwhile. Our event
machine's `writeStamp` is empty, so Apple's queue completes the command buffer itself after we return. `run.sh` reads
how `IOAccelCommandQueue` submits and retires command buffers, how `IOAccelEventMachineFast2`'s stamps decide that one
is complete, and how Apple's paravirtual GPU (asynchronous) writes and signals its stamps. Round 31 is `run31.sh`.
