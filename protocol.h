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
    STATUS_GOING_TO_PICKUP = 3,
    STATUS_IN_PROGRESS = 4,
    STATUS_SETTLING = 5
} status_t;

typedef enum {
    MSG_LOGIN = 1,
    MSG_LOGIN_ACK,
    MSG_RIDE_REQUEST,
    MSG_CANCEL_RIDE,
    MSG_DISPATCH_JOB,
    MSG_ACCEPT,
    MSG_REJECT,
    MSG_UPDATE_POS,
    MSG_MATCHED,
    MSG_DRIVER_ARRIVED,
    MSG_PICKUP_CONFIRM,
    MSG_ARRIVED,
    MSG_BILL,
    MSG_TIP_SELECTION,
    MSG_FINAL_SETTLEMENT,
    MSG_ERROR,
    MSG_LOGOUT
} msg_type_t;

typedef struct {
    int32_t type;
    int32_t user_id;
    int32_t role;
    int32_t order_id;
    int32_t status;
    float x;
    float y;
    float fare;
    float tip;
    char name[NAME_LEN];
    char pickup[LOC_LEN];
    char dropoff[LOC_LEN];
    char payload[PAYLOAD_LEN];
} ride_msg_t;

#endif