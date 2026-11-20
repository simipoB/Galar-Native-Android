// cheats.c: the Atmosphère cheat VM against a fake guest (a module, a heap), file parsing, toggles and build filtering. Linux (process_vm_readv / writev).
// cc -O1 -I.. -I<dir with game_profile.h> t_cheats.c -o t_cheats -lpthread && ./t_cheats
#include "../cheats.c"
#include <assert.h>
#include <sys/stat.h>

module_t g_mod[MAX_MOD]; int g_nmod;
static uint8_t *g_heap_t; static size_t g_heap_n;
static uint64_t g_keys;
void dm_guest_regions(uint64_t *as, uint64_t *as_n, uint64_t *heap, uint64_t *heap_n, uint64_t *alias, uint64_t *alias_n) {
    *as = (uint64_t)(uintptr_t)g_heap_t; *as_n = g_heap_n; *heap = (uint64_t)(uintptr_t)g_heap_t; *heap_n = g_heap_n; *alias = 0; *alias_n = 0;
}
uint64_t dm_input_held(void) { return g_keys; }
int dm_ldn_in_session(void) { return 0; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void go(const uint32_t *p, int n) { cheat c = { .name = "t" }; cheat *own[2] = { &c, NULL }; int st[2] = { 0, 0 }; run(p, n, own, st); }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int main(void) {
    long pg = sysconf(_SC_PAGESIZE);
    // main: one code page (read + execute) then data; the heap: 1 MB
    uint8_t *m = mmap(NULL, (size_t)pg * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(m != MAP_FAILED);
    mprotect(m, (size_t)pg, PROT_READ | PROT_EXEC);
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = m; g_mod[0].size = (size_t)pg * 4; g_mod[0].text_end = (uint64_t)pg; g_mod[0].data_off = (uint64_t)pg * 2;
    strcpy(g_mod[0].build_id, "0123456789ABCDEF0123456789ABCDEF");
    g_heap_n = 1 << 20; g_heap_t = mmap(NULL, g_heap_n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint32_t D = (uint32_t)pg * 2;   // main's data

    // 0: store 4 bytes at main+D, 8 bytes at heap+0x10
    { uint32_t p[] = { 0x04000000, D, 0x00000063, 0x08100000, 0x10, 0x11223344, 0x55667788 }; go(p, 7);
      CHECK(u32(m + D) == 0x63); CHECK(u64(g_heap_t + 0x10) == 0x1122334455667788ull); }
    // pointer chain: main+D+8 holds heap+0x100; 5 load it, 7 add 0x20, 6 store 0x3e7 there (4 bytes)
    { uint64_t ptr = (uint64_t)(uintptr_t)g_heap_t + 0x100; memcpy(m + D + 8, &ptr, 8);
      uint32_t p[] = { 0x58000000, D + 8, 0x78000000, 0x20, 0x64000000, 0, 0x3e7 }; go(p, 7); CHECK(u32(g_heap_t + 0x120) == 0x3e7); }
    // 1 + else: [main+D] == 0x63 → write 1, else write 2
    { uint32_t p[] = { 0x14050000, D, 0x63, 0x04100000, 0x200, 1, 0x21000000, 0x04100000, 0x200, 2, 0x20000000 };
      go(p, 11); CHECK(u32(g_heap_t + 0x200) == 1);
      p[2] = 0x64; go(p, 11); CHECK(u32(g_heap_t + 0x200) == 2); }
    // nested condition inside a skipped block, then code after the end still runs
    { uint32_t p[] = { 0x14050000, D, 0x99, 0x14050000, D, 0x63, 0x04100000, 0x204, 7, 0x20000000, 0x20000000, 0x04100000, 0x208, 9 };
      go(p, 14); CHECK(u32(g_heap_t + 0x204) == 0); CHECK(u32(g_heap_t + 0x208) == 9); }
    // 8: only while ZL+A (0x101) are held
    { uint32_t p[] = { 0x80000101, 0x04100000, 0x300, 5, 0x20000000 };
      g_keys = 0x100; go(p, 5); CHECK(u32(g_heap_t + 0x300) == 0); g_keys = 0x101; go(p, 5); CHECK(u32(g_heap_t + 0x300) == 5); g_keys = 0; }
    // 3 loop: 4 times, 0x400 + reg1 * 4 ← 0xAA (A: store reg 2 at reg 1 + offset, incrementing reg 1)
    { uint32_t p[] = { 0x40010000, (uint32_t)((uint64_t)(uintptr_t)g_heap_t >> 32), (uint32_t)(uintptr_t)g_heap_t + 0x400, 0x40020000, 0, 0xAA,
                       0x30000000, 4, 0xA4211000, 0x31000000 };
      go(p, 10); for (int i = 0; i < 4; i++) CHECK(u32(g_heap_t + 0x400 + 4 * i) == 0xAA); CHECK(u32(g_heap_t + 0x410) == 0); }
    // 9: reg3 = reg2 (0xAA) xor 0xFF; C0 compare reg3 against a value
    { uint32_t p[] = { 0x40020000, 0, 0xAA, 0x94832100, 0xFF, 0xC0453400, 0x55, 0x04100000, 0x500, 1, 0x20000000 }; go(p, 11); CHECK(u32(g_heap_t + 0x500) == 1); }
    // a code write: main+0x10 (read + execute page) is opened, written, closed again
    { uint32_t p[] = { 0x04000000, 0x10, 0xD503201F }; go(p, 3); CHECK(u32(m + 0x10) == 0xD503201F); }
    // a bad pointer: no crash, the write fails
    { uint32_t p[] = { 0x40010000, 0, 0x1234, 0x64010000, 0, 1 }; cheat c = { .name = "bad" }; cheat *own[2] = { &c, NULL }; int st[2] = { 0 };
      run(p, 6, own, st); CHECK(atomic_load(&c.fails) == 1); }
    // unknown opcode → op_len 0
    { uint32_t p[] = { 0xB0000000 }; CHECK(op_len(p, 1) == 0); uint32_t q[] = { 0x04000000, 0 }; CHECK(op_len(q, 2) == 0); }

    // files: this build's file, another build's (ignored), a free name; toggles
    char dir[] = "/tmp/t_cheats_XXXXXX"; assert(mkdtemp(dir)); setenv("DM_CHEATS_DIR", dir, 1);
    char p[512]; FILE *f;
    snprintf(p, sizeof p, "%s/0123456789abcdef.txt", dir); f = fopen(p, "w"); fprintf(f, "{Master}\n04100000 00000600 00000001\n\n[Max Money]\n04100000 00000604 0098967F\n[Bad]\nB0000000\n"); fclose(f);
    snprintf(p, sizeof p, "%s/FFFFFFFFFFFFFFFF.txt", dir); f = fopen(p, "w"); fprintf(f, "[Other build]\n04100000 00000608 00000001\n"); fclose(f);
    snprintf(p, sizeof p, "%s/mine.txt", dir); f = fopen(p, "w"); fprintf(f, "[Shiny Pokémon]\n04100000 0000060C 00000001  // a comment\n"); fclose(f);
    snprintf(p, sizeof p, "%s/toggles.txt", dir); f = fopen(p, "w"); fprintf(f, "[Max Money]\ntrue\n\n[Shiny Pokémon]\nfalse\n"); fclose(f);
    dm_cheats_reload();
    CHECK(g_nch == 4); CHECK(dm_cheats_enabled() == 1);
    char *info = dm_cheats_info(); printf("%s", info);
    CHECK(strstr(info, "0123456789ABCDEF\n") == info); CHECK(strstr(info, "Bad\t0\t0\tbad")); CHECK(strstr(info, "Max Money\t0\t1\tok")); CHECK(strstr(info, "Master\t1\t1\tok")); CHECK(!strstr(info, "Other build")); free(info);
    // one run as the thread does it
    { static uint32_t prog[4096]; cheat *own[8]; int st[8], n = 0, k = 0;
      for (int pass = 0; pass < 2; pass++) for (int i = 0; i < g_nch; i++) { cheat *c = &g_ch[i]; if (c->bad || !c->on || c->master != !pass) continue; own[k] = c; st[k++] = n; memcpy(prog + n, c->op, 4 * (size_t)c->nop); n += c->nop; }
      own[k] = NULL; run(prog, n, own, st); }
    CHECK(u32(g_heap_t + 0x600) == 1); CHECK(u32(g_heap_t + 0x604) == 9999999); CHECK(u32(g_heap_t + 0x608) == 0); CHECK(u32(g_heap_t + 0x60C) == 0);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
