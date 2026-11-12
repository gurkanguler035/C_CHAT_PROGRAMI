#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int sockfd = -1;

static void *receiver_thread(void *arg) {
    (void)arg;
    char buf[2048];

    for (;;) {
        ssize_t n = recv(sockfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            fputs(buf, stdout);
            fflush(stdout);
        } else {
            if (n == 0) fprintf(stderr, "\nServer disconnected.\n");
            else perror("recv");
            break;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = CNET_PORT_DEFAULT;

    if (argc > 2) {
        char *end = NULL;
        long p = strtol(argv[2], &end, 10);
        if (!end || *end || p < 1 || p > 65535) {
            fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
            return EXIT_FAILURE;
        }
        port = (int)p;
    }

    signal(SIGPIPE, SIG_IGN);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Host must be an IPv4 address.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    pthread_t receiver;
    if (pthread_create(&receiver, NULL, receiver_thread, NULL) != 0) {
        perror("pthread_create");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected. Type /help for commands.\n");
    char line[CNET_MAX_LINE + 2];

    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len == 0) continue;
        if (line[len - 1] != '\n') {
            fprintf(stderr, "Input line too long; discarded.\n");
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            continue;
        }

        if (cnet_send_all(sockfd, line, len) < 0) {
            perror("send");
            break;
        }
        if (strcmp(line, "/quit\n") == 0) break;
    }

    shutdown(sockfd, SHUT_RDWR);
    pthread_join(receiver, NULL);
    close(sockfd);
    return EXIT_SUCCESS;
}
