// Cheats in the Atmosphère format (dmnt's cheat VM, docs/features/cheats.md in Atmosphère), as the Switch emulators read them: <files>/cheats/*.txt, each
//   [Name]            a cheat, off until switched on (toggles.txt)
//   {Name}            a master code: always runs, before the cheats
//   0TMR00AA AAAAAAAA VVVVVVVV ...   its opcodes, 8 hex digits each
// A file named after a build ID (the first 16 hex digits, as Atmosphère's <build id>.txt) is only used when it is this game version's main; any other name always.
// toggles.txt (written by the in-game menu, GameMenu.java): "[Name]" then "true" / "false", as Atmosphère's. Read again on dm_cheats_reload (the menu's switches).
// The enabled cheats run 12 times a second on their own thread, as dmnt does. Memory types: 0 main, 1 heap, 2 alias region, 3 the whole address block (ASLR region).
// Every access is checked to lie in guest memory (the address block or a module) and goes through process_vm_readv / writev, so an unmapped address fails instead of
//   crashing the game. A write into code or read-only data is made by opening the page for writing (mprotect), then closing it again.
// Off while local play has a session (hle_ldn.c dm_ldn_in_session), and local play refuses to start while any cheat is on (as with mods).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // (glibc: process_vm_readv / writev)
#endif
#include "dm.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/uio.h>
#endif

void dm_guest_regions(uint64_t *as, uint64_t *as_n, uint64_t *heap, uint64_t *heap_n, uint64_t *alias, uint64_t *alias_n);   // hle_svc.c
uint64_t dm_input_held(void);   // hle_hid.c
int dm_ldn_in_session(void);    // hle_ldn.c

#define MAX_CHEATS 512
#define MAX_OPS 4096
typedef struct { char file[64], name[96]; int master, on, bad; atomic_int fails; uint32_t *op; int nop; } cheat;
static cheat g_ch[MAX_CHEATS]; static int g_nch;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_enabled;   // cheats switched on (not counting master codes)
static uint64_t g_static[256];   // C3: static registers, kept across runs

// ---------- guest memory ----------
static int in_guest(uint64_t a, uint64_t n) {
    if (!n || a + n < a) return 0;
    uint64_t as, asn, h, hn, al, aln; dm_guest_regions(&as, &asn, &h, &hn, &al, &aln);
    if (as && a >= as && a + n <= as + asn) return 1;
    for (int i = 0; i < g_nmod; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_mod[i].base; if (!b) continue;
        if (a >= b && a + n <= b + g_mod[i].size) return 1;
    }
    return 0;
}
static module_t *module_at(uint64_t a) {
    for (int i = 0; i < g_nmod; i++) { uint64_t b = (uint64_t)(uintptr_t)g_mod[i].base; if (b && a >= b && a < b + g_mod[i].size) return &g_mod[i]; }
    return NULL;
}
static int rd(uint64_t a, void *out, size_t n) {
    memset(out, 0, n);
#if defined(__linux__)
    if (!in_guest(a, n)) return 0;
    struct iovec l = { out, n }, r = { (void *)(uintptr_t)a, n };
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
#else
    (void)a; return 0;
#endif
}
static int wr(uint64_t a, const void *v, size_t n) {
#if defined(__linux__)
    if (!in_guest(a, n)) return 0;
    struct iovec l = { (void *)v, n }, r = { (void *)(uintptr_t)a, n };
    if (process_vm_writev(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n) return 1;
    // code or read-only data of a module (a code cheat): the pages opened for the write, then given their own protection back
    module_t *m = module_at(a); if (!m || module_at(a + n - 1) != m) return 0;
    uint64_t b = (uint64_t)(uintptr_t)m->base, off = a - b; if (off >= m->data_off) return 0;
    int code = off < m->text_end;
    long pg = sysconf(_SC_PAGESIZE); uintptr_t p0 = (uintptr_t)a & ~(uintptr_t)(pg - 1), p1 = ((uintptr_t)a + n + pg - 1) & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)p0, p1 - p0, PROT_READ | PROT_WRITE)) return 0;
    memcpy((void *)(uintptr_t)a, v, n);
    mprotect((void *)p0, p1 - p0, code ? PROT_READ | PROT_EXEC : PROT_READ);
    if (code) __builtin___clear_cache((char *)(uintptr_t)a, (char *)(uintptr_t)a + n);
    return 1;
#else
    (void)a; (void)v; (void)n; return 0;
#endif
}
static uint64_t region(int m) {
    uint64_t as, asn, h, hn, al, aln; dm_guest_regions(&as, &asn, &h, &hn, &al, &aln);
    if (m == 0) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return (uint64_t)(uintptr_t)g_mod[i].base; return 0; }
    return m == 1 ? h : m == 2 ? al : m == 3 ? as : 0;
}
static int width_ok(int t) { return t == 1 || t == 2 || t == 4 || t == 8; }
static uint64_t mask(int t) { return t == 8 ? ~0ull : (1ull << (8 * t)) - 1; }

// ---------- the VM ----------
#define N(w, i) ((int)(((w) >> (28 - 4 * (i))) & 0xF))   // the i-th hex digit, from the left
// the length in words of the opcode at p (0: not a valid opcode); left words: how many are there
static int op_len(const uint32_t *p, int left) {
    uint32_t w = p[0]; int t = N(w, 1), len;
    switch (N(w, 0)) {
        case 0x0: case 0x1: if (!width_ok(t)) return 0; len = t == 8 ? 4 : 3; break;
        case 0x2: len = 1; break;
        case 0x3: len = t == 0 ? 2 : t == 1 ? 1 : 0; break;
        case 0x4: len = 3; break;
        case 0x5: if (!width_ok(t)) return 0; len = 2; break;
        case 0x6: if (!width_ok(t)) return 0; len = 3; break;
        case 0x7: if (!width_ok(t)) return 0; len = 2; break;
        case 0x8: len = 1; break;
        case 0x9: if (!width_ok(t)) return 0; len = N(w, 5) == 1 ? 1 + (t == 8 ? 2 : 1) : 1; break;
        case 0xA: { if (!width_ok(t)) return 0; int o = N(w, 5); if (o > 5) return 0; len = o == 2 || o == 4 || o == 5 ? 2 : 1; break; }
        case 0xC: {
            int sub = N(w, 1);
            if (sub == 0) { int tt = N(w, 2), x = N(w, 5); if (!width_ok(tt) || x > 5) return 0; len = x == 0 || x == 2 ? 2 : x == 4 ? 1 + (tt == 8 ? 2 : 1) : 1; }
            else if (sub <= 3) len = 1; else return 0;
            break; }
        case 0xF: {
            int sub = (int)((w >> 20) & 0xFFF);
            if (sub == 0xFF0 || sub == 0xFF1) len = 1;
            else if (sub == 0xFFF) { int x = N(w, 5); len = x == 0 || x == 2 ? 2 : x <= 4 ? 1 : 0; }
            else return 0;
            break; }
        default: return 0;
    }
    return len <= left ? len : 0;
}
static int cond(int c, uint64_t a, uint64_t b) {
    switch (c) { case 1: return a > b; case 2: return a >= b; case 3: return a < b; case 4: return a <= b; case 5: return a == b; case 6: return a != b; }
    return 0;
}
static int is_cond_start(uint32_t w) { return N(w, 0) == 0x1 || N(w, 0) == 0x8 || (N(w, 0) == 0xC && N(w, 1) == 0); }
// after a condition that failed (or the end of a taken branch, else_ok 0): past the matching else (21000000) or end (20000000)
static int skip_block(const uint32_t *p, int n, int ip, int else_ok) {
    int depth = 0;
    while (ip < n) {
        int len = op_len(p + ip, n - ip); if (!len) return n;
        uint32_t w = p[ip]; ip += len;
        if (is_cond_start(w)) depth++;
        else if (N(w, 0) == 0x2) {
            if (depth) { if (N(w, 1) == 0) depth--; continue; }
            if (N(w, 1) == 0) return ip;            // end
            if (N(w, 1) == 1 && else_ok) return ip;  // else: run its branch
        }
    }
    return n;
}
static uint64_t rdv(uint64_t a, int t, int *ok) { uint64_t v = 0; *ok = rd(a, &v, (size_t)t); return v & mask(t); }
static int wrv(uint64_t a, uint64_t v, int t) { return wr(a, &v, (size_t)t); }   // (little-endian: the low t bytes)
static uint64_t alu(int c, uint64_t a, uint64_t b) {
    switch (c) { case 0: return a + b; case 1: return a - b; case 2: return a * b; case 3: return a << (b & 63); case 4: return a >> (b & 63);
                 case 5: return a & b; case 6: return a | b; case 7: return ~a; case 8: return a ^ b; case 9: return a; }
    return a;
}
// one run of a program (the master codes and the switched-on cheats, in that order, as dmnt joins them)
static void run(const uint32_t *p, int n, cheat **owner, int *start) {
    uint64_t r[16] = {0}, saved[16] = {0}; int top[16] = {0};
    uint64_t keys = dm_input_held(); long steps = 0;
    for (int ip = 0; ip < n && steps < 1000000; steps++) {
        int len = op_len(p + ip, n - ip); if (!len) break;
        const uint32_t *o = p + ip; uint32_t w = o[0]; int t = N(w, 1), ok = 1, wok = 1; int next = ip + len;
        switch (N(w, 0)) {
            case 0x0: { uint64_t a = region(N(w, 2)) + r[N(w, 3)] + ((uint64_t)(w & 0xFF) << 32 | o[1]);
                        uint64_t v = t == 8 ? (uint64_t)o[2] << 32 | o[3] : o[2]; wok = wrv(a, v, t); break; }
            case 0x1: { uint64_t a = region(N(w, 2)) + ((uint64_t)(w & 0xFF) << 32 | o[1]);
                        uint64_t v = t == 8 ? (uint64_t)o[2] << 32 | o[3] : o[2];
                        uint64_t m = rdv(a, t, &ok); if (!cond(N(w, 3), m, v & mask(t))) next = skip_block(p, n, next, 1); break; }
            case 0x2: if (t == 1) next = skip_block(p, n, next, 0); break;   // else reached from the taken branch: past the end
            case 0x3: { int k = N(w, 3);
                        if (t == 0) { r[k] = o[1]; top[k] = next; }
                        else if (r[k] && --r[k]) next = top[k];
                        break; }
            case 0x4: r[N(w, 3)] = (uint64_t)o[1] << 32 | o[2]; break;
            case 0x5: { int k = N(w, 3), i = N(w, 4); uint64_t off = (uint64_t)(w & 0xFF) << 32 | o[1];
                        uint64_t a = i == 0 ? region(N(w, 2)) + off : i == 1 ? r[k] + off : region(N(w, 2)) + r[k] + off;
                        r[k] = rdv(a, t, &ok); break; }
            case 0x6: { int k = N(w, 3); uint64_t a = r[k] + (N(w, 5) ? r[N(w, 6)] : 0); wok = wrv(a, (uint64_t)o[1] << 32 | o[2], t);
                        if (N(w, 4)) r[k] += (uint64_t)t;
                        break; }
            case 0x7: { int k = N(w, 3); r[k] = alu(N(w, 4), r[k], o[1]) & mask(t); break; }
            case 0x8: { uint64_t m = w & 0x0FFFFFFF; if ((keys & m) != m) next = skip_block(p, n, next, 1); break; }
            case 0x9: { uint64_t b = N(w, 5) == 1 ? (t == 8 ? (uint64_t)o[1] << 32 | o[2] : o[1]) : r[N(w, 6)];
                        r[N(w, 3)] = alu(N(w, 2), r[N(w, 4)] & mask(t), b & mask(t)) & mask(t); break; }
            case 0xA: { int s = N(w, 2), k = N(w, 3), x = N(w, 6), aa = N(w, 7), oo = N(w, 5);
                        uint64_t imm = len == 2 ? (uint64_t)aa << 32 | o[1] : 0, a = 0;
                        switch (oo) { case 0: a = r[k]; break; case 1: a = r[k] + r[aa]; break; case 2: a = r[k] + imm; break;
                                      case 3: a = region(x) + r[k]; break; case 4: a = region(x) + imm; break; case 5: a = region(x) + r[k] + imm; break; }
                        wok = wrv(a, r[s], t); if (N(w, 4)) r[k] += (uint64_t)t; break; }
            case 0xC: {
                int sub = N(w, 1);
                if (sub == 0) {   // C0TcSX..: register against memory, a register or a value
                    int tt = N(w, 2), c = N(w, 3), s = N(w, 4), x = N(w, 5), u = N(w, 6), v7 = N(w, 7); uint64_t b = 0;
                    switch (x) {
                        case 0: b = rdv(region(u) + ((uint64_t)v7 << 32 | o[1]), tt, &ok); break;
                        case 1: b = rdv(region(u) + r[v7], tt, &ok); break;
                        case 2: b = rdv(r[u] + ((uint64_t)v7 << 32 | o[1]), tt, &ok); break;
                        case 3: b = rdv(r[u] + r[v7], tt, &ok); break;
                        case 4: b = (tt == 8 ? (uint64_t)o[1] << 32 | o[2] : o[1]) & mask(tt); break;
                        case 5: b = r[u] & mask(tt); break;
                    }
                    if (!cond(c, r[s] & mask(tt), b)) next = skip_block(p, n, next, 1);
                } else if (sub == 1) {   // C10D0Sx0: restore / save / clear one
                    int d = N(w, 3), s = N(w, 5), x = N(w, 6);
                    if (x == 0) r[d] = saved[s]; else if (x == 1) saved[d] = r[s]; else if (x == 2) saved[d] = 0; else if (x == 3) r[d] = 0;
                } else if (sub == 2) {   // C2x0XXXX: the same for the registers in a mask
                    int x = N(w, 2); for (int k = 0; k < 16; k++) if (w >> k & 1) { if (x == 0) r[k] = saved[k]; else if (x == 1) saved[k] = r[k]; else if (x == 2) saved[k] = 0; else if (x == 3) r[k] = 0; }
                } else {   // C3000XXx: static registers 00-7F read, 80-FF written
                    int sr = (int)(w >> 4 & 0xFF), k = N(w, 7); if (sr < 0x80) r[k] = g_static[sr]; else g_static[sr] = r[k];
                }
                break; }
            case 0xF: break;   // FF0 / FF1 (pause / resume the game) and FFF (debug log): not done here
        }
        if (!ok || !wok) { int c = 0; for (int i = 0; owner[i]; i++) if (ip >= start[i]) c = i; if (!atomic_fetch_add(&owner[c]->fails, 1)) LOG("[cheats] \"%s\": %s at an address outside the game's memory (opcode %08x): the cheat may be for another version", owner[c]->name, !wok ? "a write" : "a read", w); }
        ip = next;
    }
}

// ---------- files ----------
static int hex8(const char *s, uint32_t *v) {
    for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    if (s[8] && !isspace((unsigned char)s[8])) return 0;
    *v = (uint32_t)strtoul((char[9]){ s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], 0 }, NULL, 16); return 1;
}
static int toggled(const char *toggles, const char *name) {   // "[Name]\ntrue" in toggles.txt
    if (!toggles) return 0;
    size_t nl = strlen(name);
    for (const char *s = toggles; (s = strchr(s, '[')); s++) {
        if (strncmp(s + 1, name, nl) || s[1 + nl] != ']') continue;
        const char *v = s + 2 + nl; while (*v && isspace((unsigned char)*v)) v++;
        return !strncasecmp(v, "true", 4) || *v == '1';
    }
    return 0;
}
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n < 0 || n > (8 << 20)) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1); if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; } if (b) b[n] = 0; fclose(f); return b;
}
static void load_file(const char *dir, const char *fn, const char *toggles) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, fn); char *t = slurp(path); if (!t) return;
    cheat *c = NULL; uint32_t ops[MAX_OPS]; int nop = 0;
    #define CLOSE() do { if (c) { c->op = malloc(sizeof *ops * (size_t)(nop ? nop : 1)); if (c->op) memcpy(c->op, ops, sizeof *ops * (size_t)nop); c->nop = nop; \
        for (int i = 0; i < nop; ) { int l = op_len(ops + i, nop - i); if (!l) { c->bad = 1; LOG("[cheats] \"%s\" (%s): opcode %d (%08x) is not one the cheat engine knows: the cheat is left out", c->name, fn, i, ops[i]); break; } i += l; } \
        g_nch++; c = NULL; } nop = 0; } while (0)
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        while (*line && isspace((unsigned char)*line)) line++;
        char *e = line + strlen(line); while (e > line && isspace((unsigned char)e[-1])) *--e = 0;
        if (*line == '[' || *line == '{') {
            CLOSE(); char close = *line == '[' ? ']' : '}'; char *q = strrchr(line, close); if (!q || g_nch >= MAX_CHEATS) continue; *q = 0;
            c = &g_ch[g_nch]; memset(c, 0, sizeof *c); snprintf(c->file, sizeof c->file, "%s", fn); snprintf(c->name, sizeof c->name, "%s", line + 1);
            c->master = close == '}'; c->on = c->master || toggled(toggles, c->name);
            continue;
        }
        if (!c) continue;
        for (char *s = line; *s; ) {
            while (*s && isspace((unsigned char)*s)) s++;
            if (!*s) break;
            uint32_t v; if (hex8(s, &v)) { if (nop < MAX_OPS) ops[nop++] = v; s += 8; } else break;   // (the rest of the line is a comment)
        }
    }
    CLOSE();
    #undef CLOSE
    free(t);
}
static int build_file(const char *fn) {   // "<16 hex digits>.txt": made for one build
    if (strlen(fn) != 20 || strcasecmp(fn + 16, ".txt")) return 0;
    for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)fn[i])) return 0;
    return 1;
}
static int by_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
void dm_cheats_reload(void) {
    const char *dir = getenv("DM_CHEATS_DIR"); if (!dir || !*dir) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nch; i++) free(g_ch[i].op);
    g_nch = 0;
    char tp[1024]; snprintf(tp, sizeof tp, "%s/toggles.txt", dir); char *toggles = slurp(tp);
    const char *build = dm_main_build();
    DIR *d = opendir(dir); char *names[256]; int nn = 0;
    if (d) { struct dirent *de; while ((de = readdir(d)) && nn < 256) { const char *fn = de->d_name; size_t l = strlen(fn);
             if (l < 5 || fn[0] == '.' || strcasecmp(fn + l - 4, ".txt") || !strcasecmp(fn, "toggles.txt")) continue;
             if (build_file(fn) && strncasecmp(fn, build, 16)) { LOG("[cheats] %s is for another build (this game's main is %.16s): not used", fn, build); continue; }
             names[nn++] = strdup(fn); } closedir(d); }
    qsort(names, (size_t)nn, sizeof *names, by_name);
    for (int i = 0; i < nn; i++) { load_file(dir, names[i], toggles); free(names[i]); }
    free(toggles);
    int on = 0, ms = 0; for (int i = 0; i < g_nch; i++) { if (g_ch[i].bad) continue; if (g_ch[i].master) ms++; else if (g_ch[i].on) on++; }
    atomic_store(&g_enabled, on);
    LOG("[cheats] %d cheat(s) in %s, %d switched on, %d master code(s)", g_nch - ms, dir, on, ms);
    pthread_mutex_unlock(&g_mu);
}
int dm_cheats_enabled(void) { return atomic_load(&g_enabled); }
// for the menu: "file\tname\tmaster\ton\tstate" a line, state ok / bad (an opcode it does not know) / fail (an address outside the game's memory)
char *dm_cheats_info(void) {
    pthread_mutex_lock(&g_mu);
    size_t cap = 256 + (size_t)g_nch * 200, n = 0; char *s = malloc(cap); if (!s) { pthread_mutex_unlock(&g_mu); return NULL; }
    n += (size_t)snprintf(s + n, cap - n, "%.16s\n", dm_main_build());
    for (int i = 0; i < g_nch; i++) n += (size_t)snprintf(s + n, cap - n, "%s\t%s\t%d\t%d\t%s\n", g_ch[i].file, g_ch[i].name, g_ch[i].master, g_ch[i].on,
                                                       g_ch[i].bad ? "bad" : atomic_load(&g_ch[i].fails) ? "fail" : "ok");
    pthread_mutex_unlock(&g_mu); return s;
}
static void *cheat_thread(void *arg) {
    (void)arg; int paused = 0;
    for (;;) {
        struct timespec ts = { 0, 1000000000L / 12 }; nanosleep(&ts, NULL);
        if (!atomic_load(&g_enabled)) continue;   // (master codes alone set nothing up worth running)
        uint64_t as, asn, h, hn, al, aln; dm_guest_regions(&as, &asn, &h, &hn, &al, &aln); if (!hn) continue;   // (the game has not opened its heap yet)
        if (dm_ldn_in_session()) { if (!paused++) LOG("[cheats] paused: local play has a session"); continue; }
        if (paused) { paused = 0; LOG("[cheats] running again"); }
        pthread_mutex_lock(&g_mu);
        static uint32_t prog[MAX_CHEATS * 64]; static cheat *owner[MAX_CHEATS + 1]; static int start[MAX_CHEATS + 1]; int n = 0, k = 0;
        for (int pass = 0; pass < 2; pass++) for (int i = 0; i < g_nch; i++) {
            cheat *c = &g_ch[i]; if (c->bad || !c->on || c->master != !pass || n + c->nop > (int)(sizeof prog / sizeof *prog)) continue;
            owner[k] = c; start[k] = n; k++; memcpy(prog + n, c->op, sizeof *prog * (size_t)c->nop); n += c->nop;
        }
        owner[k] = NULL;
        if (n) run(prog, n, owner, start);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}
void dm_cheats_start(void) {
#if defined(__linux__)
    const char *dir = getenv("DM_CHEATS_DIR"); if (!dir || !*dir) return;
    dm_cheats_reload();
    pthread_t th; if (!pthread_create(&th, NULL, cheat_thread, NULL)) pthread_detach(th);
#endif
}
