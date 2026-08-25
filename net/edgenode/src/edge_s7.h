#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_S7_MAX_FRAME 1024U
#define EDGE_S7_DEFAULT_PDU_LENGTH 480U

typedef enum {
    EDGE_S7_OK = 0,
    EDGE_S7_INVALID_ARGUMENT,
    EDGE_S7_TRUNCATED,
    EDGE_S7_WRONG_RESPONSE,
    EDGE_S7_PROTOCOL_ERROR,
    EDGE_S7_ACCESS_DENIED,
    EDGE_S7_OUTPUT_TOO_SMALL,
} edge_s7_result;

typedef enum {
    EDGE_S7_AREA_INPUTS = 0x81,
    EDGE_S7_AREA_OUTPUTS = 0x82,
    EDGE_S7_AREA_FLAGS = 0x83,
    EDGE_S7_AREA_DB = 0x84,
    EDGE_S7_AREA_COUNTER = 0x1c,
    EDGE_S7_AREA_TIMER = 0x1d,
} edge_s7_area;

typedef struct {
    edge_s7_area area;
    uint16_t db_number;
    uint32_t start_byte;
    uint8_t start_bit;
    uint16_t size;
    bool bit_access;
} edge_s7_address;

size_t edge_s7_build_cotp_connect(uint16_t local_tsap, uint16_t remote_tsap,
                                  uint8_t *output, size_t capacity);
edge_s7_result edge_s7_parse_cotp_confirm(const uint8_t *frame, size_t frame_size);

size_t edge_s7_build_setup(uint16_t reference, uint16_t requested_pdu_length,
                           uint8_t *output, size_t capacity);
edge_s7_result edge_s7_parse_setup(const uint8_t *frame, size_t frame_size,
                                   uint16_t reference, uint16_t *negotiated_pdu_length);

size_t edge_s7_build_read(uint16_t reference, const edge_s7_address *address,
                          uint8_t *output, size_t capacity);
size_t edge_s7_build_write(uint16_t reference, const edge_s7_address *address,
                           const uint8_t *data, size_t data_size,
                           uint8_t *output, size_t capacity);

edge_s7_result edge_s7_parse_read(const uint8_t *frame, size_t frame_size,
                                  uint16_t reference, uint8_t *data,
                                  size_t capacity, size_t *data_size,
                                  uint8_t *return_code);
edge_s7_result edge_s7_parse_write(const uint8_t *frame, size_t frame_size,
                                   uint16_t reference, uint8_t *return_code);

/* True only for a complete TPKT. Callers retain partial TCP data until complete. */
bool edge_s7_frame_length(const uint8_t *data, size_t size, size_t *frame_size);
