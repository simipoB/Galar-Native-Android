#pragma once

static int startup_gate_has_romfs(int romfs_exists) {
    return romfs_exists != 0;
}

static int startup_gate_thread_started(int pthread_result) {
    return pthread_result == 0;
}
