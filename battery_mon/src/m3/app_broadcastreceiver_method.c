/*
 * app_broadcastreceiver_method.c - app method: abstract socket, ContentProvider,
 * then a shared file with inotify and polling fallback.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bm_common.h"

#define APP_SOCKET "com.battmon.battery"
#define APP_URI "content://com.battmon.battery/stream"
#define APP_FILE "/sdcard/Android/media/com.battmon/battery.log"
#define BM_LINE_MAX 1024

struct line_reader {
    char buf[BM_LINE_MAX];
    size_t used;
};

/* The relay is untrusted local data; keep terminal escape bytes out. */
static void sanitize(char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] < 0x20 && s[i] != '\t') || s[i] == 0x7f)
            s[i] = '?';
}

static int emit_line(char *line, int require_battery) {
    size_t n = strlen(line);
    if (n > 0 && line[n - 1] == '\r')
        line[--n] = '\0';
    if (require_battery && strncmp(line, "battery=", 8) != 0)
        return 0;
    sanitize(line, n);
    printf("%s\n", line);
    fflush(stdout);
    return 1;
}

static int feed(struct line_reader *r, const char *data, size_t n,
                int require_battery) {
    int emitted = 0;
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            r->buf[r->used] = '\0';
            if (r->used > 0 && emit_line(r->buf, require_battery))
                emitted = 1;
            r->used = 0;
        } else if (r->used + 1 < sizeof(r->buf)) {
            r->buf[r->used++] = c;
        } else {
            r->buf[r->used] = '\0';
            if (emit_line(r->buf, require_battery))
                emitted = 1;
            r->used = 0;
        }
    }
    return emitted;
}

static int stream_fd(int fd, int first_timeout, int require_battery) {
    struct line_reader r;
    memset(&r, 0, sizeof r);
    int emitted = 0;
    char buf[1024];

    for (;;) {
        if (!bm_running)
            break;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int timeout = emitted ? 1000 : first_timeout;
        int pr = poll(&pfd, 1, timeout);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (pr == 0) {
            if (!emitted)
                break;
            continue;
        }

        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        }
        if (n == 0)
            break;
        if (feed(&r, buf, (size_t)n, require_battery))
            emitted = 1;
    }

    if (r.used > 0) {
        r.buf[r.used] = '\0';
        if (emit_line(r.buf, require_battery))
            emitted = 1;
    }
    return emitted;
}

static int try_socket(void) {
    struct sockaddr_un addr;
    size_t name_len = strlen(APP_SOCKET);

    if (name_len == 0 || name_len + 1 > sizeof(addr.sun_path)) {
        fprintf(stderr, "invalid app socket name %s\n", APP_SOCKET);
        return 0;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, APP_SOCKET, name_len);
    socklen_t addr_len =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 0;
    }

    if (connect(fd, (struct sockaddr *)&addr, addr_len) < 0) {
        int err = errno;
        close(fd);
        fprintf(stderr, "[info] abstract socket unavailable: %s\n",
                strerror(err));
        return 0;
    }

    fprintf(stderr, "[info] monitoring via abstract socket %s\n", APP_SOCKET);
    int emitted = stream_fd(fd, 3000, 0);
    close(fd);
    return emitted;
}

static int try_provider(void) {
    /*
     * shortcut: Android denies ACCESS_CONTENT_PROVIDERS_EXTERNALLY to
     * untrusted app UIDs, so only root/shell can use the content CLI.
     */
    if (getuid() != 0 && getuid() != 2000)
        return 0;

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        perror("pipe2");
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }

    if (pid == 0) {
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipefd[0]);
        close(pipefd[1]);
        setenv("PATH", "/system/bin:/system/xbin", 1);
        execl("/system/bin/content", "content", "read", "--uri", APP_URI,
              (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    fprintf(stderr, "[info] monitoring via ContentProvider %s\n", APP_URI);
    int emitted = stream_fd(pipefd[0], 3000, 1);
    close(pipefd[0]);
    if (!emitted || !bm_running)
        kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
    if (!emitted)
        fprintf(stderr, "[info] ContentProvider unavailable\n");
    return emitted;
}

static int open_shared(void) {
    int announced = 0;
    for (;;) {
        if (!bm_running)
            return -1;
        int fd = open(APP_FILE, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
            return fd;
        if (!announced) {
            fprintf(stderr, "[info] waiting for shared file %s: %s\n",
                    APP_FILE, strerror(errno));
            announced = 1;
        }
        (void)poll(NULL, 0, 1000);
    }
}

static int read_last_line(int fd, char *out, size_t cap, off_t *end_out) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end_out != NULL)
        *end_out = end;
    if (end <= 0)
        return 0;

    size_t want = (size_t)end;
    if (want > cap)
        want = cap;
    if (lseek(fd, end - (off_t)want, SEEK_SET) < 0)
        return 0;

    char buf[BM_LINE_MAX];
    if (want > sizeof(buf))
        want = sizeof(buf);
    ssize_t n = read(fd, buf, want);
    if (n <= 0)
        return 0;

    size_t len = (size_t)n;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        len--;
    size_t start = len;
    while (start > 0 && buf[start - 1] != '\n')
        start--;
    size_t copy = len - start;
    if (copy >= cap)
        copy = cap - 1;
    memcpy(out, buf + start, copy);
    out[copy] = '\0';
    return copy > 0;
}

static int try_file(void) {
    int fd = open_shared();
    if (fd < 0)
        return 0;

    char line[BM_LINE_MAX];
    off_t offset = 0;
    int emitted = 0;
    if (read_last_line(fd, line, sizeof line, &offset)) {
        if (emit_line(line, 0))
            emitted = 1;
    }
    if (offset < 0)
        offset = 0;

    int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd >= 0) {
        if (inotify_add_watch(ifd, APP_FILE,
                              IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE_SELF |
                                  IN_DELETE_SELF) < 0) {
            close(ifd);
            ifd = -1;
        }
    }

    fprintf(stderr, "[info] monitoring via shared file %s (%s)\n", APP_FILE,
            ifd >= 0 ? "inotify" : "polling");

    struct line_reader r;
    memset(&r, 0, sizeof r);
    char buf[1024];

    while (bm_running) {
        if (ifd >= 0) {
            struct pollfd pfd;
            pfd.fd = ifd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 1000);
            if (pr > 0) {
                char events[4096];
                ssize_t n = read(ifd, events, sizeof events);
                if (n < 0 && errno != EINTR && errno != EAGAIN) {
                    close(ifd);
                    ifd = -1;
                }
            } else if (pr < 0 && errno != EINTR) {
                close(ifd);
                ifd = -1;
            }
        } else {
            (void)poll(NULL, 0, 1000);
        }

        struct stat st;
        if (fstat(fd, &st) < 0)
            break;

        if (st.st_size < offset) {
            offset = 0;
            r.used = 0;
        }
        if (st.st_size > offset) {
            if (lseek(fd, offset, SEEK_SET) < 0)
                break;
            ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
            offset += (off_t)n;
            if (feed(&r, buf, (size_t)n, 0))
                emitted = 1;
        }
    }

    if (ifd >= 0)
        close(ifd);
    close(fd);
    return emitted;
}

int bm_method_app_broadcastreceiver_method(void) {
    if (try_socket())
        return 0;
    if (!bm_running)
        return 0;
    if (try_provider())
        return 0;
    if (!bm_running)
        return 0;
    if (try_file())
        return 0;
    if (!bm_running)
        return 0;
    fprintf(stderr, "app method: no transport available\n");
    return -1;
}
