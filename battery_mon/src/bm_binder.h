#ifndef BM_BINDER_H
#define BM_BINDER_H

#include <linux/android/binder.h>
#include <stddef.h>
#include <stdint.h>

#define BM_BINDER_VM_SIZE (1024U * 1024U)
#define BM_PAD_SIZE(s)    (((size_t)(s) + 3U) & ~(size_t)3U)

#ifndef B_PACK_CHARS
#define B_PACK_CHARS(c1, c2, c3, c4) \
    ((((uint32_t)(c1)) << 24) | (((uint32_t)(c2)) << 16) | \
     (((uint32_t)(c3)) << 8)  | ((uint32_t)(c4)))
#endif

/* AIDL parcel prologue (stable AIDL). */
#define BM_STRICT_MODE_NONE   0
#define BM_UNSET_WORK_SOURCE  (-1)
#define BM_PARCEL_SYS_HEADER  B_PACK_CHARS('S', 'Y', 'S', 'T')

/* binder::Stability::Level values used by platform HALs. */
#define BM_STABILITY_SYSTEM 12
#define BM_STABILITY_VINTF  0x3f

struct bm_parcel_writer {
    uint8_t data[2048];
    size_t data_pos;
    binder_size_t offsets[32];
    size_t offs_count;
};

struct bm_parcel_reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
    const binder_size_t *offsets;
    size_t offs_count;
};

/* HIDL hidl_string wire layout; pointer-sized so 32-bit builds match. */
struct bm_hidl_wire_hdr {
    uintptr_t buffer;
    uint32_t size;
    uint8_t owns_buffer;
    uint8_t pad[3];
};

void bm_pw_init(struct bm_parcel_writer *pw);
int bm_pw_write_bytes(struct bm_parcel_writer *pw, const void *src, size_t len);
int bm_pw_write_i32(struct bm_parcel_writer *pw, int32_t v);
int bm_pw_write_cstring(struct bm_parcel_writer *pw, const char *s);
int bm_pw_write_string16_ascii(struct bm_parcel_writer *pw, const char *s);
int bm_pw_write_aidl_token(struct bm_parcel_writer *pw, const char *iface);
int bm_pw_write_local_binder(struct bm_parcel_writer *pw, void *cookie_ptr,
                             int with_stability, int32_t stability);
int bm_pw_write_buffer_obj(struct bm_parcel_writer *pw, const void *buf, size_t len,
                           int has_parent, size_t parent_idx, size_t parent_off,
                           size_t *out_obj_idx);
int bm_pw_write_fd(struct bm_parcel_writer *pw, int fd);
int bm_pw_write_hidl_string(struct bm_parcel_writer *pw,
                            struct bm_hidl_wire_hdr *hdr_storage, const char *s);

void bm_pr_init(struct bm_parcel_reader *pr, const struct binder_transaction_data *txn);
int bm_pr_read_i32(struct bm_parcel_reader *pr, int32_t *out);
int bm_pr_read_i64(struct bm_parcel_reader *pr, int64_t *out);
int bm_pr_read_f32(struct bm_parcel_reader *pr, float *out);
int bm_pr_read_bool(struct bm_parcel_reader *pr, int *out);
int bm_pr_read_string16_ascii(struct bm_parcel_reader *pr, char *ascii_buf, size_t buf_sz);
int bm_pr_read_cstring(struct bm_parcel_reader *pr, char *buf, size_t buf_sz);
int bm_pr_read_aidl_token(struct bm_parcel_reader *pr, char *iface_buf, size_t buf_sz);
int bm_pr_read_binder_handle(struct bm_parcel_reader *pr, int skip_stability,
                             uint32_t *out_handle, int *is_null);

struct bm_binder_ctx {
    int fd;
    void *vm_base;
    void (*dispatch)(struct bm_binder_ctx *ctx, void *userdata,
                     const struct binder_transaction_data *txn);
    void *userdata;
};

int bm_binder_open(struct bm_binder_ctx *ctx, const char *dev_path);
void bm_binder_close(struct bm_binder_ctx *ctx);
int bm_binder_ioctl_wr(struct bm_binder_ctx *ctx, const void *wbuf, size_t wlen,
                       void *rbuf, size_t rcap, size_t *rlen);
int bm_binder_write_cmd(struct bm_binder_ctx *ctx, const void *buf, size_t len);
void bm_binder_free_buffer(struct bm_binder_ctx *ctx, binder_uintptr_t buf_ptr);
void bm_binder_acquire_handle(struct bm_binder_ctx *ctx, uint32_t handle);
void bm_binder_release_handle(struct bm_binder_ctx *ctx, uint32_t handle);
void bm_binder_ack_ptr_cookie(struct bm_binder_ctx *ctx, uint32_t bc_cmd,
                              const struct binder_ptr_cookie *pc);
void bm_binder_send_reply_parcel(struct bm_binder_ctx *ctx,
                                 const struct bm_parcel_writer *pw);
void bm_binder_send_empty_reply(struct bm_binder_ctx *ctx, int32_t status);
int bm_binder_process_br_stream(struct bm_binder_ctx *ctx, const uint8_t *buf,
                                size_t len, struct binder_transaction_data *reply_out);
int bm_binder_transact_sync(struct bm_binder_ctx *ctx, uint32_t target_handle,
                            uint32_t code, const struct bm_parcel_writer *pw,
                            struct binder_transaction_data *reply_out);
int bm_binder_lookup_service(struct bm_binder_ctx *ctx, const char *name,
                             uint32_t *out_handle);

#endif
