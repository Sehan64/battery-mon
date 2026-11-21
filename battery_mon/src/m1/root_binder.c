/*
 * root_binder.c - root-only method: direct /dev/binder & /dev/hwbinder ioctls.
 *
 * 1. Tries AIDL Health HAL (android.hardware.health.IHealth/default) on /dev/binder.
 * 2. Falls back to HIDL Health HAL (android.hardware.health@2.1 / @2.0) on /dev/hwbinder.
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
#define DUMP_FLAG_PRIORITY_ALL  0x0f

#define HEALTH_PREFIX           "android.hardware.health.IHealth/"
#define HEALTH_SERVICE_DEFAULT  "android.hardware.health.IHealth/default"
#define IFACE_IHEALTH_AIDL      "android.hardware.health.IHealth"
#define IFACE_HEALTH_CB_AIDL    "android.hardware.health.IHealthInfoCallback"
#define TXN_HEALTH_REG_CB       1
#define TXN_HEALTH_UNREG_CB     2
#define TXN_HEALTH_UPDATE       3
#define TXN_CB_HEALTH_CHANGED   1

#define IFACE_HW_SM             "android.hidl.manager@1.0::IServiceManager"
#define TXN_HW_SM_GET           1 /* get(string fqName, string name) */

#define IFACE_HW_HEALTH_2_1     "android.hardware.health@2.1::IHealth"
#define IFACE_HW_HEALTH_2_0     "android.hardware.health@2.0::IHealth"
#define IFACE_HW_CB_2_1         "android.hardware.health@2.1::IHealthInfoCallback"
#define IFACE_HW_CB_2_0         "android.hardware.health@2.0::IHealthInfoCallback"
#define IFACE_HW_IBASE          "android.hidl.base@1.0::IBase"
#define TXN_HW_CB_CHANGED_2_0   1
#define TXN_HW_CB_CHANGED_2_1   2
#define TXN_HW_INTERFACE_CHAIN  256067662U
#define TXN_HW_INTERFACE_DESC   256136003U

static uint8_t g_cb_node_tag = 1;

struct health_state {
    int is_hwbinder;
    uint32_t health_handle;
    int has_health_handle;
    int cb_registered;
    char hidl_health_iface[64];
};

static void close_health_binder(struct bm_binder_ctx *b, struct health_state *hs) {
    if (b->fd >= 0) {
        if (hs->has_health_handle)
            bm_binder_release_handle(b, hs->health_handle);
        bm_binder_close(b);
    }
    hs->has_health_handle = 0;
    hs->cb_registered = 0;
    hs->health_handle = 0;
}

static void handle_aidl_health_changed(const struct binder_transaction_data *txn) {
    struct bm_parcel_reader pr;
    bm_pr_init(&pr, txn);

    char iface[128] = {0};
    if (bm_pr_read_aidl_token(&pr, iface, sizeof(iface)) < 0 ||
        strcmp(iface, IFACE_HEALTH_CB_AIDL) != 0)
        return;

    int32_t non_null = 0;
    if (bm_pr_read_i32(&pr, &non_null) < 0 || non_null == 0)
        return;

    /* Parcelable is size-prefixed: i32 size (including itself), then fields. */
    int32_t info_size = 0;
    if (bm_pr_read_i32(&pr, &info_size) < 0 || info_size < 4)
        return;
    size_t end_pos = pr.pos - 4 + (size_t)info_size;
    if (end_pos > pr.size)
        end_pos = pr.size;

    int ac = 0, usb = 0, wireless = 0, dock = 0, present = -1;
    int32_t max_curr = 0, max_volt = 0, b_status = 0, b_health = 0;
    int32_t level = -1, voltage_mv = -1, temp_deci_c = -1;

    if (pr.pos < end_pos) bm_pr_read_bool(&pr, &ac);
    if (pr.pos < end_pos) bm_pr_read_bool(&pr, &usb);
    if (pr.pos < end_pos) bm_pr_read_bool(&pr, &wireless);
    if (pr.pos < end_pos) bm_pr_read_bool(&pr, &dock);
    if (pr.pos < end_pos) bm_pr_read_i32(&pr, &max_curr);
    if (pr.pos < end_pos) bm_pr_read_i32(&pr, &max_volt);
    if (pr.pos < end_pos) bm_pr_read_i32(&pr, &b_status);
    if (pr.pos < end_pos) bm_pr_read_i32(&pr, &b_health);
    if (pr.pos < end_pos) bm_pr_read_bool(&pr, &present);
    if (pr.pos < end_pos) bm_pr_read_i32(&pr, &level);
    if (pr.pos < end_pos) bm_pr_read_i32(&pr, &voltage_mv);
    if (pr.pos < end_pos) bm_pr_read_i32(&pr, &temp_deci_c);

    bm_print_event(level, b_status, b_health, ac, usb, wireless, dock,
                   present, voltage_mv, temp_deci_c);
}

/* android.hardware.health@1.0::HealthInfo layout (prefix of @2.0 and @2.1 HealthInfo) */
struct hidl_health_info_1_0 {
    uint8_t chargerAcOnline;
    uint8_t chargerUsbOnline;
    uint8_t chargerWirelessOnline;
    uint8_t pad0;
    int32_t maxChargingCurrent;
    int32_t maxChargingVoltage;
    int32_t batteryStatus;
    int32_t batteryHealth;
    uint8_t batteryPresent;
    uint8_t pad1[3];
    int32_t batteryLevel;
    int32_t batteryVoltage;
    int32_t batteryTemperature;
    int32_t batteryCurrent;
    int32_t batteryCycleCount;
    int32_t batteryFullCharge;
    int32_t batteryChargeCounter;
};

static void handle_hidl_health_changed(const struct binder_transaction_data *txn) {
    struct bm_parcel_reader pr;
    bm_pr_init(&pr, txn);

    char iface[128] = {0};
    if (bm_pr_read_cstring(&pr, iface, sizeof(iface)) < 0)
        return;
    if (strcmp(iface, IFACE_HW_CB_2_0) != 0 && strcmp(iface, IFACE_HW_CB_2_1) != 0)
        return;

    if (pr.offs_count < 1)
        return;

    size_t obj_off = (size_t)pr.offsets[0];
    if (obj_off > pr.size ||
        sizeof(struct binder_buffer_object) > pr.size - obj_off)
        return;

    struct binder_buffer_object bbo;
    memcpy(&bbo, pr.data + obj_off, sizeof(bbo));
    if (bbo.hdr.type != BINDER_TYPE_PTR || bbo.buffer == 0 ||
        bbo.length < sizeof(struct hidl_health_info_1_0))
        return;

    const struct hidl_health_info_1_0 *hi =
        (const struct hidl_health_info_1_0 *)(uintptr_t)bbo.buffer;

    bm_print_event(hi->batteryLevel, hi->batteryStatus, hi->batteryHealth,
                   hi->chargerAcOnline != 0, hi->chargerUsbOnline != 0,
                   hi->chargerWirelessOnline != 0, 0,
                   hi->batteryPresent != 0, hi->batteryVoltage,
                   hi->batteryTemperature);
}

static void handle_hidl_interface_chain(struct bm_binder_ctx *b) {
    static const char *const chain[3] = {
        IFACE_HW_CB_2_1,
        IFACE_HW_CB_2_0,
        IFACE_HW_IBASE,
    };

    struct bm_hidl_wire_hdr vec_hdr;
    memset(&vec_hdr, 0, sizeof(vec_hdr));
    vec_hdr.size = 3;
    vec_hdr.owns_buffer = 0;

    struct bm_hidl_wire_hdr str_arr[3];
    for (size_t i = 0; i < 3; i++) {
        memset(&str_arr[i], 0, sizeof(str_arr[i]));
        str_arr[i].buffer = (uintptr_t)chain[i];
        str_arr[i].size = (uint32_t)strlen(chain[i]);
        str_arr[i].owns_buffer = 0;
    }
    vec_hdr.buffer = (uintptr_t)str_arr;

    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    bm_pw_write_i32(&pw, 0); /* Status::ok() */

    size_t vec_idx = 0, arr_idx = 0;
    bm_pw_write_buffer_obj(&pw, &vec_hdr, sizeof(vec_hdr), 0, 0, 0, &vec_idx);
    bm_pw_write_buffer_obj(&pw, str_arr, sizeof(str_arr), 1, vec_idx, 0, &arr_idx);
    for (size_t i = 0; i < 3; i++) {
        bm_pw_write_buffer_obj(&pw, chain[i], strlen(chain[i]) + 1,
                               1, arr_idx, i * sizeof(struct bm_hidl_wire_hdr), NULL);
    }

    bm_binder_send_reply_parcel(b, &pw);
}

static void health_dispatch(struct bm_binder_ctx *b, void *userdata,
                            const struct binder_transaction_data *txn) {
    struct health_state *hs = userdata;

    if (txn->target.ptr == (binder_uintptr_t)&g_cb_node_tag) {
        if (!hs->is_hwbinder) {
            if (txn->code == bm_txn.interface_transaction && !(txn->flags & TF_ONE_WAY)) {
                /* The health service verifies our callback's descriptor before
                 * accepting it (BpBinder::getInterfaceDescriptor). */
                struct bm_parcel_writer pw;
                bm_pw_init(&pw);
                bm_pw_write_string16_ascii(&pw, IFACE_HEALTH_CB_AIDL);
                bm_binder_send_reply_parcel(b, &pw);
            } else {
                if (txn->code == TXN_CB_HEALTH_CHANGED)
                    handle_aidl_health_changed(txn);
                if (!(txn->flags & TF_ONE_WAY))
                    bm_binder_send_empty_reply(b, 0);
            }
        } else {
            if (txn->code == TXN_HW_CB_CHANGED_2_0 || txn->code == TXN_HW_CB_CHANGED_2_1) {
                handle_hidl_health_changed(txn);
            } else if (txn->code == TXN_HW_INTERFACE_CHAIN && !(txn->flags & TF_ONE_WAY)) {
                handle_hidl_interface_chain(b);
            } else if (txn->code == TXN_HW_INTERFACE_DESC && !(txn->flags & TF_ONE_WAY)) {
                struct bm_parcel_writer pw;
                struct bm_hidl_wire_hdr shdr;
                bm_pw_init(&pw);
                bm_pw_write_i32(&pw, 0);
                bm_pw_write_hidl_string(&pw, &shdr, IFACE_HW_CB_2_1);
                bm_binder_send_reply_parcel(b, &pw);
            } else if (!(txn->flags & TF_ONE_WAY)) {
                bm_binder_send_empty_reply(b, 0);
            }
        }
    } else if (!(txn->flags & TF_ONE_WAY)) {
        bm_binder_send_empty_reply(b, 0);
    }

    bm_binder_free_buffer(b, txn->data.ptr.buffer);
}

static int aidl_lookup_one(struct bm_binder_ctx *b, const char *name,
                           uint32_t sm_code, uint32_t *out_handle) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_SERVICE_MANAGER) < 0 ||
        bm_pw_write_string16_ascii(&pw, name) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, 0, sm_code, &pw, &rep);
    if (rc < 0) {
        fprintf(stderr, "[aidl] servicemanager txn %u(%s) rc=%d\n", sm_code, name, rc);
        return rc;
    }

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);

    int32_t ex = -1;
    int is_null = 1;
    uint32_t handle = 0;
    if (bm_pr_read_i32(&pr, &ex) < 0 || ex != 0) {
        fprintf(stderr, "[aidl] servicemanager(%s) exception %d\n", name, ex);
        bm_binder_free_buffer(b, rep.data.ptr.buffer);
        return -EACCES;
    }
    if (bm_pr_read_binder_handle(&pr, 1, &handle, &is_null) < 0 || is_null) {
        bm_binder_free_buffer(b, rep.data.ptr.buffer);
        return -ENOENT;
    }

    /* The reply buffer owns the ref; acquire before freeing or the handle dies. */
    bm_binder_acquire_handle(b, handle);
    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    *out_handle = handle;
    return 0;
}

static int aidl_find_health_via_list(struct bm_binder_ctx *b, char *found_name,
                                     size_t cap) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_SERVICE_MANAGER) < 0 ||
        bm_pw_write_i32(&pw, DUMP_FLAG_PRIORITY_ALL) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, 0, bm_txn.sm_list_services, &pw, &rep);
    if (rc < 0)
        return rc;

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);

    int32_t ex = -1, count = -1;
    if (bm_pr_read_i32(&pr, &ex) < 0 || ex != 0 ||
        bm_pr_read_i32(&pr, &count) < 0) {
        fprintf(stderr, "[aidl] listServices denied/failed (ex=%d)\n", ex);
        bm_binder_free_buffer(b, rep.data.ptr.buffer);
        return -EACCES;
    }

    int found = 0;
    for (int32_t i = 0; i < count; i++) {
        char svc[192] = {0};
        if (bm_pr_read_string16_ascii(&pr, svc, sizeof(svc)) < 0)
            break;
        if (strncmp(svc, HEALTH_PREFIX, strlen(HEALTH_PREFIX)) == 0) {
            snprintf(found_name, cap, "%s", svc);
            found = 1;
            break;
        }
    }

    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    return found ? 0 : -ENOENT;
}

static int aidl_register_cb(struct bm_binder_ctx *b, struct health_state *hs,
                            uint32_t txn_code) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_IHEALTH_AIDL) < 0 ||
        bm_pw_write_local_binder(&pw, &g_cb_node_tag, 1, BM_STABILITY_VINTF) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, hs->health_handle, txn_code, &pw, &rep);
    if (rc < 0) {
        fprintf(stderr, "[aidl] transact txn=%u rc=%d\n", txn_code, rc);
        return rc;
    }

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);
    int32_t ex = -1;
    int ok = (bm_pr_read_i32(&pr, &ex) == 0 && ex == 0);
    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    if (!ok) {
        fprintf(stderr, "[aidl] IHealth txn %u failed (ex=%d)\n", txn_code, ex);
        return -EPERM;
    }
    return 0;
}

static int aidl_update(struct bm_binder_ctx *b, struct health_state *hs) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, IFACE_IHEALTH_AIDL) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, hs->health_handle, TXN_HEALTH_UPDATE, &pw, &rep);
    if (rc < 0)
        return rc;

    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    return 0;
}

static int setup_aidl_health(struct bm_binder_ctx *b, struct health_state *hs) {
    if (bm_binder_open(b, "/dev/binder") < 0)
        return -1;
    hs->is_hwbinder = 0;

    char svc_name[192] = HEALTH_SERVICE_DEFAULT;
    int rc0 = aidl_lookup_one(b, svc_name, bm_txn.sm_check_service, &hs->health_handle);
    if (rc0 < 0) {
        int rcl = aidl_find_health_via_list(b, svc_name, sizeof(svc_name));
        if (rcl < 0 ||
            aidl_lookup_one(b, svc_name, bm_txn.sm_check_service, &hs->health_handle) < 0) {
            fprintf(stderr, "[info] AIDL IHealth unavailable on /dev/binder; trying /dev/hwbinder...\n");
            close_health_binder(b, hs);
            return -1;
        }
    }

    hs->has_health_handle = 1;

    (void)aidl_update(b, hs);

    if (aidl_register_cb(b, hs, TXN_HEALTH_REG_CB) < 0) {
        close_health_binder(b, hs);
        return -1;
    }
    hs->cb_registered = 1;
    fprintf(stderr, "[info] monitoring via AIDL %s (/dev/binder)\n", svc_name);
    return 0;
}

static int hidl_lookup_service(struct bm_binder_ctx *b, const char *fq_name,
                               const char *inst_name, uint32_t *out_handle) {
    struct bm_hidl_wire_hdr fq_hdr, inst_hdr;
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_cstring(&pw, IFACE_HW_SM) < 0 ||
        bm_pw_write_hidl_string(&pw, &fq_hdr, fq_name) < 0 ||
        bm_pw_write_hidl_string(&pw, &inst_hdr, inst_name) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, 0 /* hwservicemanager */, TXN_HW_SM_GET, &pw, &rep);
    if (rc < 0) {
        fprintf(stderr, "[hidl] hwservicemanager get(%s/default) rc=%d\n", fq_name, rc);
        return rc;
    }

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);

    int32_t st = -1;
    int is_null = 1;
    uint32_t handle = 0;
    if (bm_pr_read_i32(&pr, &st) < 0 || st != 0) {
        fprintf(stderr, "[hidl] hwservicemanager get(%s/default) st=%d\n", fq_name, st);
        bm_binder_free_buffer(b, rep.data.ptr.buffer);
        return -EACCES;
    }
    if (bm_pr_read_binder_handle(&pr, 0, &handle, &is_null) < 0 || is_null) {
        bm_binder_free_buffer(b, rep.data.ptr.buffer);
        return -ENOENT;
    }

    bm_binder_acquire_handle(b, handle);
    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    *out_handle = handle;
    return 0;
}

static int hidl_register_cb(struct bm_binder_ctx *b, struct health_state *hs,
                            uint32_t txn_code) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_cstring(&pw, hs->hidl_health_iface) < 0 ||
        bm_pw_write_local_binder(&pw, &g_cb_node_tag, 0, 0) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, hs->health_handle, txn_code, &pw, &rep);
    if (rc < 0) {
        fprintf(stderr, "[hidl] transact txn=%u rc=%d\n", txn_code, rc);
        return rc;
    }

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);
    int32_t st = -1, res = -1;
    int ok = (bm_pr_read_i32(&pr, &st) == 0 && st == 0 &&
              bm_pr_read_i32(&pr, &res) == 0 && res == 0);
    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    if (!ok) {
        fprintf(stderr, "[hidl] IHealth txn %u failed (st=%d res=%d)\n", txn_code, st, res);
        return -EPERM;
    }
    return 0;
}

static int hidl_update(struct bm_binder_ctx *b, struct health_state *hs) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_cstring(&pw, hs->hidl_health_iface) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(b, hs->health_handle, TXN_HEALTH_UPDATE, &pw, &rep);
    if (rc < 0)
        return rc;

    bm_binder_free_buffer(b, rep.data.ptr.buffer);
    return 0;
}

static int setup_hidl_health(struct bm_binder_ctx *b, struct health_state *hs) {
    if (bm_binder_open(b, "/dev/hwbinder") < 0)
        return -1;
    hs->is_hwbinder = 1;

    const char *ifaces[2] = { IFACE_HW_HEALTH_2_1, IFACE_HW_HEALTH_2_0 };
    int found = 0;
    for (size_t i = 0; i < 2; i++) {
        if (hidl_lookup_service(b, ifaces[i], "default", &hs->health_handle) == 0) {
            snprintf(hs->hidl_health_iface, sizeof(hs->hidl_health_iface),
                     "%s", ifaces[i]);
            found = 1;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "[error] neither AIDL nor HIDL IHealth found (check SELinux dmesg avc)\n");
        close_health_binder(b, hs);
        return -1;
    }

    hs->has_health_handle = 1;

    if (hidl_register_cb(b, hs, TXN_HEALTH_REG_CB) < 0) {
        close_health_binder(b, hs);
        return -1;
    }
    hs->cb_registered = 1;
    fprintf(stderr, "[info] monitoring via HIDL %s/default (/dev/hwbinder)\n",
            hs->hidl_health_iface);
    (void)hidl_update(b, hs);
    return 0;
}

int bm_method_root_binder(void) {
    struct bm_binder_ctx b;
    struct health_state hs;
    memset(&b, 0, sizeof(b));
    memset(&hs, 0, sizeof(hs));
    b.fd = -1;
    b.dispatch = health_dispatch;
    b.userdata = &hs;

    if (setup_aidl_health(&b, &hs) < 0) {
        if (setup_hidl_health(&b, &hs) < 0)
            return 1;
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

    if (hs.cb_registered) {
        if (!hs.is_hwbinder)
            (void)aidl_register_cb(&b, &hs, TXN_HEALTH_UNREG_CB);
        else
            (void)hidl_register_cb(&b, &hs, TXN_HEALTH_UNREG_CB);
    }
    close_health_binder(&b, &hs);
    return failed;
}
