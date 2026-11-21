#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bm_txn_cache.h"

typedef jint (*jni_create_vm_fn)(JavaVM **, JNIEnv **, void *);
typedef void *(*jni_invocation_create_fn)(void);
typedef bool (*jni_invocation_init_fn)(void *, const char *);

/* libsigchain calls this during JVM startup. The fetcher exits with _exit(2)
 * before teardown, so an empty handler is enough. */
__attribute__((visibility("default"))) void AddSpecialSignalHandlerFn(void) {}

struct txn_code {
    const char *class_name;
    const char *field_name;
    const char *macro;
    enum bm_txn_id id;
};

static const struct txn_code TXN_CODES[] = {
    { "android/os/IBinder", "DUMP_TRANSACTION", "BM_TXN_DUMP",
      BM_TXN_ID_DUMP },
    { "android/os/IBinder", "INTERFACE_TRANSACTION", "BM_TXN_INTERFACE",
      BM_TXN_ID_INTERFACE },
    { "android/os/IBatteryPropertiesRegistrar$Stub",
      "TRANSACTION_getProperty", "BM_TXN_BPR_GET_PROPERTY",
      BM_TXN_ID_BPR_GET_PROPERTY },
    { "android/hardware/input/IInputManager$Stub",
      "TRANSACTION_getInputDeviceIds", "BM_TXN_INPUT_GET_IDS",
      BM_TXN_ID_INPUT_GET_IDS },
    { "android/hardware/input/IInputManager$Stub",
      "TRANSACTION_registerBatteryListener", "BM_TXN_INPUT_REG_BATTERY",
      BM_TXN_ID_INPUT_REG_BATTERY },
    { "android/os/IServiceManager$Stub",
      "TRANSACTION_getService", "BM_TXN_SM_GET_SERVICE",
      BM_TXN_ID_SM_GET_SERVICE },
    { "android/os/IServiceManager$Stub",
      "TRANSACTION_getService2", "BM_TXN_SM_GET_SERVICE2",
      BM_TXN_ID_SM_GET_SERVICE2 },
    { "android/os/IServiceManager$Stub",
      "TRANSACTION_checkService", "BM_TXN_SM_CHECK_SERVICE",
      BM_TXN_ID_SM_CHECK_SERVICE },
    { "android/os/IServiceManager$Stub",
      "TRANSACTION_checkService2", "BM_TXN_SM_CHECK_SERVICE2",
      BM_TXN_ID_SM_CHECK_SERVICE2 },
    { "android/os/IServiceManager$Stub",
      "TRANSACTION_listServices", "BM_TXN_SM_LIST_SERVICES",
      BM_TXN_ID_SM_LIST_SERVICES },
};

static char *read_all(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }

    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (grown == NULL) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = grown;
        }

        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }

    close(fd);
    *out_len = len;
    return buf;
}

/* ART lives in the com_android_art namespace. libnativehelper.so is exported
 * into the default namespace and links to libart.so internally, so this avoids
 * opening /apex/com.android.art/lib64/libart.so directly. */
static char *bootclasspath_from_zygote(void) {
    DIR *proc = opendir("/proc");
    if (proc == NULL) {
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        char comm[32];
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n <= 0) {
            continue;
        }
        comm[n] = '\0';
        char *newline = strchr(comm, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
        if (strcmp(comm, "zygote64") != 0 && strcmp(comm, "zygote") != 0) {
            continue;
        }

        snprintf(path, sizeof(path), "/proc/%s/environ", entry->d_name);
        size_t env_len = 0;
        char *env = read_all(path, &env_len);
        if (env == NULL) {
            continue;
        }

        for (size_t off = 0; off < env_len;) {
            const char *var = env + off;
            size_t var_len = strnlen(var, env_len - off);
            if (var_len > 14 && strncmp(var, "BOOTCLASSPATH=", 14) == 0) {
                char *value = strndup(var + 14, var_len - 14);
                free(env);
                closedir(proc);
                return value;
            }
            off += var_len + 1;
        }

        free(env);
    }

    closedir(proc);
    return NULL;
}

static char *get_bootclasspath(void) {
    const char *env = getenv("BOOTCLASSPATH");
    if (env != NULL && env[0] != '\0') {
        return strdup(env);
    }
    return bootclasspath_from_zygote();
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [--cache PATH] [--quiet]\n", prog);
}

int main(int argc, char **argv) {
    const char *cache_path = NULL;
    bool quiet = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cache") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            cache_path = argv[i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);

    char *bootclasspath = get_bootclasspath();
    if (bootclasspath == NULL) {
        fprintf(stderr,
                "code_fetcher: BOOTCLASSPATH is unset and the zygote environment "
                "is unreadable.\nRun as root or set BOOTCLASSPATH.\n");
        return 1;
    }
    if (setenv("BOOTCLASSPATH", bootclasspath, 1) != 0) {
        fprintf(stderr, "code_fetcher: setenv(BOOTCLASSPATH) failed\n");
        free(bootclasspath);
        return 1;
    }
    setenv("ANDROID_ROOT", "/system", 0);
    setenv("ANDROID_DATA", "/data", 0);

    void *nativehelper = dlopen("libnativehelper.so", RTLD_NOW | RTLD_GLOBAL);
    if (nativehelper == NULL) {
        fprintf(stderr, "code_fetcher: dlopen libnativehelper.so: %s\n",
                dlerror());
        free(bootclasspath);
        return 1;
    }

    jni_create_vm_fn create_vm =
        (jni_create_vm_fn)dlsym(nativehelper, "JNI_CreateJavaVM");
    jni_invocation_create_fn invocation_create =
        (jni_invocation_create_fn)dlsym(nativehelper, "JniInvocationCreate");
    jni_invocation_init_fn invocation_init =
        (jni_invocation_init_fn)dlsym(nativehelper, "JniInvocationInit");
    if (create_vm == NULL || invocation_create == NULL || invocation_init == NULL) {
        fprintf(stderr,
                "code_fetcher: libnativehelper.so is missing JNI invocation "
                "symbols: %s\n",
                dlerror());
        free(bootclasspath);
        return 1;
    }

    void *invocation = invocation_create();
    if (invocation == NULL || !invocation_init(invocation, "libart.so")) {
        fprintf(stderr, "code_fetcher: JniInvocationInit(libart.so) failed\n");
        free(bootclasspath);
        return 1;
    }

    JavaVMOption option = {
        .optionString = "-Djava.class.path=/apex/com.android.art/javalib/core-oj.jar",
    };
    JavaVMInitArgs args = {
        .version = JNI_VERSION_1_6,
        .nOptions = 1,
        .options = &option,
        .ignoreUnrecognized = JNI_TRUE,
    };

    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    jint rc = create_vm(&vm, &env, &args);
    if (rc != JNI_OK || vm == NULL || env == NULL) {
        fprintf(stderr, "code_fetcher: JNI_CreateJavaVM failed: %d\n", rc);
        free(bootclasspath);
        return 1;
    }

    bool ok = true;
    uint32_t values[BM_TXN_ID_COUNT] = {0};
    for (size_t i = 0; i < sizeof(TXN_CODES) / sizeof(TXN_CODES[0]); i++) {
        const struct txn_code *txn = &TXN_CODES[i];
        jclass cls = (*env)->FindClass(env, txn->class_name);
        if (cls == NULL) {
            (*env)->ExceptionClear(env);
            fprintf(stderr, "code_fetcher: class not found: %s\n",
                    txn->class_name);
            ok = false;
            continue;
        }

        jfieldID field = (*env)->GetStaticFieldID(env, cls, txn->field_name, "I");
        if (field == NULL) {
            (*env)->ExceptionClear(env);
            fprintf(stderr, "code_fetcher: field not found: %s.%s\n",
                    txn->class_name, txn->field_name);
            ok = false;
            (*env)->DeleteLocalRef(env, cls);
            continue;
        }

        jint value = (*env)->GetStaticIntField(env, cls, field);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            fprintf(stderr, "code_fetcher: read failed: %s.%s\n",
                    txn->class_name, txn->field_name);
            ok = false;
        } else {
            values[txn->id] = (uint32_t)value;
            if (strcmp(txn->macro, "BM_TXN_DUMP") == 0 && value != 0x5f444d50) {
                fprintf(stderr, "code_fetcher: unexpected IBinder.DUMP_TRANSACTION\n");
                ok = false;
            }
            if (strcmp(txn->macro, "BM_TXN_INTERFACE") == 0 &&
                value != 0x5f4e5446) {
                fprintf(stderr,
                        "code_fetcher: unexpected IBinder.INTERFACE_TRANSACTION\n");
                ok = false;
            }
        }
        (*env)->DeleteLocalRef(env, cls);
    }

    if (ok && cache_path != NULL) {
        if (bm_txn_cache_write(cache_path, values, bootclasspath) != 0) {
            fprintf(stderr, "code_fetcher: failed to write cache: %s\n",
                    cache_path);
            ok = false;
        } else if (!quiet) {
            fprintf(stderr, "code_fetcher: wrote %s\n", cache_path);
        }
    } else if (ok) {
        printf("/* Generated by tools/code_fetcher. Do not edit. */\n");
        printf("#ifndef BM_TXN_CODES_H\n#define BM_TXN_CODES_H\n\n");
        for (size_t i = 0; i < sizeof(TXN_CODES) / sizeof(TXN_CODES[0]); i++)
            printf("#define %s 0x%xU\n", TXN_CODES[i].macro,
                   values[TXN_CODES[i].id]);
        printf("\n#endif\n");
    }

    fflush(stdout);
    if (!ok) {
        fprintf(stderr, "code_fetcher: one or more transaction codes failed\n");
    }

    /* shortcut: skip DestroyJavaVM because libsigchain aborts while removing
     * its signal handler; upgrade if this becomes a long-lived process. */
    _exit(ok ? 0 : 1);
}
