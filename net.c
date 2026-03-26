#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

ssize_t read_full(int fd, void *buf, size_t count) {
    size_t bytes_read = 0;
    char *p = buf;

    while (bytes_read < count) {
        ssize_t n = read(fd, p + bytes_read, count - bytes_read);
        if (n == 0) {
            return 0;   // disconnected
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        bytes_read += (size_t)n;
    }
    return (ssize_t)bytes_read;
}

ssize_t write_full(int fd, const void *buf, size_t count) {
    size_t bytes_written = 0;
    const char *p = buf;

    while (bytes_written < count) {
        ssize_t n = write(fd, p + bytes_written, count - bytes_written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        bytes_written += (size_t)n;
    }
    return (ssize_t)bytes_written;
}

int send_msg(int fd, const ride_msg_t *msg) {
    return (write_full(fd, msg, sizeof(*msg)) == sizeof(*msg)) ? 0 : -1;
}

int recv_msg(int fd, ride_msg_t *msg) {
    ssize_t n = read_full(fd, msg, sizeof(*msg));
    if (n == 0) {
        return 0;   // disconnected
    }
    if (n < 0) {
        return -1;  // error
    }
    return 1;       // success
}

int setup_server_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 10) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int connect_to_server(const char *ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address\n");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}