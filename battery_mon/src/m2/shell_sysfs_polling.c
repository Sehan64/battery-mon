/*
 * shell_sysfs_polling.c - shell method: poll /sys/class/power_supply.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bm_common.h"

int bm_method_shell_sysfs_polling(void) {
    struct bm_state cur, prev;

    if (bm_sysfs_read_state(&cur) < 0) {
        fprintf(stderr, "[info] battery sysfs unreadable (needs root or shell UID)\n");
        return -1;
    }
    fprintf(stderr, "[info] monitoring via sysfs %s\n", BM_SYSFS_BAT);
    bm_state_print(&cur);
    prev = cur;

    while (bm_running) {
        sleep(1);
        if (bm_sysfs_read_state(&cur) < 0)
            continue;
        if (memcmp(&prev, &cur, sizeof(cur)) != 0) {
            bm_state_print(&cur);
            prev = cur;
        }
    }
    return 0;
}
