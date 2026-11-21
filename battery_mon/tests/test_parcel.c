/*
 * test_parcel.c - bounds and round-trip checks for the shared parcel code.
 * Run with: make test
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bm_binder.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void put_i32(uint8_t *buf, size_t *pos, int32_t v) {
    memcpy(buf + *pos, &v, sizeof(v));
    *pos += sizeof(v);
}

static void reader_for(struct bm_parcel_reader *pr, const uint8_t *data, size_t size) {
    memset(pr, 0, sizeof(*pr));
    pr->data = data;
    pr->size = size;
}

static void test_writer_roundtrip(void) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    CHECK(bm_pw_write_i32(&pw, 7) == 0);
    CHECK(bm_pw_write_string16_ascii(&pw, "hello") == 0);
    CHECK(bm_pw_write_cstring(&pw, "name") == 0);

    struct bm_parcel_reader pr;
    reader_for(&pr, pw.data, pw.data_pos);
    int32_t v = 0;
    char s[16];
    CHECK(bm_pr_read_i32(&pr, &v) == 0 && v == 7);
    CHECK(bm_pr_read_string16_ascii(&pr, s, sizeof(s)) == 0 && strcmp(s, "hello") == 0);
    CHECK(bm_pr_read_cstring(&pr, s, sizeof(s)) == 0 && strcmp(s, "name") == 0);
    CHECK(pr.pos == pr.size);
}

static void test_string16_bounds(void) {
    uint8_t buf[16];
    size_t pos = 0;
    char s[16];
    struct bm_parcel_reader pr;

    memset(buf, 0, sizeof(buf));
    put_i32(buf, &pos, INT32_MAX); /* hostile length must not wrap */
    reader_for(&pr, buf, pos);
    CHECK(bm_pr_read_string16_ascii(&pr, s, sizeof(s)) < 0);

    pos = 0;
    put_i32(buf, &pos, 10); /* claims 10 chars, payload absent */
    reader_for(&pr, buf, pos);
    CHECK(bm_pr_read_string16_ascii(&pr, s, sizeof(s)) < 0);

    pos = 0;
    put_i32(buf, &pos, -1); /* null string */
    reader_for(&pr, buf, pos);
    CHECK(bm_pr_read_string16_ascii(&pr, s, sizeof(s)) == 0 && s[0] == '\0');
}

static void test_writer_overflow(void) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    char big[3000];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(bm_pw_write_string16_ascii(&pw, big) < 0);
}

static void test_cstring_bounds(void) {
    const uint8_t buf[4] = { 'a', 'b', 'c', 'd' }; /* no NUL */
    struct bm_parcel_reader pr;
    char s[8];
    reader_for(&pr, buf, sizeof(buf));
    CHECK(bm_pr_read_cstring(&pr, s, sizeof(s)) < 0);
}

static void test_fixed_width_bounds(void) {
    const uint8_t buf[4] = { 0, 0, 0, 0 };
    struct bm_parcel_reader pr;
    int64_t i64 = 0;
    float f = 0.0f;
    reader_for(&pr, buf, sizeof(buf));
    CHECK(bm_pr_read_i64(&pr, &i64) < 0);
    CHECK(bm_pr_read_f32(&pr, &f) == 0);
}

static void test_local_binder_writer(void) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    CHECK(bm_pw_write_local_binder(&pw, (void *)(uintptr_t)0x1234, 1,
                                   BM_STABILITY_VINTF) == 0);
    CHECK(pw.offs_count == 1);
    CHECK(pw.offsets[0] == 0);
    CHECK(pw.data_pos == sizeof(struct flat_binder_object) + sizeof(int32_t));

    struct flat_binder_object obj;
    memcpy(&obj, pw.data, sizeof(obj));
    CHECK(obj.hdr.type == BINDER_TYPE_BINDER);
    CHECK(obj.binder == (binder_uintptr_t)(uintptr_t)0x1234);
    int32_t stability = 0;
    memcpy(&stability, pw.data + sizeof(obj), sizeof(stability));
    CHECK(stability == BM_STABILITY_VINTF);
}

static void test_fd_writer(void) {
    struct bm_parcel_writer pw;
    bm_pw_init(&pw);
    CHECK(bm_pw_write_fd(&pw, 7) == 0);
    CHECK(pw.offs_count == 1);
    CHECK(pw.offsets[0] == 0);
    CHECK(pw.data_pos == sizeof(struct flat_binder_object));

    struct flat_binder_object obj;
    memcpy(&obj, pw.data, sizeof(obj));
    CHECK(obj.hdr.type == BINDER_TYPE_FD);
    CHECK(obj.handle == 7);
    CHECK(obj.cookie == 1);
    CHECK(obj.flags & FLAT_BINDER_FLAG_ACCEPTS_FDS);
}

static void test_hidl_string_writer(void) {
    struct bm_parcel_writer pw;
    struct bm_hidl_wire_hdr hdr;
    bm_pw_init(&pw);
    CHECK(bm_pw_write_hidl_string(&pw, &hdr, "abc") == 0);
    CHECK(pw.offs_count == 2);
    CHECK(pw.data_pos == 2 * sizeof(struct binder_buffer_object));
}

static void test_read_binder_handle(void) {
    uint8_t buf[128];
    struct flat_binder_object obj;
    struct binder_transaction_data txn;
    struct bm_parcel_reader pr;
    binder_size_t offs[1] = { 0 };
    uint32_t handle = 0;
    int is_null = 0;

    memset(&obj, 0, sizeof(obj));
    obj.hdr.type = BINDER_TYPE_HANDLE;
    obj.handle = 42;
    memcpy(buf, &obj, sizeof(obj));
    size_t pos = sizeof(obj);
    int32_t stability = BM_STABILITY_SYSTEM;
    memcpy(buf + pos, &stability, sizeof(stability));
    pos += sizeof(stability);

    memset(&txn, 0, sizeof(txn));
    txn.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)buf;
    txn.data_size = (binder_size_t)pos;
    txn.data.ptr.offsets = (binder_uintptr_t)(uintptr_t)offs;
    txn.offsets_size = (binder_size_t)sizeof(offs);

    bm_pr_init(&pr, &txn);
    CHECK(bm_pr_read_binder_handle(&pr, 1, &handle, &is_null) == 0);
    CHECK(handle == 42);
    CHECK(is_null == 0);
    CHECK(pr.pos == sizeof(obj) + sizeof(stability));

    /* null binder object, no offsets array */
    memset(&obj, 0, sizeof(obj));
    obj.hdr.type = BINDER_TYPE_BINDER;
    obj.binder = 0;
    memcpy(buf, &obj, sizeof(obj));
    txn.data_size = (binder_size_t)sizeof(obj);
    txn.offsets_size = 0;
    txn.data.ptr.offsets = 0;
    bm_pr_init(&pr, &txn);
    CHECK(bm_pr_read_binder_handle(&pr, 0, &handle, &is_null) == 0);
    CHECK(is_null == 1);
}

int main(void) {
    test_writer_roundtrip();
    test_string16_bounds();
    test_writer_overflow();
    test_cstring_bounds();
    test_fixed_width_bounds();
    test_local_binder_writer();
    test_fd_writer();
    test_hidl_string_writer();
    test_read_binder_handle();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("all parcel tests passed\n");
    return 0;
}
