#include "bm_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

volatile sig_atomic_t bm_running = 1;

const char *bm_status_str(int32_t s) {
    switch (s) {
    case 1: return "UNKNOWN";
    case 2: return "CHARGING";
    case 3: return "DISCHARGING";
    case 4: return "NOT_CHARGING";
    case 5: return "FULL";
    default: return "OTHER";
    }
}

static const char *health_str(int32_t h) {
    switch (h) {
    case 1: return "UNKNOWN";
    case 2: return "GOOD";
    case 3: return "OVERHEAT";
    case 4: return "DEAD";
    case 5: return "OVERVOLTAGE";
    case 6: return "UNSPECIFIED_FAILURE";
    case 7: return "COLD";
    default: return "OTHER";
    }
}

void bm_print_event(int32_t level, int32_t b_status, int32_t b_health,
                    int ac, int usb, int wireless, int dock,
                    int present, int32_t voltage_mv, int32_t temp_deci_c) {
    const char *charger;
    if (ac)
        charger = "AC";
    else if (usb)
        charger = "USB";
    else if (wireless)
        charger = "WIRELESS";
    else if (dock)
        charger = "DOCK";
    else
        charger = "NONE";

    char present_buf[16], volt_buf[16], temp_buf[16];
    if (present < 0)
        snprintf(present_buf, sizeof(present_buf), "unknown");
    else
        snprintf(present_buf, sizeof(present_buf), "%d", present);
    if (voltage_mv < 0)
        snprintf(volt_buf, sizeof(volt_buf), "unknown");
    else
        snprintf(volt_buf, sizeof(volt_buf), "%dmV", voltage_mv);
    if (temp_deci_c < 0)
        snprintf(temp_buf, sizeof(temp_buf), "unknown");
    else
        snprintf(temp_buf, sizeof(temp_buf), "%.1fC", temp_deci_c / 10.0);

    printf("battery=%d%% status=%s health=%s charger=%s present=%s voltage=%s temp=%s\n",
           level, bm_status_str(b_status), health_str(b_health), charger,
           present_buf, volt_buf, temp_buf);
    fflush(stdout);
}

static int bm_sysfs_read(const char *path, char *buf, size_t sz) {
    if (sz < 2)
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n;
    do {
        n = read(fd, buf, sz - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static int bm_sysfs_i32(const char *path, int32_t *out) {
    char buf[64];
    if (bm_sysfs_read(path, buf, sizeof(buf)) < 0)
        return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (end == buf || errno == ERANGE)
        return -1;
    *out = (int32_t)v;
    return 0;
}

static int bm_sysfs_read_trim(const char *path, char *buf, size_t sz) {
    if (bm_sysfs_read(path, buf, sz) < 0)
        return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int bm_sysfs_online(const char *a, const char *b) {
    int32_t v = 0;
    if (bm_sysfs_i32(a, &v) < 0 && (b == NULL || bm_sysfs_i32(b, &v) < 0))
        return 0;
    return v != 0;
}

static int32_t bm_sysfs_battery_status(void) {
    char buf[64];
    if (bm_sysfs_read_trim(BM_SYSFS_BAT "/status", buf, sizeof(buf)) < 0)
        return 1;
    if (strcmp(buf, "Charging") == 0) return 2;
    if (strcmp(buf, "Discharging") == 0) return 3;
    if (strcmp(buf, "Not charging") == 0) return 4;
    if (strcmp(buf, "Full") == 0) return 5;
    return 1;
}

static int32_t bm_sysfs_battery_health(void) {
    char buf[64];
    if (bm_sysfs_read_trim(BM_SYSFS_BAT "/health", buf, sizeof(buf)) < 0)
        return 1;
    if (strcmp(buf, "Good") == 0) return 2;
    if (strcmp(buf, "Overheat") == 0) return 3;
    if (strcmp(buf, "Dead") == 0) return 4;
    if (strcmp(buf, "Over voltage") == 0) return 5;
    if (strcmp(buf, "Unspecified failure") == 0) return 6;
    if (strcmp(buf, "Cold") == 0) return 7;
    return 1;
}

int bm_sysfs_read_state(struct bm_state *st) {
    memset(st, 0, sizeof(*st));
    if (bm_sysfs_i32(BM_SYSFS_BAT "/capacity", &st->level) < 0)
        return -1;
    st->status = bm_sysfs_battery_status();
    st->health = bm_sysfs_battery_health();
    st->ac = bm_sysfs_online(BM_SYSFS_PS "/ac/online", BM_SYSFS_PS "/wired/online");
    st->usb = bm_sysfs_online(BM_SYSFS_PS "/usb/online", NULL);
    st->wireless = bm_sysfs_online(BM_SYSFS_PS "/wireless/online", NULL);
    st->dock = bm_sysfs_online(BM_SYSFS_PS "/dock/online", NULL);

    int32_t v = 0;
    st->present = -1;
    if (bm_sysfs_i32(BM_SYSFS_BAT "/present", &v) == 0)
        st->present = v;
    st->voltage_mv = -1;
    if (bm_sysfs_i32(BM_SYSFS_BAT "/voltage_now", &v) == 0)
        st->voltage_mv = v > 10000 ? v / 1000 : v;
    st->temp_deci_c = -1;
    if (bm_sysfs_i32(BM_SYSFS_BAT "/temp", &v) == 0)
        st->temp_deci_c = v;
    return 0;
}

void bm_state_print(const struct bm_state *st) {
    bm_print_event(st->level, st->status, st->health, st->ac, st->usb,
                   st->wireless, st->dock, st->present,
                   st->voltage_mv, st->temp_deci_c);
}
