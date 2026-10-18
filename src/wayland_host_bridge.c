#include "../include/wayland_host_bridge.h"
#include "../include/log.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <wayland-client.h>

#define GSR_UI_UNIX_SOCKET_DOMAIN_FD 3

// Returns the length of the app path, or 0 on failure
static size_t get_flatpak_app_path(char *path, size_t max_path) {
    if(max_path == 0)
        return 0;

    path[0] = '\0';
    FILE *f = fopen("/.flatpak-info", "rb");
    if(!f)
        return 0;

    char line[4096];

    while(fgets(line, sizeof(line), f)) {
        size_t line_len = strlen(line);
        if(line_len > 0 && line[line_len - 1] == '\n') {
            line[line_len - 1] = '\0';
            line_len -= 1;
        }

        if(line_len >= 9 && memcmp(line, "app-path=", 9) == 0) {
            line_len -= 9;
            snprintf(path, max_path, "%.*s", (int)line_len, line + 9);
            break;
        }
    }

    fclose(f);
    return strlen(path);
}

static int recv_fd(int sock) {
    char dummy = 0;
    struct iovec iov = { &dummy, 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    const ssize_t n = recvmsg(sock, &msg, 0);
    if(n <= 0)
        return -1;

    for(struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if(c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
            return fd;
        }
    }
    return -1;
}

static struct wl_display* connect_via_bridge(const char *bridge_host_path) {
    const char *wayland_display = getenv("WAYLAND_DISPLAY");

    int sv[2];
    if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        perror("WaylandHostBridge: socketpair");
        return NULL;
    }

    const pid_t pid = fork();
    if(pid < 0) {
        perror("WaylandHostBridge: fork");
        close(sv[0]);
        close(sv[1]);
        return NULL;
    }

    if(pid == 0) {
        close(sv[0]);
        if(sv[1] != GSR_UI_UNIX_SOCKET_DOMAIN_FD) {
            dup2(sv[1], GSR_UI_UNIX_SOCKET_DOMAIN_FD);
            close(sv[1]);
        }
        const int flags = fcntl(GSR_UI_UNIX_SOCKET_DOMAIN_FD, F_GETFD);
        if(flags >= 0)
            fcntl(GSR_UI_UNIX_SOCKET_DOMAIN_FD, F_SETFD, flags & ~FD_CLOEXEC);

        char forward_fd_arg[32];
        snprintf(forward_fd_arg, sizeof(forward_fd_arg), "--forward-fd=%d", GSR_UI_UNIX_SOCKET_DOMAIN_FD);

        execlp("flatpak-spawn", "flatpak-spawn", "--host", forward_fd_arg, bridge_host_path, wayland_display, (char*)NULL);
        _exit(127);
    }

    close(sv[1]);
    const int wayland_fd = recv_fd(sv[0]);
    close(sv[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if(wayland_fd < 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "WaylandHostBridge: gsr-wayland-bridge did not return a wayland fd");
        return NULL;
    }

    struct wl_display *dpy = wl_display_connect_to_fd(wayland_fd);
    if(!dpy) {
        close(wayland_fd);
        gsr_log(GSR_LOG_LEVEL_ERROR, "WaylandHostBridge: wl_display_connect_to_fd failed");
        return NULL;
    }
    return dpy;
}

struct wl_display* wayland_connect_to_host() {
    char gsr_wayland_bridge_path[PATH_MAX];
    const size_t path_len = get_flatpak_app_path(gsr_wayland_bridge_path, sizeof(gsr_wayland_bridge_path));
    const char *gsr_wayland_bridge_suffix = "/bin/gsr-wayland-bridge";
    const size_t suffix_len = strlen(gsr_wayland_bridge_suffix);

    if(path_len > 0 && path_len + suffix_len + 1 <= sizeof(gsr_wayland_bridge_path)) {
        memcpy(gsr_wayland_bridge_path + path_len, gsr_wayland_bridge_suffix, suffix_len);
        gsr_wayland_bridge_path[path_len + suffix_len] = '\0';

        struct wl_display *dpy = connect_via_bridge(gsr_wayland_bridge_path);
        if(dpy)
            return dpy;

        gsr_log(GSR_LOG_LEVEL_WARNING, "WaylandHostBridge: falling back to sandboxed wl_display_connect");
    }

    return wl_display_connect(NULL);
}
