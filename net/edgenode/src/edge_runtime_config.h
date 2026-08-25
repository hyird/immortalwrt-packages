#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"
#include "edge_memory.h"

typedef struct {
    uint64_t revision;
    iot_edge_v1_ConfigItem *items;
    uint32_t item_count;
    uint32_t endpoint_count;
    uint32_t device_count;
} edge_runtime_config;

void edge_runtime_config_free(edge_runtime_config *config);

/*
 * Decodes and validates one complete staging/active nanopb snapshot. The caller
 * swaps this result into the live runtime only after the spool commit succeeds.
 */
bool edge_runtime_config_load(edge_runtime_config *output,
                              const edge_memory_config_set *snapshot,
                              char *error, size_t error_size);

const iot_edge_v1_EndpointConfig *
edge_runtime_config_endpoint(const edge_runtime_config *config, const uint8_t id[16]);

const iot_edge_v1_DeviceConfig *
edge_runtime_config_device(const edge_runtime_config *config, const uint8_t id[16]);
