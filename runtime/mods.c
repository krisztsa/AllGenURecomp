// Mod folders (mods.h).
#include "mods.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rt.h"

static bool is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

const char *mods_dir(void) {
    const char *e = getenv("MHGU_MODS_DIR");
    return e && *e ? e : "mods";
}

void mods_make_dir(void) {
    if (!is_dir(mods_dir())) mkdir(mods_dir(), 0755);
}

static int cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

#define MAX_MODS 256
static char *g_names[MAX_MODS];
static int g_nmods = -1;

static void list_mods(void) {
    if (g_nmods >= 0) return;
    g_nmods = 0;
    char path[1100];
    snprintf(path, sizeof path, "%s/load_order.txt", mods_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (g_nmods < MAX_MODS && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#') continue;
            g_names[g_nmods++] = strdup(s);
        }
        fclose(f);
        LOG("[mods] %d mod(s) listed in %s\n", g_nmods, path);
        return;
    }
    DIR *d = opendir(mods_dir());
    if (!d) return;
    struct dirent *e;
    while (g_nmods < MAX_MODS && (e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
        snprintf(path, sizeof path, "%s/%s", mods_dir(), e->d_name);
        if (is_dir(path)) g_names[g_nmods++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(g_names, g_nmods, sizeof *g_names, cmp);
}

// The folder named `want` (any case) in dir or up to `depth` levels below it (shallowest first), into out.
static bool find_dir(const char *dir, const char *want, int depth, char *out, size_t n) {
    for (int level = 0; level <= depth; level++) {
        // breadth first: walk the tree to `level` and look there
        struct { char path[1024]; int lv; } stack[64];
        int sp = 0;
        snprintf(stack[sp].path, sizeof stack[sp].path, "%s", dir), stack[sp++].lv = 0;
        while (sp) {
            sp--;
            char cur[1024];
            int lv = stack[sp].lv;
            snprintf(cur, sizeof cur, "%s", stack[sp].path);
            DIR *d = opendir(cur);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char p[1100];
                snprintf(p, sizeof p, "%s/%s", cur, e->d_name);
                if (!is_dir(p)) continue;
                if (lv == level && !strcasecmp(e->d_name, want)) {
                    closedir(d);
                    snprintf(out, n, "%s", p);
                    return true;
                }
                if (lv < level && sp < 64) snprintf(stack[sp].path, sizeof stack[sp].path, "%s", p), stack[sp++].lv = lv + 1;
            }
            closedir(d);
        }
    }
    return false;
}

int mods_romfs_layers(const char **out, int max) {
    list_mods();
    int k = 0;
    for (int i = 0; i < g_nmods && k < max; i++) {
        char mod[1100], path[1100];
        snprintf(mod, sizeof mod, "%s/%s", mods_dir(), g_names[i]);
        if (!is_dir(mod)) { LOG("[mods] %s: no such folder in %s\n", g_names[i], mods_dir()); continue; }
        bool found = find_dir(mod, "romfs", 3, path, sizeof path);
        if (!found && find_dir(mod, "nativeNX", 3, path, sizeof path)) {  // the files without their romfs/ folder
            *strrchr(path, '/') = 0;
            found = true;
        }
        if (!found) continue;
        out[k++] = strdup(path);
        LOG("[mods] %s: romfs %s\n", g_names[i], path);
    }
    return k;
}

// ------------------------------------------------------------------ exefs patches
typedef struct {
    Module *m;
    const char *mod, *file;
    int data, code, bad;
    uint32_t first_code[4];
} Apply;

// One patch: `len` bytes at offset `off` of the module's memory image.
static void apply(Apply *a, uint32_t off, const uint8_t *bytes, uint32_t len) {
    Module *m = a->m;
    if (!len) return;
    if ((uint64_t)off + len > m->size) { a->bad++; return; }
    if (off < m->text_off + m->text_size && off + len > m->text_off) {  // code: compiled ahead, see mods.h
        if (a->code < 4) a->first_code[a->code] = m->base + off;
        a->code++;
        return;
    }
    memcpy(G2H(m->base + off), bytes, len);
    a->data++;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }

// Leading hex digits of `s` as a build id prefix: does it match `id`? (Patches name or tag a shortened id.)
static bool id_matches(const char *s, const uint8_t id[32]) {
    int n = 0;
    for (; hexval(s[n]) >= 0 && n < 64; n++)
        if (hexval(s[n]) != ((id[n / 2] >> (n & 1 ? 0 : 4)) & 15)) return false;
    return n >= 16;  // (at least 8 bytes of the id)
}

// IPSwitch text patches: "@nsobid-<id>", "@flag offset_shift <n>", "@little-endian" / "@big-endian",
// "@enabled" / "@disabled" blocks of "<hex offset> <hex bytes | "string">" lines, "@stop", // comments.
// Offsets + offset_shift are IPS offsets (the NSO file's 0x100-byte header included).
static void pchtxt(Apply *a, const char *path, const uint8_t id[32]) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    bool ours = false, on = false, le = false;
    long shift = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *cm = strstr(s, "//");
        if (cm) *cm = 0;
        s[strcspn(s, "\r\n")] = 0;
        if (!*s) continue;
        if (!strncasecmp(s, "@nsobid-", 8)) { ours = id_matches(s + 8, id); continue; }
        if (!strncasecmp(s, "@flag", 5)) {
            char key[64];
            long v;
            if (sscanf(s + 5, "%63s %li", key, &v) == 2 && !strcasecmp(key, "offset_shift")) shift = v;
            continue;
        }
        if (!strncasecmp(s, "@little-endian", 14)) { le = true; continue; }
        if (!strncasecmp(s, "@big-endian", 11)) { le = false; continue; }
        if (!strncasecmp(s, "@enabled", 8)) { on = true; continue; }
        if (!strncasecmp(s, "@disabled", 9)) { on = false; continue; }
        if (!strncasecmp(s, "@stop", 5)) break;
        if (*s == '@' || !ours || !on) continue;
        char *end;
        unsigned long off = strtoul(s, &end, 16);
        if (end == s || !isspace((unsigned char)*end)) continue;
        while (isspace((unsigned char)*end)) end++;
        uint8_t buf[2048];
        uint32_t n = 0;
        if (*end == '"') {  // a string, written without its terminator
            for (char *q = end + 1; *q && *q != '"' && n < sizeof buf; q++) {
                if (*q == '\\' && q[1]) {
                    q++;
                    buf[n++] = *q == 'n' ? '\n' : *q == 't' ? '\t' : *q == '0' ? 0 : (uint8_t)*q;
                } else {
                    buf[n++] = (uint8_t)*q;
                }
            }
        } else {
            for (char *q = end; hexval(q[0]) >= 0 && hexval(q[1]) >= 0 && n < sizeof buf; q += 2)
                buf[n++] = (uint8_t)(hexval(q[0]) << 4 | hexval(q[1]));
            if (le)
                for (uint32_t i = 0; i < n / 2; i++) { uint8_t t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t; }
        }
        long ips = (long)off + shift;
        if (ips < 0x100) { a->bad++; continue; }
        apply(a, (uint32_t)(ips - 0x100), buf, n);
    }
    fclose(f);
}

// IPS ("PATCH", 24-bit offsets, "EOF") and IPS32 ("IPS32", 32-bit offsets, "EEOF"); offsets include the NSO's
// 0x100-byte header. A size of 0 is a run of one byte.
static void ips(Apply *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? len : 1);
    long got = (long)fread(b, 1, len, f);
    fclose(f);
    bool i32 = got >= 5 && !memcmp(b, "IPS32", 5);
    if (!i32 && (got < 5 || memcmp(b, "PATCH", 5))) { a->bad++; free(b); return; }
    int ow = i32 ? 4 : 3;
    long p = 5;
    while (p + ow <= got) {
        if (!memcmp(b + p, i32 ? "EEOF" : "EOF", ow)) break;
        uint32_t off = 0;
        for (int i = 0; i < ow; i++) off = off << 8 | b[p + i];
        p += ow;
        if (p + 2 > got) break;
        uint32_t size = (uint32_t)b[p] << 8 | b[p + 1];
        p += 2;
        uint8_t *data = b + p, run[0x10000];
        if (!size) {
            if (p + 3 > got) break;
            size = (uint32_t)b[p] << 8 | b[p + 1];
            memset(run, b[p + 2], size);
            data = run;
            p += 3;
        } else {
            if (p + size > got) break;
            p += size;
        }
        if (off < 0x100) { a->bad++; continue; }
        apply(a, off - 0x100, data, size);
    }
    free(b);
}

void mods_patch_module(Module *m, const uint8_t build_id[32]) {
    list_mods();
    for (int i = 0; i < g_nmods; i++) {
        char mod[1100], ex[1100];
        snprintf(mod, sizeof mod, "%s/%s", mods_dir(), g_names[i]);
        if (!find_dir(mod, "exefs", 3, ex, sizeof ex)) continue;
        DIR *d = opendir(ex);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", ex, e->d_name);
            Apply a = {m, g_names[i], e->d_name, 0, 0, 0, {0}};
            if (!strcasecmp(dot, ".pchtxt")) pchtxt(&a, path, build_id);
            else if (!strcasecmp(dot, ".ips") && id_matches(e->d_name, build_id)) ips(&a, path);
            else continue;
            if (!a.data && !a.code && !a.bad) continue;  // (for another module or version)
            LOG("[mods] %s: %s for %s: %d data patch(es) applied", a.mod, a.file, m->name, a.data);
            if (a.code) {
                LOG(", %d code patch(es) NOT applied (at", a.code);
                for (int k = 0; k < a.code && k < 4; k++) LOG(" %08x", a.first_code[k]);
                LOG("%s): the port runs the game's code compiled ahead of time, patched instructions change nothing",
                    a.code > 4 ? " ..." : "");
            }
            if (a.bad) LOG(", %d outside the module (ignored)", a.bad);
            LOG("\n");
        }
        closedir(d);
    }
}
