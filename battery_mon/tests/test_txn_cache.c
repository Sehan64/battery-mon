#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bm_txn_cache.h"

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    const char *path = "tests/.txn_cache_test.bin";
    const char *bootclasspath = "/test/bootclasspath";
    uint32_t codes[BM_TXN_ID_COUNT] = {
        0x5f444d50U, 0x5f4e5446U, 1, 3, 58, 1, 2, 3, 4, 6,
    };
    struct bm_txn_cache cache;
    struct bm_txn_codes out;

    unlink(path);

    int rc = bm_txn_cache_read(path, &cache);
    if (rc != BM_TXN_CACHE_MISSING)
        return fail("missing cache was not reported as missing");

    if (bm_txn_cache_write(path, codes, bootclasspath) != 0)
        return fail("cache write failed");

    rc = bm_txn_cache_read(path, &cache);
    if (rc != BM_TXN_CACHE_OK)
        return fail("cache read failed");
    if (memcmp(cache.codes, codes, sizeof(codes)) != 0)
        return fail("codes did not round-trip");
    if (strcmp(cache.bootclasspath, bootclasspath) != 0)
        return fail("bootclasspath did not round-trip");
    if (cache.fingerprint[0] == '\0')
        return fail("fingerprint was not stored");

    memset(&out, 0, sizeof(out));
    out.sm_check_service = 2;
    out.sm_list_services = 4;
    bm_txn_cache_apply(&out, &cache);
    if (out.dump_transaction != codes[BM_TXN_ID_DUMP] ||
        out.interface_transaction != codes[BM_TXN_ID_INTERFACE] ||
        out.bpr_get_property != codes[BM_TXN_ID_BPR_GET_PROPERTY] ||
        out.input_get_ids != codes[BM_TXN_ID_INPUT_GET_IDS] ||
        out.input_reg_battery != codes[BM_TXN_ID_INPUT_REG_BATTERY])
        return fail("safe codes were not applied");
    if (out.sm_check_service != 2 || out.sm_list_services != 4)
        return fail("legacy service-manager codes were overwritten");

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return fail("cache reopen failed");
    off_t offset = (off_t)offsetof(struct bm_txn_cache, codes);
    uint8_t byte = 0;
    if (pread(fd, &byte, 1, offset) != 1) {
        close(fd);
        return fail("cache pread failed");
    }
    byte ^= 1U;
    if (pwrite(fd, &byte, 1, offset) != 1) {
        close(fd);
        return fail("cache pwrite failed");
    }
    close(fd);

    rc = bm_txn_cache_read(path, &cache);
    if (rc != BM_TXN_CACHE_CORRUPT)
        return fail("corrupt cache was not detected");

    unlink(path);
    printf("all txn cache tests passed\n");
    return 0;
}
