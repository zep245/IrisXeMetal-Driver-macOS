#include <mach/mach_types.h>
// IrisXeAGDC has no IOKit personality: its module start only registers the sysctls
// (debug.irisxe_agdc, debug.irisxe_agdc_status). libkmodc++'s _start calls _realmain.
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t irisxe_agdc_start(kmod_info_t *ki, void *data);
extern kern_return_t irisxe_agdc_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.irisxe.IrisXeAGDC, KEXT_VERSION, _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = irisxe_agdc_start;
__private_extern__ kmod_stop_func_t *_antimain = irisxe_agdc_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
