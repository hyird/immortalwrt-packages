#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"
#include "edge_runtime_config.h"

typedef struct edge_acquisition edge_acquisition;

typedef bool (*edge_acquisition_telemetry_callback)(
    void *context, const iot_edge_v1_TelemetryRecord *record);
typedef bool (*edge_acquisition_command_callback)(
    void *context, const iot_edge_v1_CommandResult *result);

edge_acquisition *edge_acquisition_create(
    edge_acquisition_telemetry_callback telemetry,
    edge_acquisition_command_callback command, void *callback_context);

/*
 * Builds a replacement runtime without opening device connections.
 * Only Modbus RTU/TCP and S7 TCP Client are accepted.
 */
bool edge_acquisition_apply(edge_acquisition *acquisition,
                            const edge_runtime_config *config,
                            uint64_t now_ms, char *error, size_t error_size);

/* Starts the supervised acquisition worker without blocking the WebSocket loop. */
bool edge_acquisition_start(edge_acquisition *acquisition,
                            char *error, size_t error_size);
void edge_acquisition_stop(edge_acquisition *acquisition);
int edge_acquisition_event_fd(const edge_acquisition *acquisition);

/* Drains worker events and restarts a failed worker; it performs no device I/O. */
void edge_acquisition_tick(edge_acquisition *acquisition, uint64_t now_ms);

void edge_acquisition_status(edge_acquisition *acquisition,
                             iot_edge_v1_DeviceStatusReport *report);

bool edge_acquisition_command(edge_acquisition *acquisition,
                              const iot_edge_v1_CommandRequest *request,
                              char *error, size_t error_size);

void edge_acquisition_destroy(edge_acquisition *acquisition);
