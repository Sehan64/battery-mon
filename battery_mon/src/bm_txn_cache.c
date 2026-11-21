#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "bm_txn_cache.h"

struct bm_txn_codes bm_txn;

static void set_defaults(struct bm_txn_codes *codes) {
    codes->dump_transaction = 0x5f444d50U;
    codes->interface_transaction = 0x5f4e5446U;
    codes->bpr_get_property = 1;
    codes->input_get_ids = 3;
    codes->input_reg_battery = 58;
    /* The working native service-manager calls use 2/4. The framework class
     * reports checkService=3/listServices=6, so keep the legacy pair here. */
    codes->sm_check_service = 2;
    codes->sm_list_services = 4;
}

static uint32_t crc32_bytes(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffU;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static void get_fingerprint(char *buf, size_t cap) {
    memset(buf, 0, cap);
    if (__system_property_get("ro.build.fingerprint", buf) <= 0)
        snprintf(buf, cap, "unknown");
}

static void get_fw_stat(struct bm_txn_cache_stat *out) {
    struct stat st;

    memset(out, 0, sizeof(*out));
    if (stat("/system/framework/framework.jar", &st) != 0)
        return;

    out->dev = (uint64_t)st.st_dev;
    out->ino = (uint64_t)st.st_ino;
    out->size = (uint64_t)st.st_size;
    out->mtime_sec = (int64_t)st.st_mtim.tv_sec;
    out->mtime_nsec = (int64_t)st.st_mtim.tv_nsec;
    out->mode = (uint32_t)st.st_mode;
    out->uid = (uint32_t)st.st_uid;
    out->gid = (uint32_t)st.st_gid;
    out->valid = 1;
}

static uint32_t cache_crc(const struct bm_txn_cache *cache) {
    struct bm_txn_cache copy = *cache;

    copy.crc = 0;
    return crc32_bytes(&copy, sizeof(copy));
}

int bm_txn_cache_path(char *buf, size_t cap) {
    uid_t uid = geteuid();

    if (buf == NULL || cap == 0)
        return -1;

    if (uid == 0) {
        if (mkdir("/data/data/android", 0700) != 0 && errno != EEXIST)
            return -1;
        if (snprintf(buf, cap, "/data/data/android/transact_codes.bin") >= (int)cap)
            return -1;
        return 0;
    }

    if (uid == 2000) {
        if (snprintf(buf, cap,
                     "/data/user_de/0/com.android.shell/transact_codes.bin") >=
            (int)cap)
            return -1;
        return 0;
    }

    if (snprintf(buf, cap, "/sdcard/.1t4c.bin") >= (int)cap)
        return -1;
    return 0;
}

int bm_txn_cache_read(const char *path, struct bm_txn_cache *out) {
    struct stat st;
    char fingerprint[BM_TXN_FP_MAX];
    struct bm_txn_cache_stat fw;

    if (path == NULL || out == NULL)
        return BM_TXN_CACHE_IO;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? BM_TXN_CACHE_MISSING : BM_TXN_CACHE_IO;

    if (fstat(fd, &st) != 0) {
        close(fd);
        return BM_TXN_CACHE_IO;
    }
    if (!S_ISREG(st.st_mode) || st.st_size != (off_t)sizeof(*out)) {
        close(fd);
        return BM_TXN_CACHE_CORRUPT;
    }

    uint8_t *p = (uint8_t *)out;
    size_t left = sizeof(*out);
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return BM_TXN_CACHE_IO;
        }
        if (n == 0) {
            close(fd);
            return BM_TXN_CACHE_CORRUPT;
        }
        p += n;
        left -= (size_t)n;
    }
    close(fd);

    if (out->magic != BM_TXN_CACHE_MAGIC ||
        out->version != BM_TXN_CACHE_VERSION ||
        out->size != sizeof(*out) ||
        out->code_count != BM_TXN_ID_COUNT)
        return BM_TXN_CACHE_CORRUPT;

    uint32_t expected = out->crc;
    out->crc = 0;
    uint32_t actual = crc32_bytes(out, sizeof(*out));
    out->crc = expected;
    if (actual != expected)
        return BM_TXN_CACHE_CORRUPT;

    get_fingerprint(fingerprint, sizeof(fingerprint));
    if (memcmp(out->fingerprint, fingerprint, sizeof(fingerprint)) != 0)
        return BM_TXN_CACHE_STALE;

    get_fw_stat(&fw);
    if (memcmp(&out->fw, &fw, sizeof(fw)) != 0)
        return BM_TXN_CACHE_STALE;

    return BM_TXN_CACHE_OK;
}

int bm_txn_cache_write(const char *path,
                       const uint32_t codes[BM_TXN_ID_COUNT],
                       const char *bootclasspath) {
    struct bm_txn_cache cache;
    char tmp[BM_TXN_PATH_MAX + 16];

    if (path == NULL || codes == NULL)
        return -1;
    if (snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path) >= (int)sizeof(tmp))
        return -1;

    memset(&cache, 0, sizeof(cache));
    cache.magic = BM_TXN_CACHE_MAGIC;
    cache.version = BM_TXN_CACHE_VERSION;
    cache.size = sizeof(cache);
    cache.code_count = BM_TXN_ID_COUNT;
    cache.generator_uid = (uint32_t)geteuid();
    cache.generated_at = (uint64_t)time(NULL);
    get_fingerprint(cache.fingerprint, sizeof(cache.fingerprint));
    if (bootclasspath != NULL)
        snprintf(cache.bootclasspath, sizeof(cache.bootclasspath), "%s",
                 bootclasspath);
    get_fw_stat(&cache.fw);
    memcpy(cache.codes, codes, sizeof(cache.codes));
    cache.crc = cache_crc(&cache);

    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;

    const uint8_t *p = (const uint8_t *)&cache;
    size_t left = sizeof(cache);
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }

    if (fchmod(fd, 0600) != 0 || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void bm_txn_cache_apply(struct bm_txn_codes *codes,
                        const struct bm_txn_cache *cache) {
    codes->dump_transaction = cache->codes[BM_TXN_ID_DUMP];
    codes->interface_transaction = cache->codes[BM_TXN_ID_INTERFACE];
    codes->bpr_get_property = cache->codes[BM_TXN_ID_BPR_GET_PROPERTY];
    codes->input_get_ids = cache->codes[BM_TXN_ID_INPUT_GET_IDS];
    codes->input_reg_battery = cache->codes[BM_TXN_ID_INPUT_REG_BATTERY];
}

const char *bm_txn_cache_result_str(int rc) {
    switch (rc) {
    case BM_TXN_CACHE_OK:
        return "ok";
    case BM_TXN_CACHE_MISSING:
        return "missing";
    case BM_TXN_CACHE_IO:
        return "I/O error";
    case BM_TXN_CACHE_CORRUPT:
        return "corrupt";
    case BM_TXN_CACHE_STALE:
        return "stale";
    default:
        return "unknown";
    }
}

static int run_fetcher(const char *cache_path, const char *bootclasspath) {
    char sibling[BM_TXN_PATH_MAX];
    const char *fetcher = NULL;
    const char *env = getenv("BM_CODE_FETCHER");

    if (env != NULL && env[0] == '/' && access(env, X_OK) == 0) {
        fetcher = env;
    } else {
        ssize_t n = readlink("/proc/self/exe", sibling, sizeof(sibling) - 1);
        if (n > 0) {
            sibling[n] = '\0';
            char *slash = strrchr(sibling, '/');
            if (slash != NULL) {
                snprintf(slash + 1, sizeof(sibling) - (size_t)(slash + 1 - sibling),
                         "code_fetcher");
                if (access(sibling, X_OK) == 0)
                    fetcher = sibling;
            }
        }
    }
    if (fetcher == NULL)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (bootclasspath != NULL && bootclasspath[0] != '\0')
            setenv("BOOTCLASSPATH", bootclasspath, 1);
        char *const argv[] = {
            (char *)fetcher, "--cache", (char *)cache_path, "--quiet", NULL,
        };
        execv(fetcher, argv);
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

void bm_txn_cache_init(void) {
    char path[BM_TXN_PATH_MAX];
    struct bm_txn_cache cache;
    uid_t uid = geteuid();

    set_defaults(&bm_txn);

    if (bm_txn_cache_path(path, sizeof(path)) != 0)
        return;

    memset(&cache, 0, sizeof(cache));
    int rc = bm_txn_cache_read(path, &cache);
    if (rc == BM_TXN_CACHE_OK) {
        bm_txn_cache_apply(&bm_txn, &cache);
        return;
    }

    if (uid == 0 || uid == 2000) {
        const char *bootclasspath =
            rc == BM_TXN_CACHE_STALE ? cache.bootclasspath : NULL;
        if (run_fetcher(path, bootclasspath) == 0) {
            struct bm_txn_cache fresh;
            if (bm_txn_cache_read(path, &fresh) == BM_TXN_CACHE_OK) {
                bm_txn_cache_apply(&bm_txn, &fresh);
                return;
            }
        }
    }

    if (rc == BM_TXN_CACHE_STALE || rc == BM_TXN_CACHE_CORRUPT ||
        rc == BM_TXN_CACHE_IO) {
        fprintf(stderr, "battery_mon: transaction-code cache %s; using defaults\n",
                bm_txn_cache_result_str(rc));
    }
}
