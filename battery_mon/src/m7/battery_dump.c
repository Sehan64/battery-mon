/*
 * battery_dump.c - shell/root method: poll the "battery" service via a direct
 * binder dump() transaction. The service writes its text dump to a pipe; this
 * method parses the standard BatteryService fields from it.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bm_binder.h"
#include "bm_common.h"
#include "bm_txn_cache.h"

#define SERVICE_BATTERY "battery"

static int run_dump(struct bm_binder_ctx *b, uint32_t handle, char *out,
                    size_t cap) {
    int fds[2];
    if (pipe(fds) < 0)
        return -errno;

    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_fd(&pw, fds[1]) < 0 || bm_pw_write_i32(&pw, 0) < 0) {
        close(fds[0]);
        close(fds[1]);
        return -EINVAL;
    }

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, handle, bm_txn.dump_transaction, &pw, &rep);
    close(fds[1]);
    if (rc < 0) {
        close(fds[0]);
        return rc;
    }
    bm_binder_free_buffer(b, rep.data.ptr.buffer);

    size_t used = 0;
    while (used + 1 < cap) {
        struct pollfd pfd;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0)
            break;
        ssize_t n = read(fds[0], out + used, cap - 1 - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    close(fds[0]);
    out[used] = '\0';
    return used > 0 ? 0 : -EIO;
}

static int parse_dump(const char *text, struct bm_state *st) {
    memset(st, 0, sizeof(*st));
    st->status = 1;
    st->health = 1;
    st->present = -1;
    st->voltage_mv = -1;
    st->temp_deci_c = -1;
    st->level = -1;

    const char *line = text;
    while (*line) {
        char key[64] = {0}, val[64] = {0};
        if (sscanf(line, " %63[^:]: %63s", key, val) == 2) {
            if (strcmp(key, "level") == 0)
                st->level = atoi(val);
            else if (strcmp(key, "status") == 0)
                st->status = atoi(val);
            else if (strcmp(key, "health") == 0)
                st->health = atoi(val);
            else if (strcmp(key, "present") == 0)
                st->present = strcmp(val, "true") == 0;
            else if (strcmp(key, "voltage") == 0)
                st->voltage_mv = atoi(val);
            else if (strcmp(key, "temperature") == 0)
                st->temp_deci_c = atoi(val);
            else if (strcmp(key, "AC powered") == 0)
                st->ac = strcmp(val, "true") == 0;
            else if (strcmp(key, "USB powered") == 0)
                st->usb = strcmp(val, "true") == 0;
            else if (strcmp(key, "Wireless powered") == 0)
                st->wireless = strcmp(val, "true") == 0;
            else if (strcmp(key, "Dock powered") == 0)
                st->dock = strcmp(val, "true") == 0;
        }
        const char *eol = strchr(line, '\n');
        if (!eol)
            break;
        line = eol + 1;
    }
    return st->level >= 0 ? 0 : -1;
}

int bm_method_battery_dump(void) {
    struct bm_binder_ctx b;
    memset(&b, 0, sizeof(b));
    b.fd = -1;

    if (bm_binder_open(&b, "/dev/binder") < 0)
        return -1;

    uint32_t handle = 0;
    if (bm_binder_lookup_service(&b, SERVICE_BATTERY, &handle) < 0) {
        fprintf(stderr, "[info] battery service unavailable (run under adb shell or root)\n");
        bm_binder_close(&b);
        return -1;
    }

    char text[8192];
    struct bm_state cur, prev;
    if (run_dump(&b, handle, text, sizeof(text)) < 0 ||
        parse_dump(text, &cur) < 0) {
        fprintf(stderr, "[error] battery dump failed\n");
        bm_binder_release_handle(&b, handle);
        bm_binder_close(&b);
        return -1;
    }

    fprintf(stderr, "[info] monitoring via binder dump() service %s (shell/root)\n",
            SERVICE_BATTERY);
    bm_state_print(&cur);
    prev = cur;

    while (bm_running) {
        sleep(1);
        if (!bm_running)
            break;
        if (run_dump(&b, handle, text, sizeof(text)) < 0 ||
            parse_dump(text, &cur) < 0)
            continue;
        if (memcmp(&prev, &cur, sizeof(cur)) != 0) {
            bm_state_print(&cur);
            prev = cur;
        }
    }

    bm_binder_release_handle(&b, handle);
    bm_binder_close(&b);
    return 0;
}
