// IrisXeBacklight - gives the built-in display a "brightness" parameter
// (System Settings slider + brightness keys) and drives the panel PWM.
#ifndef IRISXE_BACKLIGHT_HPP
#define IRISXE_BACKLIGHT_HPP

#include <IOKit/graphics/IODisplay.h>

class IrisXeFB;

class IrisXeBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(IrisXeBacklight)
    typedef IODisplayParameterHandler super;

    IrisXeFB  *fb      = nullptr;
    IODisplay *display = nullptr;
    UInt32     level   = 0;
    bool       dimmed  = false;
    UInt32     savedLevel = 0;
    UInt32     probeMode  = 0;    // v0.68.19: 0 no probe keys, 1 linear-brightness-probe, 2 both (mirrored)
    static UInt32 readProbeMode();    // v0.68.18: the value in NVRAM (save only when it changed)

public:
    static constexpr UInt32 kMaxLevel = 1024;

    static IrisXeBacklight *withFramebuffer(IrisXeFB *framebuffer);

    // Remember brightness across restarts (NVRAM variable "irisxe-brightness")
    void saveLevel();
    bool loadLevel(UInt32 *out);

    virtual bool setDisplay(IODisplay *display) override;
    virtual bool doIntegerSet(OSDictionary *params, const OSSymbol *paramName,
                              UInt32 value) override;
    virtual bool doDataSet(const OSSymbol *paramName, OSData *value) override;
    virtual bool doUpdate() override;
};

#endif
