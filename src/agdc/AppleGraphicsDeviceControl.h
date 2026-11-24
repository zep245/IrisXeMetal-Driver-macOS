// Hand written from the macOS 15 kernel collection (research round 16, 12-/30-*.txt).
// AppleGraphicsDeviceControl : IOService, kext com.apple.AppleGraphicsDeviceControl.
// vtable (dump numbering, 2 header slots first, IOService ends at 267):
//   186 start  187 stop  194 terminate  232 message  240 newUserClient  (IOService slots, overridden)
//   268 vendor_doDeviceAttribute(..., IOExternalMethodArguments*)  pure - the vendor answers here
//   269 vendor_doDeviceAttribute(..., AGDCClientState_t*)           base: calls 268 with state->+0x38
//   270..277 _RESERVEDAppleGraphicsDeviceControl0..7
// The family's own code (user client, filtered_doDeviceAttribute, start) calls slot 269 at vptr+0x858,
// which calls slot 268 at vptr+0x850. Apple's IntelFBClientControl overrides only 268.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

struct AGDCClientState_t;

class AppleGraphicsDeviceControl : public IOService {
    OSDeclareAbstractStructors(AppleGraphicsDeviceControl)
public:
    virtual bool start(IOService* provider) override;                       // 186
    virtual void stop(IOService* provider) override;                        // 187
    virtual bool terminate(IOOptionBits options = 0) override;              // 194
    virtual IOReturn message(UInt32 type, IOService* provider, void* argument = 0) override;   // 232
    virtual IOReturn newUserClient(task_t owningTask, void* securityID, UInt32 type,
                                   OSDictionary* properties, IOUserClient** handler) override; // 240

    virtual IOReturn vendor_doDeviceAttribute(unsigned int attr, unsigned long* in, unsigned long inSize,
                                              unsigned long* out, unsigned long* outSize,
                                              IOExternalMethodArguments* args) = 0;            // 268
    virtual IOReturn vendor_doDeviceAttribute(unsigned int attr, unsigned long* in, unsigned long inSize,
                                              unsigned long* out, unsigned long* outSize,
                                              AGDCClientState_t* state);                       // 269

    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 0);   // 270
    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 1);
    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 2);
    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 3);
    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 4);
    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 5);
    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 6);
    OSMetaClassDeclareReservedUnused(AppleGraphicsDeviceControl, 7);   // 277
};
