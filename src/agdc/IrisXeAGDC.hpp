// IrisXeAGDC.kext (v0.52.0, design step 1): an AppleGraphicsDeviceControl for the Iris Xe screen.
// WindowServer's IOPresentment needs one AGDC per GPU to build its screen map; without it the log
// says "Unable to get AGDC information" and the desktop is painted by the CPU.
// v0.64.0: created at boot for the GPU-drawn desktop (IrisXeAGDCBoot); sudo sysctl debug.irisxe_agdc=1 still creates it,
// debug.irisxe_agdc_status reports what happened and every attribute macOS asked.
#pragma once
#include "AppleGraphicsDeviceControl.h"

// Plain nub under the IGPU PCI device, the AGDC's provider (Apple: AppleIntelFramebufferController).
class IrisXeFBController : public IOService {
    OSDeclareDefaultStructors(IrisXeFBController)
};

// v0.64.0: matched on IrisXeIntelFB at boot; starts the AGDC when IrisXeAccel asks (see IrisXeAGDC.cpp)
class IrisXeAGDCBoot : public IOService {
    OSDeclareDefaultStructors(IrisXeAGDCBoot)
public:
    virtual bool start(IOService* provider) override;
};

class IrisXeAGDC : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(IrisXeAGDC)
    typedef AppleGraphicsDeviceControl super;
public:
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int attr, unsigned long* in, unsigned long inSize,
                                              unsigned long* out, unsigned long* outSize,
                                              IOExternalMethodArguments* args) override;   // slot 268
    virtual void free() override;

    bool setup(IOService* framebuffer, IOService* gpu);
    void describeAsked(char* buf, size_t len);

    // room for Apple's own fields (they start right after IOService); checked against Apple's
    // real class size before any object is made
    enum { kAppleRoom = 0x300 };
private:
    UInt8 _apple[kAppleRoom];
    IOService* fFB;
    IOService* fGPU;
    IOLock* fLock;
    struct Asked { uint32_t attr; uint32_t count; IOReturn last; };
    Asked fAsked[32];
    uint32_t fAskedN;
    bool fLinkLogged;
    uint32_t fPipeCapsLogged;
    void noteAsked(uint32_t attr, IOReturn r);
    IOReturn answerLinkConfig(unsigned long* in, unsigned long inSize, uint8_t* o, unsigned long* outSize);
    IOReturn answerPipeCapsExt(unsigned long* in, unsigned long inSize, uint8_t* o, unsigned long* outSize);
};
