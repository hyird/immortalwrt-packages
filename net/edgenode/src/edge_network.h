#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

bool edge_network_validate_static(const char *ip, uint32_t prefix_length,
                                  const char *gateway, char *error, size_t error_size);

/* Validates, backs up, stages, and commits a bounded UCI interface transaction. */
bool edge_network_prepare(const iot_edge_v1_NetworkConfigRequest *request,
                          const char *protected_device_name, char *error,
                          size_t error_size);

/* Reloads network and starts a rollback watchdog. */
bool edge_network_activate(uint32_t rollback_timeout_sec,
                           const uint8_t request_id[16]);

/* Called only after the network-owner platform reconnects successfully. */
void edge_network_confirm(void);

/* Returns a request ID once after the watchdog restored the previous UCI config. */
bool edge_network_take_rollback(uint8_t request_id[16]);
