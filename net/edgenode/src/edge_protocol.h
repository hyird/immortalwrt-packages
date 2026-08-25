#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

#define EDGENODE_PROTOCOL_VERSION 3U
#define EDGENODE_MAX_WS_MESSAGE 16384U

bool edge_protocol_validate_imei(const char *imei);

void edge_protocol_uuid_v7(uint64_t now_ms, const uint8_t random_bytes[10],
                           uint8_t output[16]);

bool edge_protocol_set_bytes(void *field, size_t field_capacity, const uint8_t *data,
                             size_t data_size);

bool edge_protocol_init_envelope(iot_edge_v1_Envelope *envelope,
                                 const uint8_t platform_id[16], const uint8_t *node_id,
                                 uint64_t session_epoch, uint64_t sequence, int64_t now_ms,
                                 const uint8_t random_bytes[10]);

bool edge_protocol_encode(const iot_edge_v1_Envelope *envelope, uint8_t *output,
                          size_t output_capacity, size_t *output_size, const char **error);

bool edge_protocol_decode(const uint8_t *input, size_t input_size,
                          iot_edge_v1_Envelope *envelope, const char **error);
