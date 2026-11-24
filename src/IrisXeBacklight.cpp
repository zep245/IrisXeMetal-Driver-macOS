// IrisXeBacklight - brightness parameter handler for the built-in panel.
//
// IODisplay searches the framebuffer's registry subtree for
// IODisplayParameterHandler objects and calls setDisplay() on each. We add a
// "brightness" parameter (0..1024) to the display, and when macOS changes it
// (slider, keys, dimming) we program the panel PWM duty cycle.

#include "IrisXeBacklight.hpp"
#include "IrisXeFB.hpp"
#include <libkern/libkern.h>
#include <string.h>
#include <IOKit/IODeviceTreeSupport.h>

#define LOG(fmt, ...) IOLog("IrisXeBacklight: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(IrisXeBacklight, IODisplayParameterHandler)

IrisXeBacklight *IrisXeBacklight::withFramebuffer(IrisXeFB *framebuffer) {
    IrisXeBacklight *h = OSTypeAlloc(IrisXeBacklight);
    if (!h) return nullptr;
    if (!h->init()) {
        h->release();
        return nullptr;
    }
    h->fb = framebuffer;
    h->level = framebuffer->backlightLevel(kMaxLevel);

    UInt32 saved;
    if (h->loadLevel(&saved)) {
        h->level = saved;
        framebuffer->setBacklightLevel(saved, kMaxLevel);
        framebuffer->setProperty("IrisXe-BrightnessRestored", saved, 32);
    }
    return h;
}

#define kNVRAMKey "irisxe-brightness"

UInt32 IrisXeBacklight::readProbeMode() {
    if (PE_parse_boot_argn("-irisxeprobekeys", nullptr, 0)) return 2;
    UInt32 m = 0;   // v0.68.21: none by default again. v0.68.20's default 2 worked when the panel came up dimmed, but with
                    // it at full CoreDisplay sent the slider to "brightness-probe" and pinned linear-brightness at 1024
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options) return m;
    OSObject *obj = options->copyProperty("irisxe-probekeys");
    OSData *d = OSDynamicCast(OSData, obj);
    OSString *str = OSDynamicCast(OSString, obj);
    const char *c = d && d->getLength() ? (const char *)d->getBytesNoCopy() : str ? str->getCStringNoCopy() : nullptr;
    if (c && c[0] >= '0' && c[0] <= '2') m = (UInt32)(c[0] - '0');   // "sudo nvram irisxe-probekeys=2" stores text
    else if (d && d->getLength() >= 1 && c[0] <= 2) m = (UInt32)c[0];
    OSSafeReleaseNULL(obj);
    options->release();
    return m;
}

bool IrisXeBacklight::loadLevel(UInt32 *out) {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options) return false;
    bool ok = false;
    OSObject *obj = options->copyProperty(kNVRAMKey);
    OSData *data = OSDynamicCast(OSData, obj);
    if (data && data->getLength() == sizeof(UInt32)) {
        UInt32 v;
        memcpy(&v, data->getBytesNoCopy(), sizeof(v));
        if (v > kMaxLevel) v = kMaxLevel;
        if (v < 16) v = 16;                 // never restore to (almost) black
        *out = v;
        savedLevel = v;
        ok = true;
    }
    OSSafeReleaseNULL(obj);
    options->release();
    return ok;
}

void IrisXeBacklight::saveLevel() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options) return;
    if (level == savedLevel || level < 16) { options->release(); return; }   // v0.68.18: unchanged / (almost) black
    UInt32 v = level;
    savedLevel = level;
    OSData *data = OSData::withBytes(&v, sizeof(v));
    if (data) {
        options->setProperty(kNVRAMKey, data);
        data->release();
        LOG("saved brightness %u to NVRAM", v);
    }
    options->release();
}

bool IrisXeBacklight::setDisplay(IODisplay *newDisplay) {
    display = newDisplay;            // not retained (same as Apple's handlers)
    if (!display || !fb || !fb->backlightAvailable()) return false;

    OSObject *prop = display->copyProperty(gIODisplayParametersKey);
    OSDictionary *current = OSDynamicCast(OSDictionary, prop);
    OSDictionary *params = current ? OSDictionary::withDictionary(current)
                                   : OSDictionary::withCapacity(2);
    OSSafeReleaseNULL(prop);
    if (!params) return false;

    // "brightness" is the classic key; modern macOS (CoreDisplay) drives the
    // built-in panel through "linear-brightness". Offer both.
    IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, kMaxLevel);
    IODisplay::setParameter(params, gIODisplayBrightnessKey, (SInt32)level);
    IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, kMaxLevel);
    IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, (SInt32)level);
    // CoreDisplay reads the user brightness through this "probe" key at login
    // and after wake (it logged "Set linear-brightness-probe ... failed").
    // After a full system wake it uses "brightness-probe" instead.
    // v0.10.5/6 published both by default; CoreDisplay then sent the slider to "brightness-probe" (not the real
    // keys) and moving the slider stopped changing the panel, so they were turned off. Brightness tests (v0.68.17/18):
    // at login CoreDisplay SETS "linear-brightness-probe" to the panel level and then READS "brightness-probe" back
    // as the user brightness. v0.68.17: no probe keys -> the set fails -> the slider shows full. v0.68.18: only the
    // linear one -> the read fails -> CoreBrightness's user brightness is 0 and it pinned the panel (slider dead).
    // v0.68.19: mode from NVRAM "irisxe-probekeys" (0 = none, as v0.68.17; 1 = linear only, as v0.68.18;
    // 2 = both, and a write to either probe is copied to the other so the read returns the level just set).
    // -irisxeprobekeys = mode 2. v0.68.20 made mode 2 the default; v0.68.21 back to 0 (see readProbeMode).
    // Probe writes never drive the panel.
    static const char *probes[] = { "linear-brightness-probe", "brightness-probe" };
    probeMode = readProbeMode();
    unsigned nProbes = probeMode > 2 ? 2 : probeMode;
    if (fb) fb->setProperty("IrisXe-ProbeKeys", (unsigned long long)probeMode, 32);
    for (unsigned i = 0; i < nProbes; i++) {
        const OSSymbol *probe = OSSymbol::withCString(probes[i]);
        if (!probe) continue;
        IODisplay::addParameter(params, probe, 0, kMaxLevel);
        IODisplay::setParameter(params, probe, (SInt32)level);
        probe->release();
    }
    // Display power ("dsyp"): with this key AppleBacklightDisplay dims and
    // un-dims through us (2 on, 1 dim, 0 off). Without it, it dims by writing
    // brightness=1 and then "restores" min(max, current) = 1 again, so the
    // panel stayed dark after the idle dim (bug found in v0.11.1).
    IODisplay::addParameter(params, gIODisplayPowerStateKey,
                            kIODisplayPowerStateOff, kIODisplayPowerStateOn);
    IODisplay::setParameter(params, gIODisplayPowerStateKey, kIODisplayPowerStateOn);
    display->setProperty(gIODisplayParametersKey, params);
    params->release();

    LOG("brightness parameter added (level %u/%u)", level, kMaxLevel);
    return true;
}

bool IrisXeBacklight::doIntegerSet(OSDictionary *params, const OSSymbol *paramName,
                                   UInt32 value) {
    // CoreDisplay may drive the panel through the classic key, the linear key,
    // or (after a full wake) the "-probe" variants. Treat them all the same.
    bool isBrightness = (paramName == gIODisplayBrightnessKey);
    bool isLinear     = (paramName == gIODisplayLinearBrightnessKey);
    bool isProbe      = paramName && (paramName->isEqualTo("linear-brightness-probe") ||
                                      paramName->isEqualTo("brightness-probe"));
    if (fb && paramName) fb->noteBrightness(paramName->getCStringNoCopy(), value);

    if (fb && paramName == gIODisplayPowerStateKey) {
        if (params) IODisplay::setParameter(params, paramName, (SInt32)value);
        if (value == kIODisplayPowerStateMinUsable) {
            // idle dim: about a third of the user's level (user level is kept)
            dimmed = true;
            UInt32 d = level / 3;
            if (d < 16) d = (level < 16) ? level : 16;
            fb->setBacklightLevel(d, kMaxLevel);
        } else if (value == kIODisplayPowerStateOn) {
            dimmed = false;
            fb->setBacklightLevel(level, kMaxLevel);       // back to the user level
        }
        // Off: the framebuffer's own power handling turns the backlight off
        return true;
    }

    if (!fb || (!isBrightness && !isLinear && !isProbe)) return false;

    if (value > kMaxLevel) value = kMaxLevel;

    // "-probe" keys are CoreDisplay reading/testing the panel range, not the
    // user's brightness (v0.10.6 let them drive the PWM, which pinned the
    // backlight near 0). Accept and store them only.
    if (isProbe) {
        if (params) IODisplay::setParameter(params, paramName, (SInt32)value);
        if (params && probeMode == 2) {            // v0.68.19: keep both probes equal (our two scales are the same)
            const OSSymbol *other = OSSymbol::withCString(paramName->isEqualTo("brightness-probe")
                                                          ? "linear-brightness-probe" : "brightness-probe");
            if (other) { IODisplay::setParameter(params, other, (SInt32)value); other->release(); }
        }
        char pn[64];
        snprintf(pn, sizeof(pn), "%s=%u", paramName->getCStringNoCopy(), value);
        fb->setProperty("IrisXe-LastProbeSet", pn);
        return true;
    }

    level = value;
    dimmed = false;
    fb->setBacklightLevel(level, kMaxLevel);

    // keep both keys in sync so the slider and keys agree
    if (params) {
        IODisplay::setParameter(params, gIODisplayBrightnessKey, (SInt32)level);
        IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, (SInt32)level);
        
    }

    char note[64];
    snprintf(note, sizeof(note), "%s=%u", paramName->getCStringNoCopy(), value);
    fb->setProperty("IrisXe-LastBrightnessSet", note);
    return true;
}

bool IrisXeBacklight::doDataSet(const OSSymbol *paramName, OSData *) {
    if (fb && paramName) fb->noteBrightness(paramName->getCStringNoCopy(), 0xFFFFFFFF);
    return false;
}

bool IrisXeBacklight::doUpdate() {
    return true;
}
