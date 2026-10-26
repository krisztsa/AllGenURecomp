// NSO loading (AArch32 modules), REL relocation and symbol resolution.
#include <stdlib.h>
#include <string.h>

#include "mods.h"
#include "rt.h"

Module g_modules[8];
int g_nmodules;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_INIT = 12, DT_FINI = 13, DT_REL = 17, DT_RELSZ = 18, DT_PLTREL = 20, DT_JMPREL = 23,
       DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28 };
enum { R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };

typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } Elf32Sym;
typedef struct { int32_t tag; uint32_t val; } Elf32Dyn;

static size_t lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { b = *s++; lit += b; } while (b == 255); }
        if (d + lit > de || s + lit > se) break;
        memcpy(d, s, lit);
        d += lit; s += lit;
        if (s >= se) break;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        uint8_t *m = d - off;
        if (m < dst || d + ml > de) break;
        while (ml--) *d++ = *m++;
    }
    return d - dst;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    if (fread(p, 1, *len, f) != *len) { fclose(f); free(p); return NULL; }
    fclose(f);
    return p;
}

static void load_nso(Module *m, const char *path, uint32_t base) {
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw || memcmp(raw, "NSO0", 4)) FATAL("cannot load %s\n", path);
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    uint8_t build_id[32];
    memcpy(build_id, raw + 0x40, 32);
    uint32_t size = (seg[2][1] + seg[2][2] + bss + 0xFFF) & ~0xFFFu;
    plat_commit(GUEST_BASE + base, size);
    memset(G2H(base), 0, size);
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        uint8_t *dst = (uint8_t *)G2H(base + seg[i][1]);
        if (flags & (1 << i)) {
            size_t got = lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1]; m->data_size = seg[2][2] + bss;
    m->bss_size = bss;

    uint32_t mod0 = LD32(base + 4);
    uint32_t mod = base + mod0;
    m->dynamic = mod + (int32_t)LD32(mod + 4);
    for (const Elf32Dyn *d = G2H(m->dynamic); d->tag != DT_NULL; d++) {
        switch (d->tag) {
        case DT_SYMTAB: m->symtab = base + d->val; break;
        case DT_STRTAB: m->strtab = base + d->val; break;
        case DT_HASH: m->nsyms = LD32(base + d->val + 4); break;
        case DT_INIT: m->init = base + d->val; break;
        case DT_FINI: m->fini = base + d->val; break;
        case DT_INIT_ARRAY: m->init_array = base + d->val; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d->val; break;
        case DT_FINI_ARRAY: m->fini_array = base + d->val; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d->val; break;
        }
    }
    LOG("loaded %-5s at %08x size %08x syms %u\n", m->name, base, size, m->nsyms);
    mods_patch_module(m, build_id);  // (before relocation, as on the console)
}

static const Elf32Sym *sym(const Module *m, uint32_t i) { return (const Elf32Sym *)G2H(m->symtab) + i; }
static const char *symname(const Module *m, const Elf32Sym *s) { return gstr(m->strtab + s->name); }

uint32_t loader_find_symbol(const char *name) {
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint32_t i = 1; i < m->nsyms; i++) {
            const Elf32Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return 0;
}

const char *loader_symbol_at(uint32_t addr, uint32_t *offset) {
    const char *best = NULL;
    uint32_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint32_t i = 1; i < m->nsyms; i++) {
            const Elf32Sym *s = sym(m, i);
            uint32_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

static uint32_t resolve(Module *m, uint32_t symidx) {
    const Elf32Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    if (s->shndx) return m->base + s->value;
    HostFn impl = hle_lookup_impl(name);
    if (impl) return hle_register(name, impl);  // a host implementation wins over the guest SDK's
    uint32_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    return hle_register(name, NULL);
}

static void relocate(Module *m) {
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltsz = 0, pltrel = DT_REL;
    for (const Elf32Dyn *d = G2H(m->dynamic); d->tag != DT_NULL; d++) {
        if (d->tag == DT_REL) rel = m->base + d->val;
        if (d->tag == DT_RELSZ) relsz = d->val;
        if (d->tag == DT_JMPREL) jmprel = m->base + d->val;
        if (d->tag == DT_PLTRELSZ) pltsz = d->val;
        if (d->tag == DT_PLTREL) pltrel = d->val;
        if (d->tag == DT_RELA) FATAL("%s: RELA relocations in an AArch32 module\n", m->name);
    }
    if (pltrel != DT_REL) FATAL("%s: PLT uses RELA\n", m->name);
    int unresolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t tab = pass ? jmprel : rel, n = (pass ? pltsz : relsz) / 8;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t off = LD32(tab + 8 * i), info = LD32(tab + 8 * i + 4);
            uint32_t where = m->base + off, type = info & 0xFF, si = info >> 8;
            switch (type) {
            case R_ARM_RELATIVE: ST32(where, LD32(where) + m->base); break;
            case R_ARM_ABS32: {
                uint32_t s = resolve(m, si);
                ST32(where, s ? s + LD32(where) : 0);
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: {
                uint32_t s = resolve(m, si);
                ST32(where, s);
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            default: FATAL("%s: reloc type %u\n", m->name, type);
            }
        }
    }
    LOG("relocated %-5s (%d imports bound to host functions)\n", m->name, unresolved);
}

void loader_load(const char *exefs) {
    char path[1024];
    static const struct { const char *name; uint32_t base; } mods[] = {{"main", MAIN_BASE}, {"sdk", SDK_BASE}};
    for (unsigned i = 0; i < 2; i++) {
        Module *m = &g_modules[g_nmodules++];
        m->name = mods[i].name;
        snprintf(path, sizeof path, "%s/%s", exefs, m->name);
        load_nso(m, path, mods[i].base);
    }
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
}

uint32_t loader_module_symbol(const Module *m, const char *name) {
    for (uint32_t i = 1; i < m->nsyms; i++) {
        const Elf32Sym *s = sym(m, i);
        if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
    }
    return 0;
}

// NRO: a flat image (file offsets are memory offsets) with the segment table at 0x20 and the bss size at 0x38
Module *loader_load_nro(const char *name, const uint8_t *data, size_t len, uint32_t base) {
    if (g_nmodules == (int)(sizeof g_modules / sizeof g_modules[0])) return NULL;
    if (len < 0x80 || memcmp(data + 0x10, "NRO0", 4)) return NULL;
    uint32_t seg[3][2], bss, mod0;
    memcpy(seg, data + 0x20, sizeof seg);
    memcpy(&bss, data + 0x38, 4);
    memcpy(&mod0, data + 4, 4);
    uint32_t image = seg[2][0] + seg[2][1];
    if (image > len || mod0 + 8 > len || memcmp(data + mod0, "MOD0", 4)) return NULL;
    Module *m = &g_modules[g_nmodules];
    memset(m, 0, sizeof *m);
    m->name = strdup(name);
    m->base = base;
    m->size = (image + bss + 0xFFF) & ~0xFFFu;
    plat_commit(GUEST_BASE + base, m->size);
    memset(G2H(base), 0, m->size);
    memcpy(G2H(base), data, image);
    m->text_off = seg[0][0]; m->text_size = seg[0][1];
    m->ro_off = seg[1][0];   m->ro_size = seg[1][1];
    m->data_off = seg[2][0]; m->data_size = seg[2][1] + bss;
    m->bss_size = bss;
    m->dynamic = base + mod0 + (int32_t)LD32(base + mod0 + 4);
    for (const Elf32Dyn *d = G2H(m->dynamic); d->tag != DT_NULL; d++) {
        switch (d->tag) {
        case DT_SYMTAB: m->symtab = base + d->val; break;
        case DT_STRTAB: m->strtab = base + d->val; break;
        case DT_HASH: m->nsyms = LD32(base + d->val + 4); break;
        case DT_INIT: m->init = base + d->val; break;
        case DT_FINI: m->fini = base + d->val; break;
        case DT_INIT_ARRAY: m->init_array = base + d->val; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d->val; break;
        case DT_FINI_ARRAY: m->fini_array = base + d->val; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d->val; break;
        }
    }
    g_nmodules++;
    LOG("loaded %s at %08x size %08x syms %u\n", m->name, base, m->size, m->nsyms);
    relocate(m);
    return m;
}
