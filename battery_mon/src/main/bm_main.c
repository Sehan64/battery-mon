#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bm_common.h"
#include "bm_txn_cache.h"

static void on_signal(int sig) {
    (void)sig;
    bm_running = 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--method root_binder|netlink_uevent|shell_sysfs_polling|"
            "app_broadcastreceiver_method|input_battery_listener|"
            "battery_properties_registrar|battery_dump|auto]\n"
            "       root_binder and netlink_uevent need root; battery_dump needs\n"
            "       adb shell or root; input_battery_listener needs the input\n"
            "       service (some devices need root or BLUETOOTH).\n",
            prog);
}

static int run_method(const char *method) {
    if (strcmp(method, "root_binder") == 0)
        return bm_method_root_binder();
    if (strcmp(method, "netlink_uevent") == 0)
        return bm_method_netlink_uevent();
    if (strcmp(method, "shell_sysfs_polling") == 0)
        return bm_method_shell_sysfs_polling();
    if (strcmp(method, "app_broadcastreceiver_method") == 0)
        return bm_method_app_broadcastreceiver_method();
    if (strcmp(method, "input_battery_listener") == 0)
        return bm_method_input_battery_listener();
    if (strcmp(method, "battery_properties_registrar") == 0)
        return bm_method_battery_properties_registrar();
    if (strcmp(method, "battery_dump") == 0)
        return bm_method_battery_dump();
    return 2;
}

int main(int argc, char **argv) {
    const char *method = "auto";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--method") == 0 || strcmp(argv[i], "-m") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            method = argv[i];
        } else if (strncmp(argv[i], "--method=", 9) == 0) {
            method = argv[i] + 9;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    bm_txn_cache_init();

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (strcmp(method, "auto") == 0) {
        static const char *const as_root[] = {
            "root_binder", "netlink_uevent", "shell_sysfs_polling",
            "battery_properties_registrar", "app_broadcastreceiver_method"
        };
        static const char *const as_user[] = {
            "battery_properties_registrar", "shell_sysfs_polling",
            "app_broadcastreceiver_method"
        };
        int is_root = geteuid() == 0;
        const char *const *order = is_root ? as_root : as_user;
        size_t n = is_root ? sizeof(as_root) / sizeof(as_root[0])
                           : sizeof(as_user) / sizeof(as_user[0]);

        for (size_t i = 0; i < n; i++) {
            int rc = run_method(order[i]);
            if (rc == 0 || i + 1 == n)
                return rc < 0 ? 1 : rc;
        }
    }

    int rc = run_method(method);
    if (rc == 2) {
        fprintf(stderr, "unknown method: %s\n", method);
        usage(argv[0]);
        return 2;
    }
    return rc < 0 ? 1 : rc;
}
