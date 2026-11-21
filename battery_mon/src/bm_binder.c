/*
 * bm_binder.c - shared parcel and binder-transaction plumbing.
 *
 * Used by the root_binder (m1) and input_battery_listener (m5) methods so the
 * wire-format code exists once. Every read is bounds-checked before any
 * multiplication or pointer arithmetic.
 */

#define _GNU_SOURCE
#include "bm_binder.h"
#include "bm_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

void bm_pw_init(struct bm_parcel_writer *pw) {
    pw->data_pos = 0;
    pw->offs_count = 0;
}

int bm_pw_write_bytes(struct bm_parcel_writer *pw, const void *src, size_t len) {
    if (len > sizeof(pw->data))
        return -1;
    size_t padded = BM_PAD_SIZE(len);
    if (padded > sizeof(pw->data) - pw->data_pos)
        return -1;
    memcpy(pw->data + pw->data_pos, src, len);
    if (padded > len)
        memset(pw->data + pw->data_pos + len, 0, padded - len);
    pw->data_pos += padded;
    return 0;
}

int bm_pw_write_i32(struct bm_parcel_writer *pw, int32_t v) {
    return bm_pw_write_bytes(pw, &v, sizeof(v));
}

int bm_pw_write_cstring(struct bm_parcel_writer *pw, const char *s) {
    return bm_pw_write_bytes(pw, s, strlen(s) + 1);
}

int bm_pw_write_string16_ascii(struct bm_parcel_writer *pw, const char *s) {
    size_t len = strlen(s);
    /* Check capacity before (len + 1) * 2 so a huge length cannot wrap. */
    if (len + 1 > (sizeof(pw->data) - pw->data_pos) / sizeof(uint16_t))
        return -1;
    if (bm_pw_write_i32(pw, (int32_t)len) < 0)
        return -1;
    size_t byte_len = (len + 1) * sizeof(uint16_t);
    size_t padded = BM_PAD_SIZE(byte_len);
    if (padded > sizeof(pw->data) - pw->data_pos)
        return -1;
    uint16_t *dst = (uint16_t *)(pw->data + pw->data_pos);
    for (size_t i = 0; i < len; i++)
        dst[i] = (uint8_t)s[i];
    dst[len] = 0;
    if (padded > byte_len)
        memset(pw->data + pw->data_pos + byte_len, 0, padded - byte_len);
    pw->data_pos += padded;
    return 0;
}

int bm_pw_write_aidl_token(struct bm_parcel_writer *pw, const char *iface) {
    if (bm_pw_write_i32(pw, BM_STRICT_MODE_NONE) < 0) return -1;
    if (bm_pw_write_i32(pw, BM_UNSET_WORK_SOURCE) < 0) return -1;
    if (bm_pw_write_i32(pw, (int32_t)BM_PARCEL_SYS_HEADER) < 0) return -1;
    return bm_pw_write_string16_ascii(pw, iface);
}

int bm_pw_write_local_binder(struct bm_parcel_writer *pw, void *cookie_ptr,
                             int with_stability, int32_t stability) {
    if (pw->offs_count >= (sizeof(pw->offsets) / sizeof(pw->offsets[0])))
        return -1;
    size_t extra = with_stability ? sizeof(int32_t) : 0;
    if (sizeof(struct flat_binder_object) + extra >
        sizeof(pw->data) - pw->data_pos)
        return -1;

    pw->offsets[pw->offs_count++] = (binder_size_t)pw->data_pos;

    struct flat_binder_object obj;
    memset(&obj, 0, sizeof(obj));
    obj.hdr.type = BINDER_TYPE_BINDER;
    obj.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS;
    obj.binder = (binder_uintptr_t)cookie_ptr;
    obj.cookie = (binder_uintptr_t)cookie_ptr;

    memcpy(pw->data + pw->data_pos, &obj, sizeof(obj));
    pw->data_pos += sizeof(obj);

    if (with_stability)
        return bm_pw_write_i32(pw, stability);
    return 0;
}

int bm_pw_write_buffer_obj(struct bm_parcel_writer *pw, const void *buf, size_t len,
                           int has_parent, size_t parent_idx, size_t parent_off,
                           size_t *out_obj_idx) {
    if (pw->offs_count >= (sizeof(pw->offsets) / sizeof(pw->offsets[0])))
        return -1;
    if (sizeof(struct binder_buffer_object) > sizeof(pw->data) - pw->data_pos)
        return -1;

    size_t idx = pw->offs_count;
    pw->offsets[pw->offs_count++] = (binder_size_t)pw->data_pos;

    struct binder_buffer_object bbo;
    memset(&bbo, 0, sizeof(bbo));
    bbo.hdr.type = BINDER_TYPE_PTR;
    bbo.flags = has_parent ? BINDER_BUFFER_FLAG_HAS_PARENT : 0;
    bbo.buffer = (binder_uintptr_t)buf;
    bbo.length = (binder_size_t)len;
    bbo.parent = (binder_size_t)parent_idx;
    bbo.parent_offset = (binder_size_t)parent_off;

    memcpy(pw->data + pw->data_pos, &bbo, sizeof(bbo));
    pw->data_pos += sizeof(bbo);

    if (out_obj_idx)
        *out_obj_idx = idx;
    return 0;
}

int bm_pw_write_fd(struct bm_parcel_writer *pw, int fd) {
    if (pw->offs_count >= (sizeof(pw->offsets) / sizeof(pw->offsets[0])))
        return -1;
    if (sizeof(struct flat_binder_object) > sizeof(pw->data) - pw->data_pos)
        return -1;

    pw->offsets[pw->offs_count++] = (binder_size_t)pw->data_pos;

    struct flat_binder_object obj;
    memset(&obj, 0, sizeof(obj));
    obj.hdr.type = BINDER_TYPE_FD;
    obj.flags = 0x7f | FLAT_BINDER_FLAG_ACCEPTS_FDS;
    obj.handle = (uint32_t)fd;
    obj.cookie = 1; /* the receiving service takes ownership of its dup */

    memcpy(pw->data + pw->data_pos, &obj, sizeof(obj));
    pw->data_pos += sizeof(obj);
    return 0;
}

int bm_pw_write_hidl_string(struct bm_parcel_writer *pw,
                            struct bm_hidl_wire_hdr *hdr_storage, const char *s) {
    size_t slen = strlen(s);
    memset(hdr_storage, 0, sizeof(*hdr_storage));
    hdr_storage->buffer = (uintptr_t)s;
    hdr_storage->size = (uint32_t)slen;
    hdr_storage->owns_buffer = 0;

    size_t parent_idx = 0;
    if (bm_pw_write_buffer_obj(pw, hdr_storage, sizeof(*hdr_storage),
                               0, 0, 0, &parent_idx) < 0)
        return -1;
    return bm_pw_write_buffer_obj(pw, s, slen + 1, 1, parent_idx, 0, NULL);
}

void bm_pr_init(struct bm_parcel_reader *pr,
                const struct binder_transaction_data *txn) {
    pr->data = (const uint8_t *)(uintptr_t)txn->data.ptr.buffer;
    pr->size = (size_t)txn->data_size;
    pr->pos = 0;
    pr->offsets = (const binder_size_t *)(uintptr_t)txn->data.ptr.offsets;
    pr->offs_count = (size_t)(txn->offsets_size / sizeof(binder_size_t));
}

int bm_pr_read_i32(struct bm_parcel_reader *pr, int32_t *out) {
    if (pr->size - pr->pos < sizeof(int32_t))
        return -1;
    memcpy(out, pr->data + pr->pos, sizeof(int32_t));
    pr->pos += sizeof(int32_t);
    return 0;
}

int bm_pr_read_i64(struct bm_parcel_reader *pr, int64_t *out) {
    if (pr->size - pr->pos < sizeof(int64_t))
        return -1;
    memcpy(out, pr->data + pr->pos, sizeof(int64_t));
    pr->pos += sizeof(int64_t);
    return 0;
}

int bm_pr_read_f32(struct bm_parcel_reader *pr, float *out) {
    if (pr->size - pr->pos < sizeof(float))
        return -1;
    memcpy(out, pr->data + pr->pos, sizeof(float));
    pr->pos += sizeof(float);
    return 0;
}

int bm_pr_read_bool(struct bm_parcel_reader *pr, int *out) {
    int32_t v = 0;
    if (bm_pr_read_i32(pr, &v) < 0)
        return -1;
    *out = (v != 0);
    return 0;
}

int bm_pr_read_string16_ascii(struct bm_parcel_reader *pr, char *ascii_buf,
                              size_t buf_sz) {
    int32_t len = 0;
    if (bm_pr_read_i32(pr, &len) < 0)
        return -1;
    if (len < 0) {
        if (ascii_buf && buf_sz > 0) ascii_buf[0] = '\0';
        return 0;
    }
    size_t remaining = pr->size - pr->pos;
    /* String16 payload is (len + 1) UTF-16 units. Check before multiplying so
     * a hostile length cannot wrap size_t on a 32-bit build. */
    if ((size_t)len + 1 > remaining / sizeof(uint16_t))
        return -1;
    size_t byte_len = ((size_t)len + 1) * sizeof(uint16_t);
    size_t padded = BM_PAD_SIZE(byte_len);
    if (padded > remaining)
        return -1;
    if (ascii_buf && buf_sz > 0) {
        const uint16_t *u16 = (const uint16_t *)(pr->data + pr->pos);
        size_t copy_n = (size_t)len < (buf_sz - 1) ? (size_t)len : (buf_sz - 1);
        for (size_t i = 0; i < copy_n; i++)
            ascii_buf[i] = (u16[i] < 128) ? (char)u16[i] : '?';
        ascii_buf[copy_n] = '\0';
    }
    pr->pos += padded;
    return 0;
}

int bm_pr_read_cstring(struct bm_parcel_reader *pr, char *buf, size_t buf_sz) {
    if (pr->pos >= pr->size)
        return -1;
    const char *s = (const char *)(pr->data + pr->pos);
    size_t avail = pr->size - pr->pos;
    size_t slen = strnlen(s, avail);
    if (slen == avail)
        return -1;
    if (buf && buf_sz > 0) {
        size_t n = slen < (buf_sz - 1) ? slen : (buf_sz - 1);
        memcpy(buf, s, n);
        buf[n] = '\0';
    }
    size_t padded = BM_PAD_SIZE(slen + 1);
    pr->pos += (padded <= avail) ? padded : avail;
    return 0;
}

int bm_pr_read_aidl_token(struct bm_parcel_reader *pr, char *iface_buf,
                          size_t buf_sz) {
    int32_t strict_mode = 0, work_source = 0, header = 0;
    if (bm_pr_read_i32(pr, &strict_mode) < 0) return -1;
    if (bm_pr_read_i32(pr, &work_source) < 0) return -1;
    if (bm_pr_read_i32(pr, &header) < 0) return -1;
    return bm_pr_read_string16_ascii(pr, iface_buf, buf_sz);
}

static int binder_handle_from_obj(const struct flat_binder_object *obj,
                                  uint32_t *out_handle, int *is_null) {
    if (obj->hdr.type == BINDER_TYPE_HANDLE) {
        *out_handle = obj->handle;
        *is_null = 0;
        return 0;
    }
    if (obj->hdr.type == BINDER_TYPE_BINDER && obj->binder == 0) {
        *out_handle = 0;
        *is_null = 1;
        return 0;
    }
    return -1;
}

int bm_pr_read_binder_handle(struct bm_parcel_reader *pr, int skip_stability,
                             uint32_t *out_handle, int *is_null) {
    /* The reply's flat_binder_object is not necessarily at the current
     * position; locate it by scanning the offsets array instead. */
    if (pr->offsets && pr->offs_count > 0) {
        for (size_t i = 0; i < pr->offs_count; i++) {
            size_t off = (size_t)pr->offsets[i];
            if (off > pr->size ||
                sizeof(struct flat_binder_object) > pr->size - off)
                continue;
            struct flat_binder_object obj;
            memcpy(&obj, pr->data + off, sizeof(obj));
            if (binder_handle_from_obj(&obj, out_handle, is_null) == 0) {
                pr->pos = off + sizeof(obj);
                if (skip_stability && sizeof(int32_t) <= pr->size - pr->pos)
                    pr->pos += sizeof(int32_t);
                return 0;
            }
        }
        return -1;
    }

    if (sizeof(struct flat_binder_object) > pr->size - pr->pos)
        return -1;
    struct flat_binder_object obj;
    memcpy(&obj, pr->data + pr->pos, sizeof(obj));
    pr->pos += sizeof(obj);

    if (skip_stability && sizeof(int32_t) <= pr->size - pr->pos)
        pr->pos += sizeof(int32_t);

    return binder_handle_from_obj(&obj, out_handle, is_null);
}

int bm_binder_ioctl_wr(struct bm_binder_ctx *ctx,
                       const void *wbuf, size_t wlen,
                       void *rbuf, size_t rcap, size_t *rlen) {
    struct binder_write_read bwr;
    memset(&bwr, 0, sizeof(bwr));
    bwr.write_buffer = (binder_uintptr_t)wbuf;
    bwr.write_size = (binder_size_t)wlen;
    bwr.read_buffer = (binder_uintptr_t)rbuf;
    bwr.read_size = (binder_size_t)rcap;

    if (ioctl(ctx->fd, BINDER_WRITE_READ, &bwr) < 0)
        return -errno;

    if (rlen)
        *rlen = (size_t)bwr.read_consumed;
    return 0;
}

int bm_binder_write_cmd(struct bm_binder_ctx *ctx, const void *buf, size_t len) {
    int rc = bm_binder_ioctl_wr(ctx, buf, len, NULL, 0, NULL);
    if (rc < 0) {
        uint32_t cmd = 0;
        if (len >= sizeof(cmd))
            memcpy(&cmd, buf, sizeof(cmd));
        fprintf(stderr, "[warn] binder cmd 0x%08x failed: %s\n",
                cmd, strerror(-rc));
    }
    return rc;
}

void bm_binder_free_buffer(struct bm_binder_ctx *ctx, binder_uintptr_t buf_ptr) {
    struct {
        uint32_t cmd;
        binder_uintptr_t ptr;
    } __attribute__((packed)) pkt = {
        .cmd = BC_FREE_BUFFER,
        .ptr = buf_ptr,
    };
    (void)bm_binder_write_cmd(ctx, &pkt, sizeof(pkt));
}

void bm_binder_acquire_handle(struct bm_binder_ctx *ctx, uint32_t handle) {
    struct {
        uint32_t cmd_inc;
        uint32_t handle_inc;
        uint32_t cmd_acq;
        uint32_t handle_acq;
    } __attribute__((packed)) pkt = {
        .cmd_inc = BC_INCREFS,
        .handle_inc = handle,
        .cmd_acq = BC_ACQUIRE,
        .handle_acq = handle,
    };
    (void)bm_binder_write_cmd(ctx, &pkt, sizeof(pkt));
}

void bm_binder_release_handle(struct bm_binder_ctx *ctx, uint32_t handle) {
    struct {
        uint32_t cmd_rel;
        uint32_t handle_rel;
        uint32_t cmd_dec;
        uint32_t handle_dec;
    } __attribute__((packed)) pkt = {
        .cmd_rel = BC_RELEASE,
        .handle_rel = handle,
        .cmd_dec = BC_DECREFS,
        .handle_dec = handle,
    };
    (void)bm_binder_write_cmd(ctx, &pkt, sizeof(pkt));
}

void bm_binder_ack_ptr_cookie(struct bm_binder_ctx *ctx, uint32_t bc_cmd,
                              const struct binder_ptr_cookie *pc) {
    struct {
        uint32_t cmd;
        struct binder_ptr_cookie pc;
    } __attribute__((packed)) pkt = {
        .cmd = bc_cmd,
        .pc = *pc,
    };
    (void)bm_binder_write_cmd(ctx, &pkt, sizeof(pkt));
}

void bm_binder_send_reply_parcel(struct bm_binder_ctx *ctx,
                                 const struct bm_parcel_writer *pw) {
    struct {
        uint32_t cmd;
        struct binder_transaction_data txn;
    } __attribute__((packed)) pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = BC_REPLY;
    pkt.txn.data_size = (binder_size_t)pw->data_pos;
    pkt.txn.offsets_size = (binder_size_t)(pw->offs_count * sizeof(binder_size_t));
    pkt.txn.data.ptr.buffer = (binder_uintptr_t)pw->data;
    pkt.txn.data.ptr.offsets = (binder_uintptr_t)pw->offsets;
    (void)bm_binder_write_cmd(ctx, &pkt, sizeof(pkt));
}

void bm_binder_send_empty_reply(struct bm_binder_ctx *ctx, int32_t status) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    bm_pw_write_i32(&pw, status);
    bm_binder_send_reply_parcel(ctx, &pw);
}

int bm_binder_process_br_stream(struct bm_binder_ctx *ctx, const uint8_t *buf,
                                size_t len, struct binder_transaction_data *reply_out) {
    size_t off = 0;
    while (off + sizeof(uint32_t) <= len) {
        uint32_t cmd = 0;
        memcpy(&cmd, buf + off, sizeof(cmd));
        off += sizeof(cmd);

        switch (cmd) {
        case BR_NOOP:
        case BR_TRANSACTION_COMPLETE:
        case BR_SPAWN_LOOPER:
            break;

        case BR_INCREFS: {
            if (off + sizeof(struct binder_ptr_cookie) > len) return -EINVAL;
            struct binder_ptr_cookie pc;
            memcpy(&pc, buf + off, sizeof(pc));
            off += sizeof(pc);
            bm_binder_ack_ptr_cookie(ctx, BC_INCREFS_DONE, &pc);
            break;
        }
        case BR_ACQUIRE: {
            if (off + sizeof(struct binder_ptr_cookie) > len) return -EINVAL;
            struct binder_ptr_cookie pc;
            memcpy(&pc, buf + off, sizeof(pc));
            off += sizeof(pc);
            bm_binder_ack_ptr_cookie(ctx, BC_ACQUIRE_DONE, &pc);
            break;
        }
        case BR_RELEASE:
        case BR_DECREFS: {
            if (off + sizeof(struct binder_ptr_cookie) > len) return -EINVAL;
            off += sizeof(struct binder_ptr_cookie);
            break;
        }
        case BR_ATTEMPT_ACQUIRE: {
            if (off + sizeof(struct binder_pri_ptr_cookie) > len) return -EINVAL;
            off += sizeof(struct binder_pri_ptr_cookie);
            struct {
                uint32_t cmd;
                int32_t success;
            } __attribute__((packed)) pkt = { BC_ACQUIRE_RESULT, 1 };
            (void)bm_binder_write_cmd(ctx, &pkt, sizeof(pkt));
            break;
        }
        case BR_DEAD_BINDER: {
            if (off + sizeof(binder_uintptr_t) > len) return -EINVAL;
            binder_uintptr_t cookie = 0;
            memcpy(&cookie, buf + off, sizeof(cookie));
            off += sizeof(cookie);
            struct {
                uint32_t cmd;
                binder_uintptr_t cookie;
            } __attribute__((packed)) pkt = { BC_DEAD_BINDER_DONE, cookie };
            (void)bm_binder_write_cmd(ctx, &pkt, sizeof(pkt));
            fprintf(stderr, "[info] remote binder died\n");
            bm_running = 0;
            break;
        }
        case BR_TRANSACTION: {
            if (off + sizeof(struct binder_transaction_data) > len) return -EINVAL;
            struct binder_transaction_data txn;
            memcpy(&txn, buf + off, sizeof(txn));
            off += sizeof(txn);
            if (ctx->dispatch) {
                ctx->dispatch(ctx, ctx->userdata, &txn);
            } else {
                if (!(txn.flags & TF_ONE_WAY))
                    bm_binder_send_empty_reply(ctx, 0);
                bm_binder_free_buffer(ctx, txn.data.ptr.buffer);
            }
            break;
        }
        case BR_REPLY: {
            if (off + sizeof(struct binder_transaction_data) > len) return -EINVAL;
            struct binder_transaction_data rep;
            memcpy(&rep, buf + off, sizeof(rep));
            off += sizeof(rep);
            if (reply_out) {
                *reply_out = rep;
                return 1;
            }
            bm_binder_free_buffer(ctx, rep.data.ptr.buffer);
            break;
        }
        case BR_DEAD_REPLY:
            return -EPIPE;
        case BR_FAILED_REPLY:
            return -EIO;
        case BR_ERROR: {
            if (off + sizeof(int32_t) > len) return -EINVAL;
            int32_t err = 0;
            memcpy(&err, buf + off, sizeof(err));
            off += sizeof(err);
            return err < 0 ? err : -EIO;
        }
        default:
            fprintf(stderr, "[warn] unhandled BR cmd: 0x%08x\n", cmd);
            return 0;
        }
    }
    return 0;
}

int bm_binder_transact_sync(struct bm_binder_ctx *ctx, uint32_t target_handle,
                            uint32_t code, const struct bm_parcel_writer *pw,
                            struct binder_transaction_data *reply_out) {
    struct {
        uint32_t cmd;
        struct binder_transaction_data txn;
    } __attribute__((packed)) req;

    memset(&req, 0, sizeof(req));
    req.cmd = BC_TRANSACTION;
    req.txn.target.handle = target_handle;
    req.txn.code = code;
    req.txn.flags = TF_ACCEPT_FDS;
    req.txn.data_size = (binder_size_t)pw->data_pos;
    req.txn.offsets_size = (binder_size_t)(pw->offs_count * sizeof(binder_size_t));
    req.txn.data.ptr.buffer = (binder_uintptr_t)pw->data;
    req.txn.data.ptr.offsets = (binder_uintptr_t)pw->offsets;

    uint8_t rbuf[4096];
    size_t rlen = 0;
    int rc = bm_binder_ioctl_wr(ctx, &req, sizeof(req), rbuf, sizeof(rbuf), &rlen);
    if (rc < 0)
        return rc;

    while (1) {
        int res = bm_binder_process_br_stream(ctx, rbuf, rlen, reply_out);
        if (res != 0)
            return res;

        rc = bm_binder_ioctl_wr(ctx, NULL, 0, rbuf, sizeof(rbuf), &rlen);
        if (rc < 0)
            return rc;
    }
}

int bm_binder_lookup_service(struct bm_binder_ctx *ctx, const char *name,
                             uint32_t *out_handle) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    if (bm_pw_write_aidl_token(&pw, "android.os.IServiceManager") < 0 ||
        bm_pw_write_string16_ascii(&pw, name) < 0)
        return -EINVAL;

    struct binder_transaction_data rep;
    int rc = bm_binder_transact_sync(ctx, 0, 1 /* checkService */, &pw, &rep);
    if (rc < 0)
        return rc;

    struct bm_parcel_reader pr;
    bm_pr_init(&pr, &rep);
    int32_t ex = -1;
    int is_null = 1;
    uint32_t handle = 0;
    if (bm_pr_read_i32(&pr, &ex) < 0 || ex != 0 ||
        bm_pr_read_binder_handle(&pr, 1, &handle, &is_null) < 0 || is_null) {
        bm_binder_free_buffer(ctx, rep.data.ptr.buffer);
        return -ENOENT;
    }

    /* The reply buffer owns the ref; acquire before freeing or the handle dies. */
    bm_binder_acquire_handle(ctx, handle);
    bm_binder_free_buffer(ctx, rep.data.ptr.buffer);
    *out_handle = handle;
    return 0;
}

int bm_binder_open(struct bm_binder_ctx *ctx, const char *dev_path) {
    ctx->fd = open(dev_path, O_RDWR | O_CLOEXEC);
    ctx->vm_base = NULL;
    if (ctx->fd < 0) {
        fprintf(stderr, "open(%s): %s\n", dev_path, strerror(errno));
        return -errno;
    }

    ctx->vm_base = mmap(NULL, BM_BINDER_VM_SIZE, PROT_READ, MAP_PRIVATE, ctx->fd, 0);
    if (ctx->vm_base == MAP_FAILED) {
        int err = -errno;
        fprintf(stderr, "mmap(%s): %s\n", dev_path, strerror(errno));
        ctx->vm_base = NULL;
        close(ctx->fd);
        ctx->fd = -1;
        return err;
    }

    int32_t protocol_version = 0;
    if (ioctl(ctx->fd, BINDER_VERSION, &protocol_version) < 0)
        fprintf(stderr, "BINDER_VERSION: %s\n", strerror(errno));

    uint32_t max_threads = 15;
    (void)ioctl(ctx->fd, BINDER_SET_MAX_THREADS, &max_threads);

    uint32_t one = 1;
    (void)ioctl(ctx->fd, BINDER_ENABLE_ONEWAY_SPAM_DETECTION, &one);

    return 0;
}

void bm_binder_close(struct bm_binder_ctx *ctx) {
    if (ctx->fd < 0)
        return;
    uint32_t exit_cmd = BC_EXIT_LOOPER;
    (void)bm_binder_write_cmd(ctx, &exit_cmd, sizeof(exit_cmd));
    if (ctx->vm_base)
        munmap(ctx->vm_base, BM_BINDER_VM_SIZE);
    close(ctx->fd);
    ctx->fd = -1;
    ctx->vm_base = NULL;
}
