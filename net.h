#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <sys/types.h>
#include "protocol.h"

ssize_t read_full(int fd, void *buf, size_t count);
ssize_t write_full(int fd, const void *buf, size_t count);

int send_msg(int fd, const ride_msg_t *msg);
int recv_msg(int fd, ride_msg_t *msg);

int setup_server_socket(uint16_t port);
int connect_to_server(const char *ip, uint16_t port);

#endif