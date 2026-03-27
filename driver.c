#include "protocol.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>

static void strip_newline(char *s) {
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n') s[n - 1] = '\0';
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
    if (fd < 0) return 1;

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

    send_msg(fd, &msg);
    recv_msg(fd, &msg);
    printf("Driver login successful. Initial position: (%.1f, %.1f)\n", start_x, start_y);

    while (1) {
        printf("\n[STATUS: IDLE] Waiting for dispatch... (Type 'exit' to logout): ");
        fflush(stdout);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(fd, &rfds);

        if (select(fd + 1, &rfds, NULL, NULL, NULL) < 0) break;

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char input[64];
            if (fgets(input, sizeof(input), stdin)) {
                strip_newline(input);
                if (strcmp(input, "exit") == 0) {
                    ride_msg_t logout_msg;
                    memset(&logout_msg, 0, sizeof(logout_msg));
                    logout_msg.type = MSG_LOGOUT;
                    send_msg(fd, &logout_msg);
                    printf("Logging out... Goodbye!\n");
                    break;
                }
            }
            continue; 
        }

        if (FD_ISSET(fd, &rfds)) {
            int rc = recv_msg(fd, &msg);
            if (rc <= 0) break;

            if (msg.type == MSG_DISPATCH_JOB) {
                printf("\n--- New Dispatch Received! ---\n");
                printf("Order ID: %d\nPassenger: %s\nPickup: %s\nDropoff: %s\n", 
                       msg.order_id, msg.name, msg.pickup, msg.dropoff);
                printf("Accept ride? (y/n): ");
                fflush(stdout);

                int order_cancelled = 0;
                int accepted = 0;

                while (1) {
                    fd_set afds;
                    FD_ZERO(&afds);
                    FD_SET(STDIN_FILENO, &afds);
                    FD_SET(fd, &afds);

                    select(fd + 1, &afds, NULL, NULL, NULL);

                    if (FD_ISSET(fd, &afds)) {
                        ride_msg_t srv_msg;
                        if (recv_msg(fd, &srv_msg) <= 0) break;
                        if (srv_msg.type == MSG_ERROR) {
                            printf("\n[SYSTEM] %s. Order voided.\n", srv_msg.payload);
                            order_cancelled = 1;
                            break;
                        } else if (srv_msg.type == MSG_FINAL_SETTLEMENT) {
                            float driver_earning = (srv_msg.fare * 0.6f) + srv_msg.tip;
                            printf("\n>>> [INCOME REPORT] Previous Trip Settle Detail <<<\n");
                            printf("Total Fare: $%.2f | Tip: $%.2f | Your Earning: $%.2f\n", 
                                   srv_msg.fare, srv_msg.tip, driver_earning);
                            printf("Accept ride? (y/n): ");
                            fflush(stdout);
                        }
                    }

                    if (FD_ISSET(STDIN_FILENO, &afds)) {
                        char ans[16];
                        if (fgets(ans, sizeof(ans), stdin)) {
                            if (ans[0] == 'y' || ans[0] == 'Y') accepted = 1;
                        }
                        break;
                    }
                }

                if (order_cancelled) continue;

                if (accepted) {
                    ride_msg_t reply;
                    memset(&reply, 0, sizeof(reply));
                    reply.type = MSG_ACCEPT;
                    reply.order_id = msg.order_id;
                    send_msg(fd, &reply);

                    int on_trip = 1;
                    while (on_trip) {
                        fd_set tfds;
                        FD_ZERO(&tfds);
                        FD_SET(STDIN_FILENO, &tfds);
                        FD_SET(fd, &tfds);

                        printf("\n[ON TRIP] [u x y]-Pos | [p]-Arrived Pickup | [s]-Picked Up | [a]-Arrived Dest: ");
                        fflush(stdout);

                        select(fd + 1, &tfds, NULL, NULL, NULL);

                        if (FD_ISSET(fd, &tfds)) {
                            ride_msg_t s_msg;
                            if (recv_msg(fd, &s_msg) <= 0) break;
                            if (s_msg.type == MSG_ERROR) {
                                printf("\n[SYSTEM] Trip cancelled: %s\n", s_msg.payload);
                                on_trip = 0;
                                continue;
                            } else if (s_msg.type == MSG_FINAL_SETTLEMENT) {
                                float driver_earning = (s_msg.fare * 0.6f) + s_msg.tip;
                                printf("\n>>> [INCOME REPORT] Previous Trip Settle Detail <<<\n");
                                printf("Total Fare: $%.2f | Tip: $%.2f | Your Earning: $%.2f\n", 
                                       s_msg.fare, s_msg.tip, driver_earning);
                            }
                        }

                        if (FD_ISSET(STDIN_FILENO, &tfds)) {
                            char cmd[64];
                            if (fgets(cmd, sizeof(cmd), stdin) == NULL) break;
                            strip_newline(cmd);
                            ride_msg_t t_msg;
                            memset(&t_msg, 0, sizeof(t_msg));
                            t_msg.order_id = msg.order_id;

                            if (cmd[0] == 'p') { t_msg.type = MSG_DRIVER_ARRIVED; send_msg(fd, &t_msg); }
                            else if (cmd[0] == 's') { t_msg.type = MSG_PICKUP_CONFIRM; send_msg(fd, &t_msg); }
                            else if (cmd[0] == 'a') { 
                                t_msg.type = MSG_ARRIVED; send_msg(fd, &t_msg); 
                                printf("Trip completed! Returning to IDLE status.\n");
                                on_trip = 0; 
                            }
                            else if (cmd[0] == 'u') {
                                float nx, ny;
                                if (sscanf(cmd, "u %f %f", &nx, &ny) == 2) {
                                    t_msg.type = MSG_UPDATE_POS; t_msg.x = nx; t_msg.y = ny; send_msg(fd, &t_msg);
                                }
                            }
                        }
                    }
                } else {
                    ride_msg_t rej;
                    memset(&rej, 0, sizeof(rej));
                    rej.type = MSG_REJECT;
                    rej.order_id = msg.order_id;
                    send_msg(fd, &rej);
                    printf("Order rejected.\n");
                }
            } else if (msg.type == MSG_FINAL_SETTLEMENT) {
                float driver_earning = (msg.fare * 0.6f) + msg.tip;
                printf("\n>>> [INCOME REPORT] Previous Trip Settle Detail <<<\n");
                printf("Total Fare: $%.2f | Tip: $%.2f | Your Earning: $%.2f\n", 
                       msg.fare, msg.tip, driver_earning);
            }
        }
    }

    close(fd);
    return 0;
}