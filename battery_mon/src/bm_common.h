#ifndef BM_COMMON_H
#define BM_COMMON_H

#include <signal.h>
#include <stdint.h>

#define BM_SYSFS_PS  "/sys/class/power_supply"
#define BM_SYSFS_BAT BM_SYSFS_PS "/battery"

extern volatile sig_atomic_t bm_running;

struct bm_state {
    int32_t level, status, health;
    int ac, usb, wireless, dock, present;
    int32_t voltage_mv, temp_deci_c;
};

int bm_sysfs_read_state(struct bm_state *st);
void bm_state_print(const struct bm_state *st);

const char *bm_status_str(int32_t s);

void bm_print_event(int32_t level, int32_t b_status, int32_t b_health,
                    int ac, int usb, int wireless, int dock,
                    int present, int32_t voltage_mv, int32_t temp_deci_c);

int bm_method_root_binder(void);
int bm_method_netlink_uevent(void);
int bm_method_shell_sysfs_polling(void);
int bm_method_app_broadcastreceiver_method(void);
int bm_method_input_battery_listener(void);
int bm_method_battery_properties_registrar(void);
int bm_method_battery_dump(void);

#endif
