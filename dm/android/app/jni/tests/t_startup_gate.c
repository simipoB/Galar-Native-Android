#include "../startup_gate.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "ok" : "FAIL", s); } while (0)

int main(void) {
    CHECK(!startup_gate_has_romfs(0), "missing RomFS fails closed");
    CHECK(startup_gate_has_romfs(1), "present RomFS permits startup");
    CHECK(!startup_gate_thread_started(11), "host-thread creation failure fails closed");
    CHECK(startup_gate_thread_started(0), "successful host-thread creation permits startup");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
