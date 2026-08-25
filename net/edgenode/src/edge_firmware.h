#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

bool edge_firmware_start(const uint8_t platform_id[16],
                         const iot_edge_v1_FirmwareUpdateRequest *request,
                         char *error, size_t error_size);

/* Returns and removes the latest child-process status for this platform. */
bool edge_firmware_read_status(const uint8_t platform_id[16],
                               iot_edge_v1_FirmwareUpdateResult *result);
bool edge_firmware_active(void);
bool edge_firmware_has_status(const uint8_t platform_id[16]);
