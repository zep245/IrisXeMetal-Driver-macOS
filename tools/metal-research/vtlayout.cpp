// Compiled only to print the compiler's vtable layout for the kernel base
// classes (clang -fdump-vtable-layouts), i.e. the exact order of the slots a
// kext subclass inherits from OSObject / IORegistryEntry / IOService.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
class VTService : public IOService { OSDeclareDefaultStructors(VTService) };
OSDefineMetaClassAndStructors(VTService, IOService)
class VTUserClient : public IOUserClient { OSDeclareDefaultStructors(VTUserClient) };
OSDefineMetaClassAndStructors(VTUserClient, IOUserClient)
