// IrisXeVideo.hpp - v0.68.0: hardware video decoding, step V2b: a RECORD-ONLY video context.
//
// Apple's Ice Lake VA driver (AppleIntelICLGraphicsVADriver, loaded by AppleGVA when the accelerator announces
// IODVDBundleName) opens a "Media" video context (IntelAccelerator::newContext(0x101), research round 36) and sends
// it command streams: a sideband buffer of tokens (u16 id, u16 size in dwords, u32 offset into the command buffer).
// Tokens with id 0x00xx..0x05xx are the family's (IOAccelContext2: Start, End, BindDataBuffer, SetBlockFence,
// ResourcePurgeable, ResourceRelease); 0x80xx..0x82xx are Apple's Intel video tokens (SetFence, BatchBufferStart,
// CodecHAL) and the codec tokens follow (MPEG2 VLD, AVC PAK, AVCDecode, MPEG2 IT, JPEG, VDENC, CODECDecode).
//
// This context accepts all of them and only RECORDS what it sees (token ids, sizes, their first dwords and the
// command-buffer dwords they point at) - nothing runs on the GPU yet. The record (IrisXe-VideoRecord on the
// accelerator) is the ground truth for the next step (filling in the addresses, running on VCS0).
// Only on a one-boot test: sudo nvram irisxe-videotest=1 (read and erased by IrisXeAccel at start).
#pragma once
#include "apple/IOAccelVideoContext2.h"
#include "apple/IOAccelDevice2.h"

class IrisXeAccelerator;

class IrisXeVideoContext : public IOAccelVideoContext2 {
    OSDeclareDefaultStructors(IrisXeVideoContext)
public:
    virtual IOAccelDataBufferLimits getDataBufferLimits() override;                    // slot 320
    virtual IRISXE_SLOT_RET processSidebandToken(IOAccelCommandStreamInfo&) override;   // slot 324
    virtual IRISXE_SLOT_RET discardSidebandToken(IOAccelCommandStreamInfo&) override;   // slot 325
    virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index) override;
    // v0.68.6: watched only (the family's code runs; the record gets what was called and what it returned)
    virtual IRISXE_SLOT_RET processSidebandBuffer(IOAccelCommandDescriptor*, bool) override;   // slot 323
    virtual IRISXE_SLOT_RET processDataBuffers(unsigned int) override;                        // slot 327
    virtual IRISXE_SLOT_RET beginCommandStream(IOAccelCommandStreamInfo&) override;           // slot 328
    virtual IRISXE_SLOT_RET endCommandStream(IOAccelCommandStreamInfo&) override;             // slot 329
    virtual IRISXE_SLOT_RET populateContextConfig(IOAccelContextConfig*) override;            // slot 339 (v0.68.7)
    // the Intel video context's own methods (IGAccelVideoContext::attach's method_descs, selectors 0x100..0x102)
    IOReturn getAcceleratorId(void *out, IOByteCount *outSize);
    IOReturn perfCapability(void *in, IOByteCount inSize);
    IOReturn setPriority(void *in, IOByteCount inSize);
private:
    // Apple's part of the object (IOAccelContext2 + IOAccelVideoContext2): below 0x10e8 bytes on macOS 15 (Apple's
    // Intel subclass starts its own fields there); the padding only makes our allocation big enough.
    UInt8 _apple[4664];
public:
    UInt32 xeType = 0;                                 // v0.68.1: 0x100 Main, 0x101 Media, 0x102 VEBox (for the record)
};

// v0.68.2: the device connection (IOAccelDevice2). Apple's Intel device (IGAccelDevice, research rounds 37-39) adds
// selectors 10..24 to the family's 0..9; the VA driver asks selector 11 (get_hw_caps) first and stops with "Failed
// to get hw caps from kernel" when it gets no answer. Ours answers 10 (workaround table), 11 (hardware caps), 23 (vram
// size) and 24 (hardware info) with Ice Lake GT2 style data and records every Intel selector asked. Test boot only.
class IrisXeDevice : public IOAccelDevice2 {
    OSDeclareDefaultStructors(IrisXeDevice)
public:
    virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index) override;
private:
    UInt8 _apple[1024];        // Apple's part ends at 0x188 (IGAccelDevice's own fields start there); padding only
};
IRISXE_SLOT_RET IrisXeVideoNewDevice();   // IrisXeAccelerator::newDevice on the test boot (0 = the family's device)

// shared by the context and the accelerator (one record for the whole boot)
void IrisXeVideoRecordNote(const char *what, UInt32 type);
void IrisXeVideoRecordPublish(IrisXeAccelerator *accel);
bool IrisXeVideoTestOn();
bool IrisXeVideoTestPending();           // v0.68.5: peek (no erase), for populateAccelConfig
void IrisXeVideoSetROPrivate(UInt32 n);
UInt32 IrisXeROPrivateSize();
void IrisXeVideoTestStart(IrisXeAccelerator *accel);   // at accelerator start: reads nvram irisxe-videotest
