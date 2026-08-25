#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_MAX_PLATFORMS 4U
#define EDGE_URL_MAX 255U
#define EDGE_BOOTSTRAP_PLATFORM_ID "00000000-0000-7000-8000-000000000001"

typedef struct {
    uint8_t id[16];
    char name[49];
    /* User-visible platform base address. Production connections require HTTPS. */
    char url[EDGE_URL_MAX + 1U];
    bool enabled;
    bool network_owner;
    uint16_t priority;
    uint16_t reconnect_interval_sec;
    uint32_t outbox_max_bytes;
    bool bootstrap;
} edge_platform_config;

typedef struct {
    char imei[16];
    char model[129];
    char serial_port[97];
    char lan_interface[33];
    char wan_interface[33];
    char modem_at_port[97];
    char modem_status_path[129];
    bool bridge;
    bool serial_rs485;
    uint16_t heartbeat_interval_sec;
    uint8_t platform_count;
    edge_platform_config platforms[EDGE_MAX_PLATFORMS];
} edge_app_config;

bool edge_config_parse_uuid(const char *text, uint8_t output[16]);
void edge_config_format_uuid(const uint8_t value[16], char output[37]);
bool edge_config_valid_platform_url(const char *value);

/* Loads /etc/config/edgenode through libuci. error never contains credentials. */
bool edge_config_load(edge_app_config *config, char *error, size_t error_size);
