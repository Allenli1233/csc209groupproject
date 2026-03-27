#include "protocol.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>  
#include <netinet/in.h>  
#include <math.h>

#define MAX_CLIENTS 32
#define FARE_PER_UNIT 0.8f

typedef struct {
    char name[LOC_LEN];
    float x;
    float y;
} location_map_t;

location_map_t city_map[] = {
    {"Airport", 10.0, 10.0},
    {"Hotel", 50.0, 50.0},
    {"Station", 20.0, 80.0},
    {"University", 70.0, 10.0},
    {"Mall", 90.0, 90.0}
};

#define MAP_SIZE (sizeof(city_map) / sizeof(location_map_t))

typedef struct {
    int fd;
    role_t role;
    status_t status;
    char name[NAME_LEN];
    float x;
    float y;
    int current_order_id;
    int peer_fd;
    float trip_dist;
} client_t;

static client_t clients[MAX_CLIENTS];
static int next_order_id = 1;

static void reset_client(client_t *c) {
    c->fd = -1;
    c->role = ROLE_NONE;
    c->status = STATUS_IDLE;
    c->name[0] = '\0';
    c->x = 0.0f;
    c->y = 0.0f;
    c->current_order_id = 0;
    c->peer_fd = -1;
    c->trip_dist = 0.0f;
}

static void init_clients(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        reset_client(&clients[i]);
    }
}

static int add_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == -1) {
            reset_client(&clients[i]);
            clients[i].fd = fd;
            return i;
        }
    }
    return -1;
}

static int find_client_by_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            return i;
        }
    }
    return -1;
}

static void set_client_idle(int idx) {
    clients[idx].status = STATUS_IDLE;
    clients[idx].current_order_id = 0;
    clients[idx].peer_fd = -1;
    clients[idx].trip_dist = 0.0f;
}

static void send_error_msg(int fd, const char *text) {
    ride_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_ERROR;
    strncpy(msg.payload, text, PAYLOAD_LEN - 1);
    send_msg(fd, &msg);
}

static void send_login_ack(int fd) {
    ride_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LOGIN_ACK;
    send_msg(fd, &msg);
}

static void remove_client(int idx) {
    if (idx < 0 || idx >= MAX_CLIENTS || clients[idx].fd == -1) {
        return;
    }
    int peer_fd = clients[idx].peer_fd;
    if (peer_fd != -1) {
        int peer_idx = find_client_by_fd(peer_fd);
        if (peer_idx != -1) {
            send_error_msg(clients[peer_idx].fd, "Matched peer disconnected.");
            set_client_idle(peer_idx);
        }
    }
    close(clients[idx].fd);
    reset_client(&clients[idx]);
}

static void handle_login(int idx, const ride_msg_t *msg) {
    clients[idx].role = (role_t)msg->role;
    clients[idx].status = STATUS_IDLE;
    strncpy(clients[idx].name, msg->name, NAME_LEN - 1);
    clients[idx].name[NAME_LEN - 1] = '\0';
    if (clients[idx].role == ROLE_DRIVER) {
        clients[idx].x = msg->x;
        clients[idx].y = msg->y;
    }
    printf("Client fd=%d logged in as %s (%s) at pos (%.1f, %.1f)\n",
           clients[idx].fd,
           clients[idx].name,
           clients[idx].role == ROLE_DRIVER ? "driver" : "passenger",
           clients[idx].x, clients[idx].y);
    send_login_ack(clients[idx].fd);
}

static void handle_ride_request(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_PASSENGER) {
        send_error_msg(clients[idx].fd, "Only passengers can request rides.");
        return;
    }
    if (clients[idx].status != STATUS_IDLE) {
        send_error_msg(clients[idx].fd, "Passenger is not idle.");
        return;
    }

    float px = -1.0f, py = -1.0f;
    float dx = -1.0f, dy = -1.0f;

    for (size_t i = 0; i < MAP_SIZE; i++) {
        if (strcmp(city_map[i].name, msg->pickup) == 0) {
            px = city_map[i].x;
            py = city_map[i].y;
        }
        if (strcmp(city_map[i].name, msg->dropoff) == 0) {
            dx = city_map[i].x;
            dy = city_map[i].y;
        }
    }

    if (px < 0 || dx < 0) {
        send_error_msg(clients[idx].fd, "Unknown pickup or dropoff location.");
        return;
    }

    float trip_dist = sqrtf(powf(dx - px, 2) + powf(dy - py, 2));
    clients[idx].trip_dist = trip_dist;

    int driver_idx = -1;
    float min_dist = -1.0f;

    printf("\n--- Dispatching Logic for Passenger: %s ---\n", clients[idx].name);
    printf("Pickup: %s (%.1f, %.1f) | Dropoff: %s (%.1f, %.1f) | Trip Distance: %.2f\n", 
           msg->pickup, px, py, msg->dropoff, dx, dy, trip_dist);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].role == ROLE_DRIVER && clients[i].status == STATUS_IDLE) {
            float dist_to_pickup = sqrtf(powf(clients[i].x - px, 2) + powf(clients[i].y - py, 2));
            printf("Checking Driver: %s | Pos: (%.1f, %.1f) | Dist to Pickup: %.2f\n", 
                   clients[i].name, clients[i].x, clients[i].y, dist_to_pickup);
            if (driver_idx == -1 || dist_to_pickup < min_dist) {
                min_dist = dist_to_pickup;
                driver_idx = i;
            }
        }
    }

    if (driver_idx == -1) {
        send_error_msg(clients[idx].fd, "No idle driver available.");
        return;
    }

    printf("Result: Selected Best Driver -> %s (Wait Dist: %.2f)\n", clients[driver_idx].name, min_dist);

    int order_id = next_order_id++;
    clients[idx].status = STATUS_WAITING;
    clients[idx].current_order_id = order_id;
    clients[idx].peer_fd = clients[driver_idx].fd;

    clients[driver_idx].status = STATUS_ASSIGNED;
    clients[driver_idx].current_order_id = order_id;
    clients[driver_idx].peer_fd = clients[idx].fd;

    ride_msg_t dispatch_msg;
    memset(&dispatch_msg, 0, sizeof(dispatch_msg));
    dispatch_msg.type = MSG_DISPATCH_JOB;
    dispatch_msg.order_id = order_id;
    strncpy(dispatch_msg.name, clients[idx].name, NAME_LEN - 1);
    strncpy(dispatch_msg.pickup, msg->pickup, LOC_LEN - 1);
    strncpy(dispatch_msg.dropoff, msg->dropoff, LOC_LEN - 1);
    send_msg(clients[driver_idx].fd, &dispatch_msg);
}

static void handle_accept(int idx) {
    if (clients[idx].role != ROLE_DRIVER) return;
    int passenger_idx = find_client_by_fd(clients[idx].peer_fd);
    if (passenger_idx == -1) {
        set_client_idle(idx);
        return;
    }
    clients[idx].status = STATUS_ON_TRIP;
    clients[passenger_idx].status = STATUS_ON_TRIP;
    ride_msg_t matched_msg;
    memset(&matched_msg, 0, sizeof(matched_msg));
    matched_msg.type = MSG_MATCHED;
    matched_msg.order_id = clients[idx].current_order_id;
    strncpy(matched_msg.name, clients[idx].name, NAME_LEN - 1);
    send_msg(clients[passenger_idx].fd, &matched_msg);
}

static void handle_reject(int idx) {
    if (clients[idx].role != ROLE_DRIVER) return;
    int passenger_idx = find_client_by_fd(clients[idx].peer_fd);
    if (passenger_idx != -1) {
        send_error_msg(clients[passenger_idx].fd, "Driver rejected the order.");
        set_client_idle(passenger_idx);
    }
    set_client_idle(idx);
}

static void handle_update_pos(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_DRIVER) return;
    clients[idx].x = msg->x;
    clients[idx].y = msg->y;
    int passenger_idx = find_client_by_fd(clients[idx].peer_fd);
    if (passenger_idx != -1) {
        ride_msg_t forward_msg = *msg;
        strncpy(forward_msg.name, clients[idx].name, NAME_LEN - 1);
        send_msg(clients[passenger_idx].fd, &forward_msg);
    }
}

static void handle_arrived(int idx) {
    if (clients[idx].role != ROLE_DRIVER) return;
    int passenger_idx = find_client_by_fd(clients[idx].peer_fd);
    if (passenger_idx != -1) {
        float total_fare = clients[passenger_idx].trip_dist * FARE_PER_UNIT;
        ride_msg_t bill_msg;
        memset(&bill_msg, 0, sizeof(bill_msg));
        bill_msg.type = MSG_BILL;
        bill_msg.order_id = clients[idx].current_order_id;
        snprintf(bill_msg.payload, PAYLOAD_LEN - 1, "Trip Done. Travel Dist: %.2f, Total Fare: $%.2f", 
                 clients[passenger_idx].trip_dist, total_fare);
        send_msg(clients[passenger_idx].fd, &bill_msg);
        set_client_idle(passenger_idx);
    }
    set_client_idle(idx);
}

static void handle_message(int idx, const ride_msg_t *msg) {
    switch (msg->type) {
        case MSG_LOGIN: handle_login(idx, msg); break;
        case MSG_RIDE_REQUEST: handle_ride_request(idx, msg); break;
        case MSG_ACCEPT: handle_accept(idx); break;
        case MSG_REJECT: handle_reject(idx); break;
        case MSG_UPDATE_POS: handle_update_pos(idx, msg); break;
        case MSG_ARRIVED: handle_arrived(idx); break;
        case MSG_LOGOUT: remove_client(idx); break;
        default: send_error_msg(clients[idx].fd, "Unknown message type."); break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    int listen_fd = setup_server_socket(port);
    if (listen_fd < 0) return 1;
    init_clients();
    printf("Dispatch server listening on port %d\n", port);
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &readfds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        }
        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) { perror("select"); continue; }
        if (FD_ISSET(listen_fd, &readfds)) {
            int new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                if (add_client(new_fd) == -1) {
                    send_error_msg(new_fd, "Server full.");
                    close(new_fd);
                }
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 && FD_ISSET(clients[i].fd, &readfds)) {
                ride_msg_t msg;
                int rc = recv_msg(clients[i].fd, &msg);
                if (rc <= 0) remove_client(i);
                else handle_message(i, &msg);
            }
        }
    }
    close(listen_fd);
    return 0;
}