#include "protocol.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static void strip_newline(char *s) {
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n') {
        s[n - 1] = '\0';
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <port> <name>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *name = argv[3];

    int fd = connect_to_server(server_ip, port);
    if (fd < 0) {
        return 1;
    }

    srand(time(NULL) ^ getpid());
    float start_x = (float)(rand() % 100);
    float start_y = (float)(rand() % 100);

    ride_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LOGIN;
    msg.role = ROLE_DRIVER;
    msg.x = start_x;
    msg.y = start_y;
    strncpy(msg.name, name, NAME_LEN - 1);

    if (send_msg(fd, &msg) < 0) {
        perror("send login");
        close(fd);
        return 1;
    }

    int rc = recv_msg(fd, &msg);
    if (rc <= 0 || msg.type != MSG_LOGIN_ACK) {
        fprintf(stderr, "Login failed\n");
        close(fd);
        return 1;
    }

    printf("Driver login successful. Initial position: (%.1f, %.1f)\n", start_x, start_y);

    while (1) {
        printf("\n[STATUS: IDLE] Waiting for new dispatch...\n");
        rc = recv_msg(fd, &msg);
        if (rc == 0) {
            printf("Server disconnected.\n");
            break;
        }
        if (rc < 0) {
            perror("recv_msg");
            break;
        }

        if (msg.type == MSG_DISPATCH_JOB) {
            printf("\n--- New Dispatch Received! ---\n");
            printf("Order ID: %d\n", msg.order_id);
            printf("Passenger: %s\n", msg.name);
            printf("Pickup: %s\n", msg.pickup);
            printf("Dropoff: %s\n", msg.dropoff);

            char answer[16];
            printf("Accept ride? (y/n): ");
            if (fgets(answer, sizeof(answer), stdin) == NULL) {
                break;
            }

            if (answer[0] == 'y' || answer[0] == 'Y') {
                ride_msg_t reply;
                memset(&reply, 0, sizeof(reply));
                reply.type = MSG_ACCEPT;
                reply.order_id = msg.order_id;
                send_msg(fd, &reply);

                while (1) {
                    char cmd[64];
                    printf("\n[ON TRIP] Enter command [u x y] or [a]: ");
                    if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
                        break;
                    }
                    strip_newline(cmd);

                    if (cmd[0] == 'a') {
                        ride_msg_t arrived;
                        memset(&arrived, 0, sizeof(arrived));
                        arrived.type = MSG_ARRIVED;
                        arrived.order_id = msg.order_id;
                        send_msg(fd, &arrived);
                        printf("Trip completed. Returning to IDLE status.\n");
                        break;
                    } else if (cmd[0] == 'u') {
                        float x, y;
                        if (sscanf(cmd, "u %f %f", &x, &y) == 2) {
                            ride_msg_t update;
                            memset(&update, 0, sizeof(update));
                            update.type = MSG_UPDATE_POS;
                            update.order_id = msg.order_id;
                            update.x = x;
                            update.y = y;
                            send_msg(fd, &update);
                            printf("Position updated to (%.1f, %.1f).\n", x, y);
                        } else {
                            printf("Invalid format. Example: u 10 20\n");
                        }
                    } else {
                        printf("Unknown command.\n");
                    }
                }

            } else {
                ride_msg_t reply;
                memset(&reply, 0, sizeof(reply));
                reply.type = MSG_REJECT;
                reply.order_id = msg.order_id;
                send_msg(fd, &reply);
                printf("Order rejected. Returning to IDLE status.\n");
            }

        } else if (msg.type == MSG_ERROR) {
            printf("\nERROR: %s\n", msg.payload);
        } else {
            printf("\nReceived message type %d\n", msg.type);
        }
    }

    close(fd);
    return 0;
}