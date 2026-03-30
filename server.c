#include "protocol.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    {"Pearson Airport", 10.0, 50.0},
    {"Sheraton Hotel", 45.0, 40.0},
    {"Scotiabank Arena", 46.0, 38.0},
    {"UofT St George campus", 44.0, 45.0},
    {"Eaton Centre", 47.0, 42.0},
    {"Yorkdale", 30.0, 70.0},
    {"CN Tower", 43.0, 37.0},
    {"Scarborough Town Centre", 85.0, 80.0},
    {"High Park", 25.0, 35.0},
    {"NorthYork Centre", 55.0, 85.0}
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
    int tried_drivers[MAX_CLIENTS];
    int tried_count;
    char saved_pickup[LOC_LEN];
    char saved_dropoff[LOC_LEN];
} client_t;

static client_t clients[MAX_CLIENTS];
static int next_order_id = 1;

static int wait_queue[MAX_CLIENTS];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

static int dequeue_passenger(void) {
    if (queue_count == 0) return -1;
    int idx = wait_queue[queue_head];
    queue_head = (queue_head + 1) % MAX_CLIENTS;
    queue_count--;
    return idx;
}

static void remove_from_queue(int idx) {
    int new_queue[MAX_CLIENTS];
    int new_count = 0;
    int h = queue_head;
    for (int i = 0; i < queue_count; i++) {
        int q_idx = wait_queue[h];
        h = (h + 1) % MAX_CLIENTS;
        if (q_idx != idx) {
            new_queue[new_count++] = q_idx;
        }
    }
    memcpy(wait_queue, new_queue, sizeof(int) * new_count);
    queue_head = 0;
    queue_tail = new_count;
    queue_count = new_count;
}

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
    c->tried_count = 0;
    memset(c->tried_drivers, 0, sizeof(c->tried_drivers));
    c->saved_pickup[0] = '\0';
    c->saved_dropoff[0] = '\0';
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
        if (clients[i].fd == fd) return i;
    }
    return -1;
}

static void set_client_idle(int idx) {
    clients[idx].status = STATUS_IDLE;
    clients[idx].current_order_id = 0;
    clients[idx].peer_fd = -1;
    clients[idx].trip_dist = 0.0f;
    clients[idx].tried_count = 0;
    memset(clients[idx].tried_drivers, 0, sizeof(clients[idx].tried_drivers));
}

static int send_error_msg(int fd, const char *text) {
    ride_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_ERROR;
    strncpy(msg.payload, text, PAYLOAD_LEN - 1);

    if (send_msg(fd, &msg) < 0) {
        perror("send MSG_ERROR");
        return -1;
    }
    return 0;
}

static int send_login_ack(int fd) {
    ride_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LOGIN_ACK;

    if (send_msg(fd, &msg) < 0) {
        perror("send MSG_LOGIN_ACK");
        return -1;
    }
    return 0;
}

static void remove_client(int idx) {
    if (idx < 0 || idx >= MAX_CLIENTS || clients[idx].fd == -1) return;
    if (clients[idx].status == STATUS_QUEUED) {
        remove_from_queue(idx);
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

static void enqueue_passenger(int idx) {
    if (queue_count >= MAX_CLIENTS) {
        send_error_msg(clients[idx].fd, "Queue is full. Please try again later.");
        set_client_idle(idx);
        return;
    }
    wait_queue[queue_tail] = idx;
    queue_tail = (queue_tail + 1) % MAX_CLIENTS;
    queue_count++;
    clients[idx].status = STATUS_QUEUED;

    ride_msg_t qmsg;
    memset(&qmsg, 0, sizeof(qmsg));
    qmsg.type = MSG_QUEUED;
    snprintf(qmsg.payload, PAYLOAD_LEN - 1,
             "No driver available. You are #%d in the waiting queue.", queue_count);
    if (send_msg(clients[idx].fd, &qmsg) < 0) {
        perror("send MSG_QUEUED");
        remove_client(idx);
    }
}

static void dispatch_to_next_driver(int p_idx) {
    if (p_idx < 0 || p_idx >= MAX_CLIENTS) return;
    if (clients[p_idx].fd == -1) return;
    if (clients[p_idx].role != ROLE_PASSENGER) return;

    float px = -1.0f, py = -1.0f;

    for (size_t i = 0; i < MAP_SIZE; i++) {
        if (strcasecmp(city_map[i].name, clients[p_idx].saved_pickup) == 0) {
            px = city_map[i].x;
            py = city_map[i].y;
            break;
        }
    }

    if (px < 0 || py < 0) {
        send_error_msg(clients[p_idx].fd, "Cannot re-dispatch: pickup location lost.");
        set_client_idle(p_idx);
        return;
    }

    int driver_idx = -1;
    float min_dist = -1.0f;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 &&
            clients[i].role == ROLE_DRIVER &&
            clients[i].status == STATUS_IDLE) {

            int already_tried = 0;
            for (int j = 0; j < clients[p_idx].tried_count; j++) {
                if (clients[p_idx].tried_drivers[j] == clients[i].fd) {
                    already_tried = 1;
                    break;
                }
            }

            if (already_tried) {
                continue;
            }

            float d = sqrtf(powf(clients[i].x - px, 2) + powf(clients[i].y - py, 2));
            if (driver_idx == -1 || d < min_dist) {
                min_dist = d;
                driver_idx = i;
            }
        }
    }

    if (driver_idx == -1) {
        set_client_idle(p_idx);
        enqueue_passenger(p_idx);
        return;
    }

    clients[p_idx].status = STATUS_WAITING;
    clients[p_idx].peer_fd = clients[driver_idx].fd;

    clients[driver_idx].status = STATUS_ASSIGNED;
    clients[driver_idx].current_order_id = clients[p_idx].current_order_id;
    clients[driver_idx].peer_fd = clients[p_idx].fd;

    ride_msg_t d_msg;
    memset(&d_msg, 0, sizeof(d_msg));
    d_msg.type = MSG_DISPATCH_JOB;
    d_msg.order_id = clients[p_idx].current_order_id;
    strncpy(d_msg.name, clients[p_idx].name, NAME_LEN - 1);
    strncpy(d_msg.pickup, clients[p_idx].saved_pickup, LOC_LEN - 1);
    strncpy(d_msg.dropoff, clients[p_idx].saved_dropoff, LOC_LEN - 1);

    if (send_msg(clients[driver_idx].fd, &d_msg) < 0) {
        perror("send MSG_DISPATCH_JOB (re-dispatch)");
        remove_client(driver_idx);
        dispatch_to_next_driver(p_idx);
        return;
    }

    printf("Re-dispatching order %d to driver %s\n",
           d_msg.order_id, clients[driver_idx].name);
}

static void try_dispatch_from_queue(void) {
    while (queue_count > 0) {
        int p_idx = wait_queue[queue_head];

        if (p_idx < 0 || p_idx >= MAX_CLIENTS ||
            clients[p_idx].fd == -1 ||
            clients[p_idx].status != STATUS_QUEUED) {
            dequeue_passenger();
            continue;
        }

        float px = -1.0f, py = -1.0f;
        for (size_t i = 0; i < MAP_SIZE; i++) {
            if (strcasecmp(city_map[i].name, clients[p_idx].saved_pickup) == 0) {
                px = city_map[i].x;
                py = city_map[i].y;
                break;
            }
        }

        if (px < 0 || py < 0) {
            dequeue_passenger();
            send_error_msg(clients[p_idx].fd, "Queue dispatch failed: pickup location lost.");
            clients[p_idx].status = STATUS_IDLE;
            continue;
        }

        int driver_idx = -1;
        float min_dist = -1.0f;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 &&
                clients[i].role == ROLE_DRIVER &&
                clients[i].status == STATUS_IDLE) {
                float d = sqrtf(powf(clients[i].x - px, 2) + powf(clients[i].y - py, 2));
                if (driver_idx == -1 || d < min_dist) {
                    min_dist = d;
                    driver_idx = i;
                }
            }
        }

        if (driver_idx == -1) {
            break;
        }

        dequeue_passenger();

        int order_id = next_order_id++;
        clients[p_idx].status = STATUS_WAITING;
        clients[p_idx].current_order_id = order_id;
        clients[p_idx].peer_fd = clients[driver_idx].fd;
        clients[p_idx].tried_count = 0;
        memset(clients[p_idx].tried_drivers, 0, sizeof(clients[p_idx].tried_drivers));

        clients[driver_idx].status = STATUS_ASSIGNED;
        clients[driver_idx].current_order_id = order_id;
        clients[driver_idx].peer_fd = clients[p_idx].fd;

        ride_msg_t d_msg;
        memset(&d_msg, 0, sizeof(d_msg));
        d_msg.type = MSG_DISPATCH_JOB;
        d_msg.order_id = order_id;
        strncpy(d_msg.name, clients[p_idx].name, NAME_LEN - 1);
        strncpy(d_msg.pickup, clients[p_idx].saved_pickup, LOC_LEN - 1);
        strncpy(d_msg.dropoff, clients[p_idx].saved_dropoff, LOC_LEN - 1);

        if (send_msg(clients[driver_idx].fd, &d_msg) < 0) {
            perror("send MSG_DISPATCH_JOB (from queue)");
            remove_client(driver_idx);
            dispatch_to_next_driver(p_idx);
        } else {
            printf("Dispatched queued order %d to driver %s\n",
                   order_id, clients[driver_idx].name);
        }
    }
}



static void handle_login(int idx, const ride_msg_t *msg) {
    clients[idx].role = (role_t)msg->role;
    clients[idx].status = STATUS_IDLE;
    strncpy(clients[idx].name, msg->name, NAME_LEN - 1);
    if (clients[idx].role == ROLE_DRIVER) {
        clients[idx].x = msg->x;
        clients[idx].y = msg->y;
    }
    printf("Client fd=%d logged in as %s\n", clients[idx].fd, clients[idx].name);
    if (send_login_ack(clients[idx].fd) < 0) {
        remove_client(idx);
        return;
    }
    if (clients[idx].role == ROLE_DRIVER) {
        try_dispatch_from_queue();
    }
}

static void handle_ride_request(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_PASSENGER || clients[idx].status != STATUS_IDLE) {
        return;
    }

    float px = -1.0f, py = -1.0f;
    float dx = -1.0f, dy = -1.0f;

    for (size_t i = 0; i < MAP_SIZE; i++) {
        if (strcasecmp(city_map[i].name, msg->pickup) == 0) {
            px = city_map[i].x;
            py = city_map[i].y;
        }
        if (strcasecmp(city_map[i].name, msg->dropoff) == 0) {
            dx = city_map[i].x;
            dy = city_map[i].y;
        }
    }

    if (px < 0 || dx < 0) {
        send_error_msg(clients[idx].fd, "Unknown location.");
        return;
    }

    strncpy(clients[idx].saved_pickup, msg->pickup, LOC_LEN - 1);
    clients[idx].saved_pickup[LOC_LEN - 1] = '\0';

    strncpy(clients[idx].saved_dropoff, msg->dropoff, LOC_LEN - 1);
    clients[idx].saved_dropoff[LOC_LEN - 1] = '\0';

    clients[idx].trip_dist = sqrtf(powf(dx - px, 2) + powf(dy - py, 2));

    clients[idx].tried_count = 0;
    memset(clients[idx].tried_drivers, 0, sizeof(clients[idx].tried_drivers));

    int driver_idx = -1;
    float min_dist = -1.0f;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 &&
            clients[i].role == ROLE_DRIVER &&
            clients[i].status == STATUS_IDLE) {

            float d = sqrtf(powf(clients[i].x - px, 2) + powf(clients[i].y - py, 2));
            if (driver_idx == -1 || d < min_dist) {
                min_dist = d;
                driver_idx = i;
            }
        }
    }

    if (driver_idx == -1) {
        enqueue_passenger(idx);
        return;
    }

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
    strncpy(d_msg.pickup, clients[idx].saved_pickup, LOC_LEN - 1);
    strncpy(d_msg.dropoff, clients[idx].saved_dropoff, LOC_LEN - 1);

    if (send_msg(clients[driver_idx].fd, &d_msg) < 0) {
    int bad_fd = clients[driver_idx].fd;
    perror("send MSG_DISPATCH_JOB");

    if (clients[idx].tried_count < MAX_CLIENTS) {
        clients[idx].tried_drivers[clients[idx].tried_count] = bad_fd;
        clients[idx].tried_count++;
    }

    clients[idx].peer_fd = -1;

    close(clients[driver_idx].fd);
    reset_client(&clients[driver_idx]);

    dispatch_to_next_driver(idx);
    return;
}
}

static void handle_cancel_ride(int idx) {
    if (clients[idx].role != ROLE_PASSENGER) {
        return;
    }

    if (clients[idx].status == STATUS_QUEUED) {
        remove_from_queue(idx);
        set_client_idle(idx);
        clients[idx].saved_pickup[0] = '\0';
        clients[idx].saved_dropoff[0] = '\0';
        return;
    }

    if (clients[idx].status != STATUS_WAITING) {
        send_error_msg(clients[idx].fd, "Cancel is only allowed before driver arrival.");
        return;
    }

    int d_fd = clients[idx].peer_fd;
    if (d_fd != -1) {
        int d_idx = find_client_by_fd(d_fd);
        if (d_idx != -1) {
            send_error_msg(clients[d_idx].fd, "Passenger cancelled the request.");
            set_client_idle(d_idx);
            try_dispatch_from_queue();
        }
    }

    set_client_idle(idx);
    clients[idx].saved_pickup[0] = '\0';
    clients[idx].saved_dropoff[0] = '\0';
}

static void handle_accept(int idx) {
    if (clients[idx].role != ROLE_DRIVER || clients[idx].status != STATUS_ASSIGNED) {
        send_error_msg(clients[idx].fd, "Invalid ACCEPT message.");
        return;
    }

    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx == -1) {
        set_client_idle(idx);
        return;
    }

    if (clients[p_idx].role != ROLE_PASSENGER || clients[p_idx].status != STATUS_WAITING) {
        send_error_msg(clients[idx].fd, "Passenger is not in waiting state.");
        set_client_idle(idx);
        return;
    }

    clients[idx].status = STATUS_GOING_TO_PICKUP;
    clients[p_idx].status = STATUS_GOING_TO_PICKUP;

    ride_msg_t m_msg;
    memset(&m_msg, 0, sizeof(m_msg));
    m_msg.type = MSG_MATCHED;
    m_msg.order_id = clients[idx].current_order_id;
    strncpy(m_msg.name, clients[idx].name, NAME_LEN - 1);

    if (send_msg(clients[p_idx].fd, &m_msg) < 0) {
        perror("send MSG_MATCHED");
        remove_client(p_idx);
        set_client_idle(idx);
        return;
    }
}

static void handle_reject(int idx) {
    if (clients[idx].role != ROLE_DRIVER || clients[idx].status != STATUS_ASSIGNED) {
        send_error_msg(clients[idx].fd, "Invalid REJECT message.");
        return;
    }

    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx == -1) {
        set_client_idle(idx);
        return;
    }

    if (clients[p_idx].role != ROLE_PASSENGER || clients[p_idx].status != STATUS_WAITING) {
        set_client_idle(idx);
        return;
    }

    if (clients[p_idx].tried_count < MAX_CLIENTS) {
        clients[p_idx].tried_drivers[clients[p_idx].tried_count] = clients[idx].fd;
        clients[p_idx].tried_count++;
    }

    set_client_idle(idx);
    dispatch_to_next_driver(p_idx);
    try_dispatch_from_queue();
}

static void handle_update_pos(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_DRIVER) {
        send_error_msg(clients[idx].fd, "Only drivers can update position.");
        return;
    }

    if (clients[idx].status != STATUS_GOING_TO_PICKUP &&
        clients[idx].status != STATUS_IN_PROGRESS) {
        send_error_msg(clients[idx].fd, "Position update not allowed in current state.");
        return;
    }

    clients[idx].x = msg->x;
    clients[idx].y = msg->y;

    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        ride_msg_t f_msg = *msg;
        strncpy(f_msg.name, clients[idx].name, NAME_LEN - 1);

        if (send_msg(clients[p_idx].fd, &f_msg) < 0) {
            perror("send MSG_UPDATE_POS");
            remove_client(p_idx);
            set_client_idle(idx);
            return;
        }
    }
}

static void handle_driver_arrived(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_DRIVER || clients[idx].status != STATUS_GOING_TO_PICKUP) {
        send_error_msg(clients[idx].fd, "Invalid DRIVER_ARRIVED message.");
        return;
    }

    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        ride_msg_t n = *msg;
        strncpy(n.payload, "Your driver has arrived at the pickup location!", PAYLOAD_LEN - 1);

        if (send_msg(clients[p_idx].fd, &n) < 0) {
            perror("send MSG_DRIVER_ARRIVED");
            remove_client(p_idx);
            set_client_idle(idx);
            return;
        }
    }
}

static void handle_pickup_confirm(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_DRIVER || clients[idx].status != STATUS_GOING_TO_PICKUP) {
        send_error_msg(clients[idx].fd, "Invalid PICKUP_CONFIRM message.");
        return;
    }

    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        if (clients[p_idx].role != ROLE_PASSENGER) {
            send_error_msg(clients[idx].fd, "Invalid passenger peer.");
            set_client_idle(idx);
            return;
        }

        clients[idx].status = STATUS_IN_PROGRESS;
        clients[p_idx].status = STATUS_IN_PROGRESS;

        ride_msg_t n = *msg;
        strncpy(n.payload, "Passenger picked up. Trip in progress!", PAYLOAD_LEN - 1);

        if (send_msg(clients[p_idx].fd, &n) < 0) {
            perror("send MSG_PICKUP_CONFIRM");
            remove_client(p_idx);
            set_client_idle(idx);
            return;
        }
    }
}


static void handle_arrived(int idx) {
    if (clients[idx].role != ROLE_DRIVER || clients[idx].status != STATUS_IN_PROGRESS) {
        send_error_msg(clients[idx].fd, "Invalid ARRIVED message.");
        return;
    }

    int p_idx = find_client_by_fd(clients[idx].peer_fd);
    if (p_idx != -1) {
        if (clients[p_idx].role != ROLE_PASSENGER) {
            send_error_msg(clients[idx].fd, "Invalid passenger peer.");
            set_client_idle(idx);
            return;
        }

        float fare = clients[p_idx].trip_dist * FARE_PER_UNIT;
        clients[p_idx].status = STATUS_SETTLING;

        ride_msg_t b;
        memset(&b, 0, sizeof(b));
        b.type = MSG_BILL;
        b.fare = fare;
        snprintf(b.payload, PAYLOAD_LEN - 1,
                 "Trip Done. Dist: %.2f, Fare: $%.2f",
                 clients[p_idx].trip_dist, fare);

        if (send_msg(clients[p_idx].fd, &b) < 0) {
            perror("send MSG_BILL");
            remove_client(p_idx);
        }
    }

    set_client_idle(idx);
    try_dispatch_from_queue();
}

static void handle_tip_selection(int idx, const ride_msg_t *msg) {
    if (clients[idx].role != ROLE_PASSENGER || clients[idx].status != STATUS_SETTLING) {
        send_error_msg(clients[idx].fd, "Invalid tip selection state.");
        return;
    }

    int d_fd = clients[idx].peer_fd;

    ride_msg_t s;
    memset(&s, 0, sizeof(s));
    s.type = MSG_FINAL_SETTLEMENT;
    s.fare = msg->fare;
    s.tip = msg->tip;

    int d_idx = find_client_by_fd(d_fd);
    if (d_idx != -1) {
        if (send_msg(clients[d_idx].fd, &s) < 0) {
            perror("send MSG_FINAL_SETTLEMENT to driver");
            remove_client(d_idx);
        }
    }

    if (send_msg(clients[idx].fd, &s) < 0) {
        perror("send MSG_FINAL_SETTLEMENT to passenger");
        remove_client(idx);
        return;
    }

    set_client_idle(idx);
    clients[idx].saved_pickup[0] = '\0';
    clients[idx].saved_dropoff[0] = '\0';
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
        default:
            send_error_msg(clients[idx].fd, "Unknown message type.");
            break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) return 1;
    int l_fd = setup_server_socket((uint16_t)atoi(argv[1]));
    if (l_fd < 0) return 1;
    init_clients();
    printf("Server listening on port %s\n", argv[1]);
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