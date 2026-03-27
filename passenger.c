#include "protocol.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    ride_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LOGIN;
    msg.role = ROLE_PASSENGER;
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

    printf("Passenger login successful.\n");

    while (1) {
        char pickup[LOC_LEN];
        char dropoff[LOC_LEN];

        printf("\n--- New Ride Request ---\n");
        printf("Enter pickup location (or 'exit' to quit): ");
        if (fgets(pickup, sizeof(pickup), stdin) == NULL) break;
        strip_newline(pickup);
        
        if (strcmp(pickup, "exit") == 0) break;

        printf("Enter dropoff location: ");
        if (fgets(dropoff, sizeof(dropoff), stdin) == NULL) break;
        strip_newline(dropoff);

        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_RIDE_REQUEST;
        strncpy(msg.pickup, pickup, LOC_LEN - 1);
        strncpy(msg.dropoff, dropoff, LOC_LEN - 1);

        if (send_msg(fd, &msg) < 0) {
            perror("send request");
            break;
        }

        int ride_active = 1;
        while (ride_active) {
            rc = recv_msg(fd, &msg);
            if (rc == 0) {
                printf("Server disconnected.\n");
                close(fd);
                return 0;
            }
            if (rc < 0) {
                perror("recv_msg");
                close(fd);
                return 1;
            }

            if (msg.type == MSG_MATCHED) {
                printf("Matched with driver: %s (order %d)\n", msg.name, msg.order_id);
            } else if (msg.type == MSG_UPDATE_POS) {
                printf("Driver %s position update: (%.1f, %.1f)\n", msg.name, msg.x, msg.y);
            } else if (msg.type == MSG_BILL) {
                float base_fare = msg.fare;
                printf("\n>>> %s\n", msg.payload);
                
                printf("\nSelect Tip Percentage:\n");
                printf("1) No Tip (0%%)\n");
                printf("2) 5%%\n");
                printf("3) 10%%\n");
                printf("4) 12%%\n");
                printf("5) Other Amount\n");
                printf("Selection: ");
                
                char choice[16];
                float tip_val = 0.0f;
                if (fgets(choice, sizeof(choice), stdin) != NULL) {
                    int c = atoi(choice);
                    if (c == 2) tip_val = base_fare * 0.05f;
                    else if (c == 3) tip_val = base_fare * 0.10f;
                    else if (c == 4) tip_val = base_fare * 0.12f;
                    else if (c == 5) {
                        printf("Enter custom tip amount: ");
                        char custom[16];
                        if (fgets(custom, sizeof(custom), stdin) != NULL) {
                            tip_val = atof(custom);
                        }
                    }
                }

                printf("Final Total: $%.2f (Fare: $%.2f + Tip: $%.2f)\n", 
                       base_fare + tip_val, base_fare, tip_val);

                ride_msg_t tip_msg;
                memset(&tip_msg, 0, sizeof(tip_msg));
                tip_msg.type = MSG_TIP_SELECTION;
                tip_msg.order_id = msg.order_id;
                tip_msg.fare = base_fare;
                tip_msg.tip = tip_val;
                send_msg(fd, &tip_msg);
                
                ride_active = 0;
            } else if (msg.type == MSG_ERROR) {
                printf("ERROR: %s\n", msg.payload);
                ride_active = 0;
            } else {
                printf("Received message type %d\n", msg.type);
            }
        }
    }

    close(fd);
    return 0;
}