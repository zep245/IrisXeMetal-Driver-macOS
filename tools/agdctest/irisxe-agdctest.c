// irisxe-agdctest (v0.52.0): does what WindowServer's IOPresentment does to find the screens of a GPU.
// For every AppleGraphicsDeviceControl service (except the one named AppleGPUWrangler):
//   IOServiceOpen type 2, selector 1 (vendor info, 0x2C bytes), selector 0x980 (GPU capability, 0xDC bytes).
// Then checks that the screen ID it gets back is IrisXeFB's registry ID.
// Run: sudo ./irisxe-agdctest
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint64_t regID(io_object_t o) { uint64_t id = 0; IORegistryEntryGetRegistryEntryID(o, &id); return id; }

static uint64_t serviceID(const char* cls)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(cls));
    if (!s) return 0;
    uint64_t id = regID(s);
    IOObjectRelease(s);
    return id;
}

int main(void)
{
    uint64_t fbID = serviceID("IrisXeFB");
    printf("IrisXeFB registry ID: 0x%llx%s\n", fbID, fbID ? "" : " (IrisXeFB not found)");

    io_iterator_t it = 0;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleGraphicsDeviceControl"), &it);
    if (kr != KERN_SUCCESS) { printf("FAIL: cannot list AGDC services (0x%x)\n", kr); return 1; }

    int found = 0, ours = 0, pass = 0;
    io_service_t s;
    while ((s = IOIteratorNext(it))) {
        io_name_t name = "";
        IORegistryEntryGetName(s, name);
        io_name_t cls = "";
        IOObjectGetClass(s, cls);
        printf("\n== AGDC service \"%s\" (class %s, ID 0x%llx)\n", name, cls, regID(s));
        if (!strcmp(name, "AppleGPUWrangler")) { printf("   skipped (WindowServer skips this one too)\n"); IOObjectRelease(s); continue; }
        found++;
        int isOurs = !strcmp(cls, "IrisXeAGDC");
        if (isOurs) ours++;

        io_connect_t c = 0;
        kr = IOServiceOpen(s, mach_task_self(), 2, &c);
        if (kr != KERN_SUCCESS) { printf("   FAIL: IOServiceOpen type 2 -> 0x%x\n", kr); IOObjectRelease(s); continue; }

        uint8_t vi[0x2C]; size_t vs = sizeof(vi); memset(vi, 0, sizeof(vi));
        kr = IOConnectCallMethod(c, 1, NULL, 0, NULL, 0, NULL, NULL, vi, &vs);
        uint32_t type = *(uint32_t*)(vi + 0x28);
        printf("   selector 1 (vendor info): 0x%x, %zu bytes, version 0x%x, name \"%.32s\", vendor 0x%x, type %u -> %s\n",
               kr, vs, *(uint32_t*)vi, (char*)vi + 4, *(uint16_t*)(vi + 0x24), type,
               (kr == KERN_SUCCESS && vs && (type == 1 || type == 2)) ? "OK for WindowServer" : "WindowServer would reject this");

        uint8_t gc[0xDC]; size_t gs = sizeof(gc); memset(gc, 0, sizeof(gc));
        kr = IOConnectCallMethod(c, 0x980, NULL, 0, NULL, 0, NULL, NULL, gc, &gs);
        uint32_t n = *(uint32_t*)(gc + 0x30);
        printf("   selector 0x980 (GPU capability): 0x%x, %zu bytes, ports 0x%llx, values %u %u, screens %u, GPU ID 0x%llx\n",
               kr, gs, *(uint64_t*)gc, *(uint32_t*)(gc + 0x28), *(uint32_t*)(gc + 0x2C), n, *(uint64_t*)(gc + 0x34));
        int fbMatch = 0;
        for (uint32_t i = 0; i < n && i < 8; i++) {
            uint64_t id = *(uint64_t*)(gc + 0x3C + 8 * i);
            printf("     screen %u ID 0x%llx%s\n", i, id, id && id == fbID ? "  = IrisXeFB" : "");
            if (id && id == fbID) fbMatch = 1;
        }
        if (isOurs) {
            int ok = kr == KERN_SUCCESS && n == 1 && fbMatch && (type == 1 || type == 2);
            printf("   %s\n", ok ? "PASS: ours answers like Apple's, screen = IrisXeFB" : "FAIL: ours does not answer as WindowServer needs");
            if (ok) pass = 1;
        }
        IOServiceClose(c);
        IOObjectRelease(s);
    }
    IOObjectRelease(it);

    printf("\n%d AGDC service(s) checked, %d of them IrisXeAGDC\n", found, ours);
    if (!ours) printf("RESULT: IrisXeAGDC not found - run: sudo sysctl debug.irisxe_agdc=1\n");
    else printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
