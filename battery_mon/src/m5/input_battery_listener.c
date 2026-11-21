/*
 * input_battery_listener.c - battery state of input devices (styli, BT peripherals)
 * via the input service: register IInputDeviceBatteryListener and print changes.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <linux/android/binder.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bm_binder.h"
#include "bm_common.h"
#include "bm_txn_cache.h"

#define IFACE_SERVICE_MANAGER   "android.os.IServiceManager"

#define IFACE_INPUT             "android.hardware.input.IInputManager"
#define IFACE_INPUT_BATTERY_CB  "android.hardware.input.IInputDeviceBatteryListener"
#define TXN_CB_BATTERY_CHANGED  1

#define INPUT_SERVICE           "input"
#define MAX_INPUT_DEVICES       64

static uint8_t g_cb_node_tag = 1;

struct dev_state {
    int id;
    int present;
    int status;
    float capacity;
};

static struct dev_state g_devs[MAX_INPUT_DEVICES];
static int g_ndevs;

static void report_battery_state(int device_id, int present, int status,
                                 float capacity) {
    if (!(capacity >= 0.0f))
        capacity = -1.0f; /* NaN and negative sentinels mean "unknown" */

    struct dev_state *slot = NULL;
    for (int i = 0; i < g_ndevs; i++) {
        if (g_devs[i].id == device_id) {
            slot = &g_devs[i];
            break;
        }
    }
    if (slot == NULL) {
        if (g_ndevs >= MAX_INPUT_DEVICES)
            return;
        slot = &g_devs[g_ndevs++];
        slot->id = device_id;
    } else if (slot->present == present && slot->status == status &&
               slot->capacity == capacity) {
        return;
    }

    slot->present = present;
    slot->status = status;
    slot->capacity = capacity;

    if (!present) {
        printf("input-device=%d no battery\n", device_id);
    } else if (capacity >= 0.0f) {
        printf("input-device=%d battery=%.0f%% status=%s\n",
               device_id, capacity, bm_status_str(status));
    } else {
        printf("input-device=%d battery=unknown status=%s\n",
               device_id, bm_status_str(status));
    }
    fflush(stdout);
}

static void handle_battery_callback(const struct binder_transaction_data *txn) {
    struct bm_parcel_reader pr;
    bm_pr_init(&pr, txn);

    char iface[128] = {0};
    if (bm_pr_read_aidl_token(&pr, iface, sizeof(iface)) < 0 ||
        strcmp(iface, IFACE_INPUT_BATTERY_CB) != 0)
        return;

    int32_t object_present = 0;
    if (bm_pr_read_i32(&pr, &object_present) < 0 || object_present == 0)
        return;

    int32_t size = 0, device_id = 0, is_present = 0, status = 0;
    int64_t update_time = 0;
    float capacity = 0.0f;
    if (bm_pr_read_i32(&pr, &size) < 0 || size < 28) return;
    if (bm_pr_read_i32(&pr, &device_id) < 0) return;
    if (bm_pr_read_i64(&pr, &update_time) < 0) return;
    if (bm_pr_read_i32(&pr, &is_present) < 0) return;
    if (bm_pr_read_i32(&pr, &status) < 0) return;
    if (bm_pr_read_f32(&pr, &capacity) < 0) return;

    report_battery_state(device_id, is_present, status, capacity);
}

static void input_dispatch(struct bm_binder_ctx *b, void *userdata,
                           const struct binder_transaction_data *txn) {
    (void)userdata;

    if (txn->target.ptr == (binder_uintptr_t)&g_cb_node_tag) {
        if (txn->code == bm_txn.interface_transaction && !(txn->flags & TF_ONE_WAY)) {
            struct bm_parcel_writer pw;
            bm_pw_init(&pw);
            bm_pw_write_string16_ascii(&pw, IFACE_INPUT_BATTERY_CB);
            bm_binder_send_reply_parcel(b, &pw);
        } else {
            if (txn->code == TXN_CB_BATTERY_CHANGED)
                handle_battery_callback(txn);
            if (!(txn->flags & TF_ONE_WAY))
                bm_binder_send_empty_reply(b, 0);
        }
    } else if (!(txn->flags & TF_ONE_WAY)) {
        bm_binder_send_empty_reply(b, 0);
    }

    bm_binder_free_buffer(b, txn->data.ptr.buffer);
}

static int lookup_service(struct bm_binder_ctx *b, const char *name,
                          uint32_t *out_handle) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_SERVICE_MANAGER) < 0 ||
        bm_pw_write_string16_ascii(&pw, name) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, 0, bm_txn.sm_check_service, &pw, &rep);
    if (rc < 0)
        return rc;

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);

    int32_t ex = -1;
    int is_null = 1;
    uint32_t handle = 0;
    if (bm_pr_read_i32(&pr, &ex) < 0 || ex != 0 ||
        bm_pr_read_binder_handle(&pr, 1, &handle, &is_null) < 0 || is_null) {
        bm_binder_free_buffer(b, rep.data.ptr.buffer);
        return -ENOENT;
    }

    bm_binder_acquire_handle(b, handle);
    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    *out_handle = handle;
    return 0;
}

static int get_input_device_ids(struct bm_binder_ctx *b, uint32_t input_handle,
                                int *ids, size_t cap, size_t *out_n) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_INPUT) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, input_handle, bm_txn.input_get_ids, &pw, &rep);
    if (rc < 0)
        return rc;

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);

    int32_t ex = -1, count = -1;
    if (bm_pr_read_i32(&pr, &ex) < 0 || ex != 0 ||
        bm_pr_read_i32(&pr, &count) < 0) {
        bm_binder_free_buffer(b, rep.data.ptr.buffer);
        return -EACCES;
    }

    size_t n = 0;
    for (int32_t i = 0; i < count && n < cap; i++) {
        int32_t id = 0;
        if (bm_pr_read_i32(&pr, &id) < 0)
            break;
        ids[n++] = id;
    }

    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    *out_n = n;
    return 0;
}

static int register_battery_listener(struct bm_binder_ctx *b,
                                     uint32_t input_handle, int device_id) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_INPUT) < 0 ||
        bm_pw_write_i32(&pw, device_id) < 0 ||
        bm_pw_write_local_binder(&pw, &g_cb_node_tag, 1, BM_STABILITY_SYSTEM) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, input_handle, bm_txn.input_reg_battery, &pw, &rep);
    if (rc < 0)
        return rc;

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);
    int32_t ex = 0;
    char msg[160] = {0};
    int ok = (bm_pr_read_i32(&pr, &ex) == 0);
    if (ok && ex != 0)
        (void)bm_pr_read_string16_ascii(&pr, msg, sizeof(msg));
    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    if (!ok)
        return -EIO;
    if (ex != 0) {
        fprintf(stderr, "[warn] registerBatteryListener(dev=%d): %s (%d)\n",
                device_id, msg[0] ? msg : "exception", ex);
        return -EPERM;
    }
    return 0;
}

int bm_method_input_battery_listener(void) {
    struct bm_binder_ctx b;
    memset(&b, 0, sizeof(b));
    b.fd = -1;
    b.dispatch = input_dispatch;

    if (bm_binder_open(&b, "/dev/binder") < 0)
        return -1;

    uint32_t input_handle = 0;
    if (lookup_service(&b, INPUT_SERVICE, &input_handle) < 0) {
        fprintf(stderr, "[info] input service (%s) unavailable\n", INPUT_SERVICE);
        bm_binder_close(&b);
        return -1;
    }

    int ids[MAX_INPUT_DEVICES];
    size_t n = 0;
    if (get_input_device_ids(&b, input_handle, ids, MAX_INPUT_DEVICES, &n) < 0) {
        fprintf(stderr, "[info] getInputDeviceIds failed\n");
        bm_binder_release_handle(&b, input_handle);
        bm_binder_close(&b);
        return -1;
    }

    fprintf(stderr, "[info] monitoring input device batteries via %s (%zu devices)\n",
            IFACE_INPUT, n);

    int registered = 0;
    for (size_t i = 0; i < n; i++) {
        if (register_battery_listener(&b, input_handle, ids[i]) == 0)
            registered++;
    }
    fprintf(stderr, "[info] battery listeners registered: %d/%zu\n", registered, n);
    if (registered == 0) {
        bm_binder_release_handle(&b, input_handle);
        bm_binder_close(&b);
        return -1;
    }

    int failed = 0;
    uint8_t rbuf[4096];
    while (bm_running) {
        size_t rlen = 0;
        int rc = bm_binder_ioctl_wr(&b, NULL, 0, rbuf, sizeof(rbuf), &rlen);
        if (rc == -EINTR)
            continue;
        if (rc < 0) {
            fprintf(stderr, "BINDER_WRITE_READ failed: %s\n", strerror(-rc));
            failed = 1;
            break;
        }
        if (bm_binder_process_br_stream(&b, rbuf, rlen, NULL) < 0) {
            failed = 1;
            break;
        }
    }

    bm_binder_release_handle(&b, input_handle);
    bm_binder_close(&b);
    return failed;
}
