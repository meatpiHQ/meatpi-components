#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* the bounds ml_routes.c works to (the production header takes
   ML_MAX_PEERS from Kconfig) */
#define ML_MAX_PEERS 16
#define ML_MAX_PEER_ROUTES 4
#define ML_MAX_ROUTES 8

typedef struct
{
    uint32_t network;
    uint32_t mask;
    uint32_t via_vpn_ip;
} microlink_route_t;

typedef struct
{
    uint32_t vpn_ip;
    uint8_t public_key[32];
    uint8_t disco_key[32];
    char hostname[64];
    bool active;
    struct
    {
        uint32_t network;
        uint32_t mask;
    } routes[ML_MAX_PEER_ROUTES];
    int route_count;
} ml_peer_t;

typedef struct
{
    uint8_t disco_public_key[32];
    uint8_t disco_private_key[32];
    ml_peer_t peers[ML_MAX_PEERS];
    int peer_count;
    microlink_route_t routes[ML_MAX_ROUTES];
    int route_count;
} microlink_t;

typedef struct
{
    bool via_derp;
    uint32_t src_ip;
    uint16_t src_port;
} ml_rx_packet_t;

esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *key,
                            const uint8_t *data, size_t len);
