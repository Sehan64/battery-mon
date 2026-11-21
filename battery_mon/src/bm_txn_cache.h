#ifndef BM_TXN_CACHE_H
#define BM_TXN_CACHE_H

#include <stddef.h>
#include <stdint.h>

#define BM_TXN_CACHE_MAGIC   0x424d5443U /* "BMTC" */
#define BM_TXN_CACHE_VERSION 1U
#define BM_TXN_FP_MAX        128U
#define BM_TXN_BC_MAX        4096U
#define BM_TXN_PATH_MAX      512U

enum bm_txn_id {
    BM_TXN_ID_DUMP = 0,
    BM_TXN_ID_INTERFACE,
    BM_TXN_ID_BPR_GET_PROPERTY,
    BM_TXN_ID_INPUT_GET_IDS,
    BM_TXN_ID_INPUT_REG_BATTERY,
    BM_TXN_ID_SM_GET_SERVICE,
    BM_TXN_ID_SM_GET_SERVICE2,
    BM_TXN_ID_SM_CHECK_SERVICE,
    BM_TXN_ID_SM_CHECK_SERVICE2,
    BM_TXN_ID_SM_LIST_SERVICES,
    BM_TXN_ID_COUNT
};

struct bm_txn_cache_stat {
    uint64_t dev;
    uint64_t ino;
    uint64_t size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t valid;
};

struct bm_txn_cache {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t crc;
    uint32_t code_count;
    uint32_t generator_uid;
    uint64_t generated_at;
    char fingerprint[BM_TXN_FP_MAX];
    char bootclasspath[BM_TXN_BC_MAX];
    struct bm_txn_cache_stat fw;
    uint32_t codes[BM_TXN_ID_COUNT];
};

struct bm_txn_codes {
    uint32_t dump_transaction;
    uint32_t interface_transaction;
    uint32_t bpr_get_property;
    uint32_t input_get_ids;
    uint32_t input_reg_battery;
    uint32_t sm_check_service;
    uint32_t sm_list_services;
};

enum bm_txn_cache_result {
    BM_TXN_CACHE_OK = 0,
    BM_TXN_CACHE_MISSING = -1,
    BM_TXN_CACHE_IO = -2,
    BM_TXN_CACHE_CORRUPT = -3,
    BM_TXN_CACHE_STALE = -4
};

extern struct bm_txn_codes bm_txn;

int bm_txn_cache_path(char *buf, size_t cap);
int bm_txn_cache_read(const char *path, struct bm_txn_cache *out);
int bm_txn_cache_write(const char *path,
                       const uint32_t codes[BM_TXN_ID_COUNT],
                       const char *bootclasspath);
void bm_txn_cache_apply(struct bm_txn_codes *codes,
                        const struct bm_txn_cache *cache);
void bm_txn_cache_init(void);
const char *bm_txn_cache_result_str(int rc);

#endif
