#ifndef CNET_COMMON_H
#define CNET_COMMON_H

#include <stddef.h>

#define CNET_PORT_DEFAULT 8080
#define CNET_MAX_LINE 2048
#define CNET_MAX_NAME 32
#define CNET_MAX_ROOM 32

int cnet_valid_name(const char *s, size_t max_len);
int cnet_send_all(int fd, const void *data, size_t len);

#endif
