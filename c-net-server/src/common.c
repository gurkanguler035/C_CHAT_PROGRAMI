#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

int cnet_valid_name(const char *s, size_t max_len) {
    if (!s || !*s) return 0;

    size_t n = 0;
    for (; s[n]; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (n >= max_len) return 0;
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return n > 0;
}

int cnet_send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent_total = 0;

    while (sent_total < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p + sent_total, len - sent_total, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, p + sent_total, len - sent_total, 0);
#endif
        if (n > 0) {
            sent_total += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}
