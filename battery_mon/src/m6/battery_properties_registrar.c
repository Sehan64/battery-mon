/*
 * battery_properties_registrar.c - binder method: poll IBatteryPropertiesRegistrar.
 *
 * The "batteryproperties" service is reachable from apps and shell without root.
 * getProperty() exposes capacity, status, current and charge counter; ids 7+
 * need BATTERY_STATS, so this method sticks to the unprivileged ids.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bm_binder.h"
#include "bm_common.h"
#include "bm_txn_cache.h"

#define IFACE_BPR   "android.os.IBatteryPropertiesRegistrar"
#define SERVICE_BPR "batteryproperties"

enum {
    PROP_CHARGE_COUNTER = 1,
    PROP_CURRENT_NOW = 2,
    PROP_CURRENT_AVERAGE = 3,
    PROP_CAPACITY = 4,
    PROP_ENERGY_COUNTER = 5,
    PROP_STATUS = 6,
};

struct bpr_state {
    int have_capacity, have_status, have_charge_counter;
    int have_current, have_current_avg, have_energy;
    int64_t capacity, status, charge_counter, current, current_avg, energy;
};

static int get_property(struct bm_binder_ctx *b, uint32_t handle, int id,
                        int64_t *out) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_BPR) < 0 ||
        bm_pw_write_i32(&pw, id) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, handle, bm_txn.bpr_get_property, &pw, &rep);
    if (rc < 0)
        return rc;

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);
    int32_t ex = -1, result = -1, present = 0;
    int64_t value = 0;
    int ok = bm_pr_read_i32(&pr, &ex) == 0 && ex == 0 &&
             bm_pr_read_i32(&pr, &result) == 0 && result == 0 &&
             bm_pr_read_i32(&pr, &present) == 0 && present != 0 &&
             bm_pr_read_i64(&pr, &value) == 0 &&
             value != INT64_MIN;
    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    if (!ok)
        return -ENOENT;
    *out = value;
    return 0;
}

static void read_state(struct bm_binder_ctx *b, uint32_t handle,
                       struct bpr_state *st) {
    memset(st, 0, sizeof(*st));
    st->have_capacity =
        get_property(b, handle, PROP_CAPACITY, &st->capacity) == 0;
    st->have_status =
        get_property(b, handle, PROP_STATUS, &st->status) == 0;
    st->have_charge_counter =
        get_property(b, handle, PROP_CHARGE_COUNTER, &st->charge_counter) == 0;
    st->have_current =
        get_property(b, handle, PROP_CURRENT_NOW, &st->current) == 0;
    st->have_current_avg =
        get_property(b, handle, PROP_CURRENT_AVERAGE, &st->current_avg) == 0;
    st->have_energy =
        get_property(b, handle, PROP_ENERGY_COUNTER, &st->energy) == 0;
}

static void print_state(const struct bpr_state *st) {
    if (st->have_capacity)
        printf("battery=%lld%%", (long long)st->capacity);
    else
        printf("battery=unknown");
    if (st->have_status)
        printf(" status=%s", bm_status_str((int32_t)st->status));
    if (st->have_charge_counter)
        printf(" charge_counter=%llduAh", (long long)st->charge_counter);
    if (st->have_current)
        printf(" current=%llduA", (long long)st->current);
    if (st->have_current_avg)
        printf(" current_avg=%llduA", (long long)st->current_avg);
    if (st->have_energy)
        printf(" energy=%lldnWh", (long long)st->energy);
    printf("\n");
    fflush(stdout);
}

int bm_method_battery_properties_registrar(void) {
    struct bm_binder_ctx b;
    memset(&b, 0, sizeof(b));
    b.fd = -1;

    if (bm_binder_open(&b, "/dev/binder") < 0)
        return -1;

    uint32_t handle = 0;
    if (bm_binder_lookup_service(&b, SERVICE_BPR, &handle) < 0) {
        fprintf(stderr, "[info] %s service unavailable\n", SERVICE_BPR);
        bm_binder_close(&b);
        return -1;
    }

    struct bpr_state cur, prev;
    read_state(&b, handle, &cur);
    if (!cur.have_capacity) {
        fprintf(stderr, "[error] %s getProperty(capacity) failed\n", IFACE_BPR);
        bm_binder_release_handle(&b, handle);
        bm_binder_close(&b);
        return -1;
    }

    fprintf(stderr, "[info] monitoring via AIDL %s (no root)\n", IFACE_BPR);
    print_state(&cur);
    prev = cur;

    int fails = 0;
    while (bm_running) {
        sleep(1);
        if (!bm_running)
            break;
        read_state(&b, handle, &cur);
        if (!cur.have_capacity) {
            if (++fails >= 3) {
                fprintf(stderr, "[error] getProperty(capacity) failed repeatedly\n");
                break;
            }
            continue;
        }
        fails = 0;
        if (memcmp(&prev, &cur, sizeof(cur)) != 0) {
            print_state(&cur);
            prev = cur;
        }
    }

    bm_binder_release_handle(&b, handle);
    bm_binder_close(&b);
    return fails >= 3 ? -1 : 0;
}
