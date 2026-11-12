#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_FD 8192
#define MAX_EVENTS 128
#define INPUT_CAP 8192
#define OUTPUT_CAP 65536
#define MAX_CLIENTS 256

typedef struct {
    int active;
    char nick[CNET_MAX_NAME + 1];
    char room[CNET_MAX_ROOM + 1];
    char input[INPUT_CAP];
    size_t input_len;
    char output[OUTPUT_CAP];
    size_t output_len;
    size_t output_sent;
} Client;

static Client clients[MAX_FD];
static int kq = -1;
static int listener = -1;
static volatile sig_atomic_t stopping = 0;

static void on_signal(int sig) {
    (void)sig;
    stopping = 1;
}

static void log_msg(const char *level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stdout, "[%s] %-5s ", stamp, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int change_event(int fd, int16_t filter, uint16_t flags) {
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)fd, filter, flags, 0, 0, NULL);
    return kevent(kq, &ev, 1, NULL, 0, NULL);
}

static int queue_bytes(int fd, const char *data, size_t len) {
    Client *c = &clients[fd];

    if (c->output_sent > 0) {
        size_t remaining = c->output_len - c->output_sent;
        memmove(c->output, c->output + c->output_sent, remaining);
        c->output_len = remaining;
        c->output_sent = 0;
    }

    if (len > OUTPUT_CAP - c->output_len) return -1;
    memcpy(c->output + c->output_len, data, len);
    c->output_len += len;

    if (change_event(fd, EVFILT_WRITE, EV_ADD | EV_ENABLE) < 0)
        return -1;
    return 0;
}

static int sendf(int fd, const char *fmt, ...) {
    char buf[CNET_MAX_LINE + 128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    size_t len = (size_t)n;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    return queue_bytes(fd, buf, len);
}

static int client_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_FD; ++i)
        if (clients[i].active) ++count;
    return count;
}

static int nick_in_use(const char *nick, int except_fd) {
    for (int i = 0; i < MAX_FD; ++i) {
        if (i != except_fd && clients[i].active &&
            strcmp(clients[i].nick, nick) == 0)
            return 1;
    }
    return 0;
}

static void broadcast_room(const char *room, const char *message, int except_fd) {
    for (int i = 0; i < MAX_FD; ++i) {
        if (clients[i].active && i != except_fd &&
            strcmp(clients[i].room, room) == 0) {
            if (queue_bytes(i, message, strlen(message)) < 0)
                log_msg("WARN", "Output queue full for fd=%d", i);
        }
    }
}

static void remove_client(int fd, const char *reason) {
    if (fd < 0 || fd >= MAX_FD || !clients[fd].active) return;

    char msg[160];
    snprintf(msg, sizeof(msg), "* %s left (%s)\n",
             clients[fd].nick, reason ? reason : "disconnected");
    char room[CNET_MAX_ROOM + 1];
    snprintf(room, sizeof(room), "%s", clients[fd].room);

    (void)change_event(fd, EVFILT_READ, EV_DELETE);
    (void)change_event(fd, EVFILT_WRITE, EV_DELETE);
    close(fd);
    memset(&clients[fd], 0, sizeof(clients[fd]));
    broadcast_room(room, msg, -1);
    log_msg("INFO", "Client fd=%d disconnected", fd);
}

static void list_users(int fd) {
    char line[256];
    (void)sendf(fd, "Users in #%s:\n", clients[fd].room);
    for (int i = 0; i < MAX_FD; ++i) {
        if (clients[i].active &&
            strcmp(clients[i].room, clients[fd].room) == 0) {
            snprintf(line, sizeof(line), " - %s%s\n",
                     clients[i].nick, i == fd ? " (you)" : "");
            if (queue_bytes(fd, line, strlen(line)) < 0) return;
        }
    }
}

static void list_rooms(int fd) {
    char seen[MAX_CLIENTS][CNET_MAX_ROOM + 1];
    int used = 0;
    (void)sendf(fd, "Active rooms:\n");
    for (int i = 0; i < MAX_FD; ++i) {
        if (!clients[i].active) continue;
        int exists = 0;
        for (int j = 0; j < used; ++j)
            if (strcmp(seen[j], clients[i].room) == 0) exists = 1;
        if (!exists && used < MAX_CLIENTS) {
            snprintf(seen[used++], sizeof(seen[0]), "%s", clients[i].room);
            (void)sendf(fd, " - #%s\n", clients[i].room);
        }
    }
}

static void handle_line(int fd, char *line) {
    Client *c = &clients[fd];

    while (*line == ' ' || *line == '\t') ++line;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                   line[len - 1] == '\t'))
        line[--len] = '\0';
    if (!len) return;

    if (line[0] != '/') {
        char msg[CNET_MAX_LINE + 128];
        int n = snprintf(msg, sizeof(msg), "[#%s] %s: %s\n",
                         c->room, c->nick, line);
        if (n > 0) broadcast_room(c->room, msg, -1);
        return;
    }

    char *arg = line + 1;
    char *space = strchr(arg, ' ');
    char *rest = NULL;
    if (space) {
        *space = '\0';
        rest = space + 1;
        while (*rest == ' ') ++rest;
    }

    if (strcmp(arg, "help") == 0) {
        (void)sendf(fd,
            "Commands: /nick NAME, /join ROOM, /msg NICK TEXT, "
            "/who, /rooms, /help, /quit\n");
    } else if (strcmp(arg, "nick") == 0) {
        if (!rest || !cnet_valid_name(rest, CNET_MAX_NAME)) {
            (void)sendf(fd, "Usage: /nick NAME (1-%d letters/digits/_/-)\n",
                        CNET_MAX_NAME);
        } else if (nick_in_use(rest, fd)) {
            (void)sendf(fd, "Nickname is already in use.\n");
        } else {
            char old[CNET_MAX_NAME + 1];
            snprintf(old, sizeof(old), "%s", c->nick);
            snprintf(c->nick, sizeof(c->nick), "%s", rest);
            char msg[128];
            snprintf(msg, sizeof(msg), "* %s is now known as %s\n",
                     old, c->nick);
            broadcast_room(c->room, msg, -1);
        }
    } else if (strcmp(arg, "join") == 0) {
        if (!rest || !cnet_valid_name(rest, CNET_MAX_ROOM)) {
            (void)sendf(fd, "Usage: /join ROOM\n");
        } else {
            char old[CNET_MAX_ROOM + 1];
            snprintf(old, sizeof(old), "%s", c->room);
            (void)sendf(fd, "* You joined #%s\n", rest);
            snprintf(c->room, sizeof(c->room), "%s", rest);
            char msg[128];
            snprintf(msg, sizeof(msg), "* %s joined #%s\n", c->nick, rest);
            broadcast_room(rest, msg, fd);
            snprintf(msg, sizeof(msg), "* %s left #%s\n", c->nick, old);
            broadcast_room(old, msg, fd);
        }
    } else if (strcmp(arg, "msg") == 0) {
        if (!rest) {
            (void)sendf(fd, "Usage: /msg NICK TEXT\n");
        } else {
            char *sep = strchr(rest, ' ');
            if (!sep || sep == rest || !sep[1]) {
                (void)sendf(fd, "Usage: /msg NICK TEXT\n");
            } else {
                *sep = '\0';
                int target = -1;
                for (int i = 0; i < MAX_FD; ++i)
                    if (clients[i].active &&
                        strcmp(clients[i].nick, rest) == 0) {
                        target = i;
                        break;
                    }
                if (target < 0) {
                    (void)sendf(fd, "User not found.\n");
                } else {
                    (void)sendf(target, "[PM from %s] %s\n", c->nick, sep + 1);
                    (void)sendf(fd, "[PM to %s] %s\n", rest, sep + 1);
                }
            }
        }
    } else if (strcmp(arg, "who") == 0) {
        list_users(fd);
    } else if (strcmp(arg, "rooms") == 0) {
        list_rooms(fd);
    } else if (strcmp(arg, "quit") == 0) {
        remove_client(fd, "quit command");
    } else {
        (void)sendf(fd, "Unknown command. Type /help.\n");
    }
}

static void read_client(int fd) {
    Client *c = &clients[fd];
    char temp[4096];

    for (;;) {
        ssize_t n = recv(fd, temp, sizeof(temp), 0);
        if (n > 0) {
            if ((size_t)n > INPUT_CAP - c->input_len) {
                remove_client(fd, "input limit exceeded");
                return;
            }
            memcpy(c->input + c->input_len, temp, (size_t)n);
            c->input_len += (size_t)n;

            size_t consumed = 0;
            for (size_t i = 0; i < c->input_len; ++i) {
                if (c->input[i] == '\n') {
                    c->input[i] = '\0';
                    char line[CNET_MAX_LINE + 1];
                    size_t line_len = i - consumed;
                    if (line_len > CNET_MAX_LINE) {
                        remove_client(fd, "line too long");
                        return;
                    }
                    memcpy(line, c->input + consumed, line_len);
                    line[line_len] = '\0';
                    handle_line(fd, line);
                    if (!clients[fd].active) return;
                    consumed = i + 1;
                }
            }

            if (consumed) {
                size_t remaining = c->input_len - consumed;
                memmove(c->input, c->input + consumed, remaining);
                c->input_len = remaining;
            }
            if (c->input_len > CNET_MAX_LINE) {
                remove_client(fd, "line too long");
                return;
            }
        } else if (n == 0) {
            remove_client(fd, "peer closed");
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            remove_client(fd, "read error");
            return;
        }
    }
}

static void write_client(int fd) {
    Client *c = &clients[fd];

    while (c->output_sent < c->output_len) {
        ssize_t n = send(fd, c->output + c->output_sent,
                         c->output_len - c->output_sent, 0);
        if (n > 0) {
            c->output_sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            remove_client(fd, "write error");
            return;
        }
    }

    c->output_len = 0;
    c->output_sent = 0;
    if (change_event(fd, EVFILT_WRITE, EV_DELETE) < 0 && errno != ENOENT)
        remove_client(fd, "write event error");
}

static void accept_clients(void) {
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_len);

        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            log_msg("ERROR", "accept: %s", strerror(errno));
            return;
        }

        if (fd >= MAX_FD || client_count() >= MAX_CLIENTS ||
            set_nonblocking(fd) < 0) {
            close(fd);
            continue;
        }

        int no_sigpipe = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                         &no_sigpipe, sizeof(no_sigpipe));

        Client *c = &clients[fd];
        memset(c, 0, sizeof(*c));
        c->active = 1;
        snprintf(c->nick, sizeof(c->nick), "Guest%d", fd);
        snprintf(c->room, sizeof(c->room), "lobby");

        if (change_event(fd, EVFILT_READ, EV_ADD | EV_ENABLE) < 0) {
            close(fd);
            memset(c, 0, sizeof(*c));
            continue;
        }

        char ip[INET_ADDRSTRLEN] = "?";
        (void)inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        log_msg("INFO", "Client %s:%u connected as %s",
                ip, ntohs(peer.sin_port), c->nick);
        (void)sendf(fd, "Welcome to C-Net Server. Your nick: %s\n"
                        "Type /help for commands.\n", c->nick);
        (void)sendf(fd, "* Joined #lobby\n");
        char msg[128];
        snprintf(msg, sizeof(msg), "* %s joined #lobby\n", c->nick);
        broadcast_room("lobby", msg, fd);
    }
}

int main(int argc, char **argv) {
    int port = CNET_PORT_DEFAULT;
    if (argc > 1) {
        char *end = NULL;
        long parsed = strtol(argv[1], &end, 10);
        if (!end || *end || parsed < 1 || parsed > 65535) {
            fprintf(stderr, "Usage: %s [port]\n", argv[0]);
            return EXIT_FAILURE;
        }
        port = (int)parsed;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int yes = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* local-only by default */

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listener, 128) < 0 ||
        set_nonblocking(listener) < 0) {
        perror("bind/listen/nonblocking");
        close(listener);
        return EXIT_FAILURE;
    }

    kq = kqueue();
    if (kq < 0) {
        perror("kqueue");
        close(listener);
        return EXIT_FAILURE;
    }

    if (change_event(listener, EVFILT_READ, EV_ADD | EV_ENABLE) < 0) {
        perror("register listener");
        close(kq);
        close(listener);
        return EXIT_FAILURE;
    }

    log_msg("INFO", "C-Net Server listening on 127.0.0.1:%d", port);
    struct kevent events[MAX_EVENTS];

    while (!stopping) {
        struct timespec timeout = {1, 0};
        int n = kevent(kq, NULL, 0, events, MAX_EVENTS, &timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_msg("ERROR", "kevent: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = (int)events[i].ident;
            if (fd == listener) {
                accept_clients();
                continue;
            }
            if (fd < 0 || fd >= MAX_FD || !clients[fd].active)
                continue;

            if (events[i].flags & EV_ERROR) {
                remove_client(fd, "event error");
                continue;
            }

            if (events[i].filter == EVFILT_READ)
                read_client(fd);
            else if (events[i].filter == EVFILT_WRITE &&
                     clients[fd].active)
                write_client(fd);
        }
    }

    log_msg("INFO", "Shutting down");
    for (int fd = 0; fd < MAX_FD; ++fd)
        if (clients[fd].active) remove_client(fd, "server shutdown");
    close(kq);
    close(listener);
    return EXIT_SUCCESS;
}
