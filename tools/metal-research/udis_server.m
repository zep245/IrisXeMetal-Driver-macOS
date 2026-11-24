// udis_server - tiny "memory server" for udis.py (user-space disassembly of
// Metal / IOAccelerator / GPU driver bundles from the dyld shared cache, where
// the files themselves don't exist on disk). Python drives it over stdin/out:
//   LOAD <path>                 dlopen a bundle/framework       -> OK | ERR msg
//   IMP <class> <selector>      instance method address         -> 0x... | 0
//   CIMP <class> <selector>     class method address            -> 0x... | 0
//   SYM <name>                  dlsym(RTLD_DEFAULT)             -> 0x... | 0
//   READ <addr> <len>           hex bytes                       -> hex...
//   NAME <addr>                 dladdr: image|symbol|symaddr    (+ "|stub->" target for ff 25 stubs)
//   PTR <addr>                  8-byte value at addr (+ its NAME)
//   SEL <addr>                  selector name (selector references)
//   STR <addr>                  printable C string at addr
//   CLASSES <imagesubstring>    classes whose image path contains it
//   IVARS <class>               name offset type per ivar
//   XREF <imagesub> <text>      code in images whose path contains imagesub that loads (lea rip+x) a string
//                               containing text: "funcstart hitaddr" lines (funcstart = nearest
//                               push rbp; mov rbp,rsp before the hit), then END
//   SYMS <imagesub> <text>      "addr name" of every symbol (symbol table) of the image containing text, END
//   TEXT <imagesub>             "addr size" of the image's __TEXT,__text
//   QUIT
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
// is [a, a+n) readable memory of this process?
static int readable(uintptr_t a, size_t n) {
    mach_vm_address_t addr = a; mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
        return 0;
    return addr <= a && a + n <= addr + size && (info.protection & VM_PROT_READ);
}

static void name(uintptr_t a) {
    Dl_info i;
    if (dladdr((void *)a, &i)) {
        printf("%s|%s|0x%lx", i.dli_fname ? strrchr(i.dli_fname, '/') + 1 : "?",
               i.dli_sname ? i.dli_sname : "?", (unsigned long)i.dli_saddr);
    } else printf("?|?|0");
    const unsigned char *p = (const unsigned char *)a;
    // v0.51.48: only look for a stub when the bytes are readable (data addresses crashed rounds 14/15)
    if (readable(a, 6) && dladdr((void *)a, &i) && p[0] == 0xff && p[1] == 0x25) {           // jmp *disp32(%rip)
        int32_t d; memcpy(&d, p + 2, 4);
        if (!readable(a + 6 + d, 8)) return;
        uintptr_t t = *(uintptr_t *)(a + 6 + d);
        Dl_info j;
        if (dladdr((void *)t, &j)) printf("|stub->%s|0x%lx", j.dli_sname ? j.dli_sname : "?", (unsigned long)t);
    }
}


// XREF: string search in an image's __TEXT (except __text), then lea [rip+disp32] references from __text
static void xref(const char *imgsub, const char *needle) {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        if (!nm || !strstr(nm, imgsub)) continue;
        const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        uintptr_t tx = 0, txn = 0; uintptr_t hits[64]; int nh = 0;
        const struct load_command *lc = (const struct load_command *)(h + 1);
        for (uint32_t k = 0; k < h->ncmds; k++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            if (strncmp(sg->segname, "__TEXT", 16)) continue;
            const struct section_64 *sc = (const struct section_64 *)(sg + 1);
            for (uint32_t q = 0; q < sg->nsects; q++, sc++) {
                uintptr_t a = sc->addr + slide; size_t n = sc->size;
                if (!strncmp(sc->sectname, "__text", 16)) { tx = a; txn = n; continue; }
                if (!readable(a, n)) continue;
                size_t L = strlen(needle);
                for (size_t o = 0; o + L <= n && nh < 64; o++) {
                    if (memcmp((const char *)a + o, needle, L)) continue;
                    size_t st = o; while (st > 0 && ((const char *)a)[st - 1]) st--;
                    hits[nh++] = a + st; o += L;
                }
            }
        }
        if (!tx || !nh || !readable(tx, txn)) continue;
        const unsigned char *p = (const unsigned char *)tx;
        for (size_t o = 0; o + 7 <= txn; o++) {
            if (!((p[o] == 0x48 || p[o] == 0x4c) && p[o + 1] == 0x8d && (p[o + 2] & 0xc7) == 0x05)) continue;
            int32_t d; memcpy(&d, p + o + 3, 4);
            uintptr_t t = tx + o + 7 + d;
            for (int k = 0; k < nh; k++) if (t == hits[k]) {
                size_t f = o; size_t lim = o > 65536 ? o - 65536 : 0;
                while (f > lim && !(p[f] == 0x55 && p[f + 1] == 0x48 && p[f + 2] == 0x89 && p[f + 3] == 0xe5)) f--;
                printf("0x%lx 0x%lx\n", (unsigned long)(tx + f), (unsigned long)(tx + o));
            }
        }
    }
}

static const struct mach_header_64 *imageFor(const char *imgsub, intptr_t *slide) {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, imgsub)) { *slide = _dyld_get_image_vmaddr_slide(i); return (const struct mach_header_64 *)_dyld_get_image_header(i); }
    }
    return NULL;
}
static void textRange(const char *imgsub) {
    intptr_t slide; const struct mach_header_64 *h = imageFor(imgsub, &slide);
    if (!h) { printf("0 0\n"); return; }
    const struct load_command *lc = (const struct load_command *)(h + 1);
    for (uint32_t k = 0; k < h->ncmds; k++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
        const struct section_64 *sc = (const struct section_64 *)(sg + 1);
        for (uint32_t q = 0; q < sg->nsects; q++, sc++)
            if (!strncmp(sc->segname, "__TEXT", 16) && !strncmp(sc->sectname, "__text", 16)) {
                printf("0x%lx %lu\n", (unsigned long)(sc->addr + slide), (unsigned long)sc->size); return;
            }
    }
    printf("0 0\n");
}
static void syms(const char *imgsub, const char *needle) {
    intptr_t slide; const struct mach_header_64 *h = imageFor(imgsub, &slide);
    if (!h) return;
    const struct load_command *lc = (const struct load_command *)(h + 1);
    uintptr_t leBase = 0; const struct symtab_command *st = NULL;
    for (uint32_t k = 0; k < h->ncmds; k++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            if (!strncmp(sg->segname, "__LINKEDIT", 16)) leBase = sg->vmaddr + slide - sg->fileoff;
        } else if (lc->cmd == LC_SYMTAB) st = (const struct symtab_command *)lc;
    }
    if (!st || !leBase) return;
    const struct nlist_64 *nl = (const struct nlist_64 *)(leBase + st->symoff);
    const char *str = (const char *)(leBase + st->stroff);
    if (!readable((uintptr_t)nl, sizeof(*nl)) || !readable((uintptr_t)str, 1)) return;
    for (uint32_t i = 0; i < st->nsyms; i++) {
        if (!readable((uintptr_t)&nl[i], sizeof(*nl))) break;
        if ((nl[i].n_type & N_STAB) || !nl[i].n_value) continue;
        const char *n = str + nl[i].n_un.n_strx;
        if (!readable((uintptr_t)n, 1)) continue;
        if (strstr(n, needle)) printf("0x%lx %s\n", (unsigned long)(nl[i].n_value + slide), n);
    }
}

int main(void) {
    @autoreleasepool {
        char line[4096];
        setvbuf(stdout, NULL, _IOLBF, 0);
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\r\n")] = 0;
            char a[2048] = "", b[1024] = "", c[1024] = "";
            sscanf(line, "%2047s %1023s %1023s", a, b, c);
            if (!strcmp(a, "QUIT")) break;
            else if (!strcmp(a, "LOAD")) {
                void *h = dlopen(line + 5, RTLD_NOW | RTLD_GLOBAL);
                if (h) printf("OK\n"); else printf("ERR %s\n", dlerror());
            } else if (!strcmp(a, "IMP") || !strcmp(a, "CIMP")) {
                Class k = objc_getClass(b);
                if (k && a[0] == 'C') k = object_getClass(k);
                Method m = k ? class_getInstanceMethod(k, sel_registerName(c)) : NULL;
                printf("0x%lx\n", m ? (unsigned long)method_getImplementation(m) : 0UL);
            } else if (!strcmp(a, "SYM")) {
                printf("0x%lx\n", (unsigned long)dlsym(RTLD_DEFAULT, b));
            } else if (!strcmp(a, "READ")) {
                uintptr_t ad = strtoull(b, NULL, 0); size_t n = strtoull(c, NULL, 0);
                Dl_info i;
                if (!dladdr((void *)ad, &i)) { printf("\n"); continue; }
                // v0.51.47: stop at the first unreadable page (reading past an image's end crashed round 14)
                for (size_t k = 0; k < n; k++) {
                    if (((ad + k) & 0xfff) == 0 || k == 0) { if (!readable(ad + k, 1)) break; }
                    printf("%02x", ((unsigned char *)ad)[k]);
                }
                printf("\n");
            } else if (!strcmp(a, "NAME")) {
                name(strtoull(b, NULL, 0)); printf("\n");
            } else if (!strcmp(a, "PTR")) {
                uintptr_t ad = strtoull(b, NULL, 0); Dl_info i;
                if (!readable(ad, 8)) { printf("0|?|?|0\n"); continue; }
                uintptr_t v = *(uintptr_t *)ad;
                printf("0x%lx|", (unsigned long)v); name(v); printf("\n");
            } else if (!strcmp(a, "SEL")) {           // selector name if addr is a registered SEL
                uintptr_t ad = strtoull(b, NULL, 0);
                if (ad && readable(ad, 1) && sel_isMapped((SEL)ad)) printf("%s\n", sel_getName((SEL)ad)); else printf("\n");
            } else if (!strcmp(a, "STR")) {           // printable C string at addr (selectors, class names)
                uintptr_t ad = strtoull(b, NULL, 0); Dl_info i;
                (void)i;
                if (!ad || !readable(ad, 1)) { printf("\n"); continue; }
                const char *p = (const char *)ad; int k = 0;
                while (k < 200 && readable(ad + k, 1) && p[k] >= 0x20 && p[k] < 0x7f) k++;
                if (p[k] == 0 && k > 0) printf("%.*s\n", k, p); else printf("\n");
            } else if (!strcmp(a, "CLASSES")) {
                unsigned n = 0; Class *cl = objc_copyClassList(&n);
                for (unsigned k = 0; k < n; k++) {
                    const char *img = class_getImageName(cl[k]);
                    if (img && strstr(img, b)) printf("%s\n", class_getName(cl[k]));
                }
                free(cl); printf("END\n");
            } else if (!strcmp(a, "IVARS")) {
                Class k = objc_getClass(b); unsigned n = 0;
                Ivar *iv = k ? class_copyIvarList(k, &n) : NULL;
                for (unsigned x = 0; x < n; x++)
                    printf("%s 0x%lx %s\n", ivar_getName(iv[x]), (long)ivar_getOffset(iv[x]), ivar_getTypeEncoding(iv[x]));
                free(iv); printf("END\n");
            } else if (!strcmp(a, "SYMS")) {
                const char *t = strstr(line + 5, " ");
                syms(b, t ? t + 1 : "");
                printf("END\n");
            } else if (!strcmp(a, "TEXT")) {
                textRange(b);
            } else if (!strcmp(a, "XREF")) {
                const char *t = strstr(line + 5, " ");
                xref(b, t ? t + 1 : "");
                printf("END\n");
            } else printf("ERR unknown\n");
            fflush(stdout);
        }
    }
    return 0;
}
