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
           clients[idx].fd, clients[idx].name,
           clients[idx].role == ROLE_DRIVER ? "driver" : "passenger",
           clients[idx].x, clients[idx].y);
    send_login_ack(clients[idx].fd);
}

static void handle_ride_request(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_PASSENGER || clients[idx].status != STATUS_IDLE) return;

    float px = -1.0f, py = -1.0f, dx = -1.0f, dy = -1.0f;
    for (size_t i = 0; i < MAP_SIZE; i++) {
        if (strcmp(city_map[i].name, msg->pickup) == 0) { px = city_map[i].x; py = city_map[i].y; }
        if (strcmp(city_map[i].name, msg->dropoff) == 0) { dx = city_map[i].x; dy = city_map[i].y; }
    }
    if (px < 0 || dx < 0) { send_error_msg(clients[idx].fd, "Unknown location."); return; }

    clients[idx].trip_dist = sqrtf(powf(dx - px, 2) + powf(dy - py, 2));

    int driver_idx = -1;
    float min_dist = -1.0f;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].role == ROLE_DRIVER && clients[i].status == STATUS_IDLE) {
            float d = sqrtf(powf(clients[i].x - px, 2) + powf(clients[i].y - py, 2));
            if (driver_idx == -1 || d < min_dist) { min_dist = d; driver_idx = i; }
        }
    }
    if (driver_idx == -1) { send_error_msg(clients[idx].fd, "No idle driver."); return; }

    int order_id = next_order_id++;
    clients[idx].status = STATUS_WAITING;
    clients[idx].current_order_id = order_id;
    clients[idx].peer_fd = clients[driver_idx].fd;
    clients[driver_idx].status = STATUS_ASSIGNED;
    clients[driver_idx].current_order_id = order_id;
    clients[driver_idx].peer_fd = clients[idx].fd;

    ride_msg_t d_msg;
    memset(&d_msg, 0, sizeof(d_msg));
    d_msg.type = MSG_DISPATCH_JOB;
    d_msg.order_id = order_id;
    strncpy(d_msg.name, clients[idx].name, NAME_LEN - 1);
    strncpy(d_msg.pickup, msg->pickup, LOC_LEN - 1);
    strncpy(d_msg.dropoff, msg->dropoff, LOC_LEN - 1);
    send_msg(clients[driver_idx].fd, &d_msg);
}

static void handle_cancel_ride(int idx) {
    if (clients[idx].role != ROLE_PASSENGER) return;

    int d_fd = clients[idx].peer_fd;
    if (d_fd != -1) {
        int d_idx = find_client_by_fd(d_fd);
        if (d_idx != -1) {
            send_error_msg(clients[d_idx].fd, "Passenger cancelled the request.");
            set_client_idle(d_idx); 
            
            printf("Driver %s (fd %d) is now IDLE due to passenger cancellation.\n", 
                   clients[d_idx].name, clients[d_idx].fd);
        }
    }
    set_client_idle(idx);
    printf("Passenger %s cancelled the request and is now IDLE.\n", clients[idx].name);
}

static void handle_accept(int idx) {
    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx == -1) { set_client_idle(idx); return; }
    clients[idx].status = STATUS_GOING_TO_PICKUP;
    clients[p_idx].status = STATUS_GOING_TO_PICKUP;
    ride_msg_t m_msg;
    memset(&m_msg, 0, sizeof(m_msg));
    m_msg.type = MSG_MATCHED;
    m_msg.order_id = clients[idx].current_order_id;
    strncpy(m_msg.name, clients[idx].name, NAME_LEN - 1);
    send_msg(clients[p_idx].fd, &m_msg);
}

static void handle_reject(int idx) {
    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        send_error_msg(clients[p_idx].fd, "Driver rejected the order.");
        set_client_idle(p_idx);
    }
    set_client_idle(idx);
}

static void handle_update_pos(int idx, const ride_msg_t *msg) {
    clients[idx].x = msg->x; clients[idx].y = msg->y;
    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        ride_msg_t f_msg = *msg;
        strncpy(f_msg.name, clients[idx].name, NAME_LEN - 1);
        send_msg(clients[p_idx].fd, &f_msg);
    }
}

static void handle_driver_arrived(int idx, const ride_msg_t *msg) {
    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        ride_msg_t notice = *msg;
        strncpy(notice.payload, "Your driver has arrived at the pickup location!", PAYLOAD_LEN - 1);
        send_msg(clients[p_idx].fd, &notice);
    }
}

static void handle_pickup_confirm(int idx, const ride_msg_t *msg) {
    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        clients[idx].status = STATUS_IN_PROGRESS;
        clients[p_idx].status = STATUS_IN_PROGRESS;
        ride_msg_t notice = *msg;
        strncpy(notice.payload, "Passenger picked up. Trip in progress!", PAYLOAD_LEN - 1);
        send_msg(clients[p_idx].fd, &notice);
    }
}

static void handle_arrived(int idx) {
    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        float fare = clients[p_idx].trip_dist * FARE_PER_UNIT;
        clients[p_idx].status = STATUS_SETTLING;
        ride_msg_t b_msg;
        memset(&b_msg, 0, sizeof(b_msg));
        b_msg.type = MSG_BILL;
        b_msg.fare = fare;
        snprintf(b_msg.payload, PAYLOAD_LEN - 1, "Trip Done. Dist: %.2f, Fare: $%.2f", 
                 clients[p_idx].trip_dist, fare);
        send_msg(clients[p_idx].fd, &b_msg);
    }
    set_client_idle(idx);
}

static void handle_tip_selection(int idx, const ride_msg_t *msg) {
    int d_fd = clients[idx].peer_fd;
    float fare = msg->fare;
    float tip = msg->tip;
    ride_msg_t s_msg;
    memset(&s_msg, 0, sizeof(s_msg));
    s_msg.type = MSG_FINAL_SETTLEMENT;
    s_msg.fare = fare;
    s_msg.tip = tip;
    int d_idx = find_client_by_fd(d_fd);
    if (d_idx != -1) send_msg(clients[d_idx].fd, &s_msg);
    send_msg(clients[idx].fd, &s_msg);
    set_client_idle(idx);
}

static void handle_message(int idx, const ride_msg_t *msg) {
    switch (msg->type) {
        case MSG_LOGIN: handle_login(idx, msg); break;
        case MSG_RIDE_REQUEST: handle_ride_request(idx, msg); break;
        case MSG_CANCEL_RIDE: handle_cancel_ride(idx); break;
        case MSG_ACCEPT: handle_accept(idx); break;
        case MSG_REJECT: handle_reject(idx); break;
        case MSG_UPDATE_POS: handle_update_pos(idx, msg); break;
        case MSG_DRIVER_ARRIVED: handle_driver_arrived(idx, msg); break;
        case MSG_PICKUP_CONFIRM: handle_pickup_confirm(idx, msg); break;
        case MSG_ARRIVED: handle_arrived(idx); break;
        case MSG_TIP_SELECTION: handle_tip_selection(idx, msg); break;
        case MSG_LOGOUT: remove_client(idx); break;
        default: break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) return 1;
    uint16_t port = (uint16_t)atoi(argv[1]);
    int l_fd = setup_server_socket(port);
    if (l_fd < 0) return 1;
    init_clients();
    printf("Server listening on port %d\n", port);
    while (1) {
        fd_set r_fds; FD_ZERO(&r_fds); FD_SET(l_fd, &r_fds);
        int m_fd = l_fd;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) { FD_SET(clients[i].fd, &r_fds); if (clients[i].fd > m_fd) m_fd = clients[i].fd; }
        }
        if (select(m_fd + 1, &r_fds, NULL, NULL, NULL) < 0) continue;
        if (FD_ISSET(l_fd, &r_fds)) { int n_fd = accept(l_fd, NULL, NULL); if (n_fd >= 0 && add_client(n_fd) == -1) close(n_fd); }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 && FD_ISSET(clients[i].fd, &r_fds)) {
                ride_msg_t m;
                if (recv_msg(clients[i].fd, &m) <= 0) remove_client(i);
                else handle_message(i, &m);
            }
        }
    }
    return 0;
}