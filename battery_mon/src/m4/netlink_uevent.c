/*
 * netlink_uevent.c - event-driven method: block on the kernel uevent socket
 * (NETLINK_KOBJECT_UEVENT) and read sysfs when a power_supply event arrives.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bm_common.h"

static int is_power_supply_event(const char *msg) {
    for (const char *p = msg; *p; p += strlen(p) + 1)
        if (strcmp(p, "SUBSYSTEM=power_supply") == 0)
            return 1;
    return 0;
}

int bm_method_netlink_uevent(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        perror("socket(NETLINK_KOBJECT_UEVENT, needs root)");
        return -1;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = (uint32_t)getpid();
    sa.nl_groups = 1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind(NETLINK_KOBJECT_UEVENT, needs root)");
        close(fd);
        return -1;
    }

    struct bm_state cur, prev;
    if (bm_sysfs_read_state(&cur) < 0) {
        fprintf(stderr, "[info] battery sysfs unreadable (needs root)\n");
        close(fd);
        return -1;
    }
    fprintf(stderr, "[info] monitoring via netlink uevents (%s)\n", BM_SYSFS_BAT);
    bm_state_print(&cur);
    prev = cur;

    char buf[8192];
    while (bm_running) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            perror("recv");
            break;
        }
        buf[n] = '\0';

        if (!is_power_supply_event(buf))
            continue;
        if (bm_sysfs_read_state(&cur) < 0)
            continue;
        if (memcmp(&prev, &cur, sizeof(cur)) != 0) {
            bm_state_print(&cur);
            prev = cur;
        }
    }

    close(fd);
    return 0;
}
