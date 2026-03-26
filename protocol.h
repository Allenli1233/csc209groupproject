#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define NAME_LEN 32
#define LOC_LEN 64
#define PAYLOAD_LEN 128

typedef enum {
    ROLE_NONE = 0,
    ROLE_PASSENGER = 1,
    ROLE_DRIVER = 2
} role_t;

typedef enum {
    STATUS_IDLE = 0,
    STATUS_WAITING = 1,
    STATUS_ASSIGNED = 2,
    STATUS_ON_TRIP = 3
} status_t;

typedef enum {
    MSG_LOGIN = 1,
    MSG_LOGIN_ACK,
    MSG_RIDE_REQUEST,
    MSG_DISPATCH_JOB,
    MSG_ACCEPT,
    MSG_REJECT,
    MSG_UPDATE_POS,
    MSG_MATCHED,
    MSG_ARRIVED,
    MSG_BILL,
    MSG_ERROR,
    MSG_LOGOUT
} msg_type_t;

typedef struct {
    int32_t type;       // msg_type_t
    int32_t user_id;
    int32_t role;       // role_t
    int32_t order_id;
    int32_t status;     // status_t
    float x;
    float y;
    char name[NAME_LEN];
    char pickup[LOC_LEN];
    char dropoff[LOC_LEN];
    char payload[PAYLOAD_LEN];
} ride_msg_t;

#endif