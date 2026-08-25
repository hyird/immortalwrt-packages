#include "edge_s7.h"

#include <string.h>

#define S7_READ_VAR 0x04U
#define S7_WRITE_VAR 0x05U

static void put_be16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static uint16_t get_be16(const uint8_t *input) {
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

static bool valid_area(edge_s7_area area) {
    return area == EDGE_S7_AREA_INPUTS || area == EDGE_S7_AREA_OUTPUTS ||
           area == EDGE_S7_AREA_FLAGS || area == EDGE_S7_AREA_DB ||
           area == EDGE_S7_AREA_COUNTER || area == EDGE_S7_AREA_TIMER;
}

static bool valid_address(const edge_s7_address *address) {
    if (address == NULL || !valid_area(address->area) || address->size == 0U ||
        address->start_bit > 7U || address->start_byte > 0x1fffffU)
        return false;
    if (address->area != EDGE_S7_AREA_DB && address->db_number != 0U)
        return false;
    return true;
}

bool edge_s7_frame_length(const uint8_t *data, size_t size, size_t *frame_size) {
    if (frame_size != NULL)
        *frame_size = 0U;
    if (data == NULL || frame_size == NULL || size < 4U || data[0] != 0x03U || data[1] != 0U)
        return false;
    const size_t length = get_be16(data + 2U);
    if (length < 7U || length > EDGE_S7_MAX_FRAME || size < length)
        return false;
    *frame_size = length;
    return true;
}

size_t edge_s7_build_cotp_connect(uint16_t local_tsap, uint16_t remote_tsap,
                                  uint8_t *output, size_t capacity) {
    static const uint8_t base[22] = {0x03, 0x00, 0x00, 0x16, 0x11, 0xe0, 0x00, 0x00,
                                     0x00, 0x01, 0x00, 0xc0, 0x01, 0x0a, 0xc1, 0x02,
                                     0x00, 0x00, 0xc2, 0x02, 0x00, 0x00};
    if (output == NULL || capacity < sizeof(base))
        return 0U;
    memcpy(output, base, sizeof(base));
    put_be16(output + 16U, local_tsap);
    put_be16(output + 20U, remote_tsap);
    return sizeof(base);
}

edge_s7_result edge_s7_parse_cotp_confirm(const uint8_t *frame, size_t frame_size) {
    size_t complete = 0U;
    if (!edge_s7_frame_length(frame, frame_size, &complete))
        return frame_size < 4U ? EDGE_S7_TRUNCATED : EDGE_S7_WRONG_RESPONSE;
    if (complete != frame_size || frame_size < 11U || frame[5] != 0xd0U)
        return EDGE_S7_WRONG_RESPONSE;
    return EDGE_S7_OK;
}

size_t edge_s7_build_setup(uint16_t reference, uint16_t requested_pdu_length,
                           uint8_t *output, size_t capacity) {
    static const uint8_t base[25] = {0x03, 0x00, 0x00, 0x19, 0x02, 0xf0, 0x80, 0x32, 0x01,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0xf0,
                                     0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    if (output == NULL || capacity < sizeof(base) || requested_pdu_length < 240U)
        return 0U;
    memcpy(output, base, sizeof(base));
    put_be16(output + 11U, reference);
    put_be16(output + 23U, requested_pdu_length);
    return sizeof(base);
}

static edge_s7_result validate_s7(const uint8_t *frame, size_t frame_size,
                                  uint16_t reference, uint8_t function,
                                  size_t *parameter_offset, uint16_t *parameter_length,
                                  uint16_t *data_length) {
    size_t complete = 0U;
    if (!edge_s7_frame_length(frame, frame_size, &complete))
        return frame_size < 4U ? EDGE_S7_TRUNCATED : EDGE_S7_WRONG_RESPONSE;
    if (complete != frame_size || frame_size < 21U || frame[4] != 0x02U ||
        frame[5] != 0xf0U || frame[6] != 0x80U || frame[7] != 0x32U ||
        (frame[8] != 0x02U && frame[8] != 0x03U) || get_be16(frame + 11U) != reference)
        return EDGE_S7_WRONG_RESPONSE;

    const size_t offset = frame[8] == 0x03U ? 19U : 17U;
    const uint16_t parameters = get_be16(frame + 13U);
    const uint16_t payload = get_be16(frame + 15U);
    if (offset + (size_t)parameters + payload != frame_size || parameters < 2U ||
        frame[offset] != function)
        return EDGE_S7_WRONG_RESPONSE;
    if (frame[8] == 0x03U && (frame[17] != 0U || frame[18] != 0U))
        return EDGE_S7_PROTOCOL_ERROR;
    *parameter_offset = offset;
    *parameter_length = parameters;
    *data_length = payload;
    return EDGE_S7_OK;
}

edge_s7_result edge_s7_parse_setup(const uint8_t *frame, size_t frame_size,
                                   uint16_t reference, uint16_t *negotiated_pdu_length) {
    size_t offset = 0U;
    uint16_t parameter_length = 0U;
    uint16_t data_length = 0U;
    if (negotiated_pdu_length == NULL)
        return EDGE_S7_INVALID_ARGUMENT;
    const edge_s7_result result = validate_s7(frame, frame_size, reference, 0xf0U,
                                              &offset, &parameter_length, &data_length);
    if (result != EDGE_S7_OK)
        return result;
    if (parameter_length != 8U || data_length != 0U || offset + 8U > frame_size)
        return EDGE_S7_WRONG_RESPONSE;
    const uint16_t negotiated = get_be16(frame + offset + 6U);
    if (negotiated < 240U || negotiated > 4096U)
        return EDGE_S7_WRONG_RESPONSE;
    *negotiated_pdu_length = negotiated;
    return EDGE_S7_OK;
}

static size_t build_header(uint16_t reference, uint16_t parameter_length,
                           uint16_t data_length, uint8_t *output, size_t capacity) {
    const size_t total = 17U + parameter_length + data_length;
    if (output == NULL || total > capacity || total > UINT16_MAX)
        return 0U;
    output[0] = 0x03U;
    output[1] = 0U;
    put_be16(output + 2U, (uint16_t)total);
    output[4] = 0x02U;
    output[5] = 0xf0U;
    output[6] = 0x80U;
    output[7] = 0x32U;
    output[8] = 0x01U;
    output[9] = 0U;
    output[10] = 0U;
    put_be16(output + 11U, reference);
    put_be16(output + 13U, parameter_length);
    put_be16(output + 15U, data_length);
    return total;
}

static bool build_item(const edge_s7_address *address, uint8_t *output) {
    if (!valid_address(address))
        return false;
    const uint32_t bit_address = address->start_byte * 8U +
                                 (address->bit_access ? address->start_bit : 0U);
    output[0] = 0x12U;
    output[1] = 0x0aU;
    output[2] = 0x10U;
    output[3] = address->bit_access ? 0x01U : 0x02U;
    put_be16(output + 4U, address->bit_access ? 1U : address->size);
    put_be16(output + 6U, address->db_number);
    output[8] = (uint8_t)address->area;
    output[9] = (uint8_t)(bit_address >> 16U);
    output[10] = (uint8_t)(bit_address >> 8U);
    output[11] = (uint8_t)bit_address;
    return true;
}

size_t edge_s7_build_read(uint16_t reference, const edge_s7_address *address,
                          uint8_t *output, size_t capacity) {
    const size_t total = build_header(reference, 14U, 0U, output, capacity);
    if (total == 0U || !build_item(address, output + 19U))
        return 0U;
    output[17] = S7_READ_VAR;
    output[18] = 1U;
    return total;
}

size_t edge_s7_build_write(uint16_t reference, const edge_s7_address *address,
                           const uint8_t *data, size_t data_size,
                           uint8_t *output, size_t capacity) {
    if (!valid_address(address) || data == NULL || data_size == 0U ||
        data_size > UINT16_MAX / 8U ||
        (address->bit_access ? data_size != 1U : data_size != address->size))
        return 0U;
    const uint16_t data_length = (uint16_t)(4U + data_size);
    const size_t total = build_header(reference, 14U, data_length, output, capacity);
    if (total == 0U || !build_item(address, output + 19U))
        return 0U;
    output[17] = S7_WRITE_VAR;
    output[18] = 1U;
    const size_t offset = 31U;
    output[offset] = 0U;
    output[offset + 1U] = address->bit_access ? 0x03U : 0x04U;
    put_be16(output + offset + 2U,
             address->bit_access ? (uint16_t)data_size : (uint16_t)(data_size * 8U));
    memcpy(output + offset + 4U, data, data_size);
    return total;
}

edge_s7_result edge_s7_parse_read(const uint8_t *frame, size_t frame_size,
                                  uint16_t reference, uint8_t *data,
                                  size_t capacity, size_t *data_size,
                                  uint8_t *return_code) {
    size_t offset = 0U;
    uint16_t parameter_length = 0U;
    uint16_t payload_length = 0U;
    if (data_size == NULL || return_code == NULL)
        return EDGE_S7_INVALID_ARGUMENT;
    *data_size = 0U;
    *return_code = 0U;
    const edge_s7_result result = validate_s7(frame, frame_size, reference, S7_READ_VAR,
                                              &offset, &parameter_length, &payload_length);
    if (result != EDGE_S7_OK)
        return result;
    if (parameter_length != 2U || frame[offset + 1U] != 1U || payload_length < 4U)
        return EDGE_S7_WRONG_RESPONSE;
    const size_t item = offset + parameter_length;
    *return_code = frame[item];
    if (*return_code != 0xffU)
        return EDGE_S7_ACCESS_DENIED;
    const uint16_t encoded_bits = get_be16(frame + item + 2U);
    const size_t bytes = (encoded_bits + 7U) / 8U;
    if (bytes + 4U > payload_length || item + 4U + bytes > frame_size)
        return EDGE_S7_WRONG_RESPONSE;
    if (capacity < bytes || (bytes != 0U && data == NULL))
        return EDGE_S7_OUTPUT_TOO_SMALL;
    memcpy(data, frame + item + 4U, bytes);
    *data_size = bytes;
    return EDGE_S7_OK;
}

edge_s7_result edge_s7_parse_write(const uint8_t *frame, size_t frame_size,
                                   uint16_t reference, uint8_t *return_code) {
    size_t offset = 0U;
    uint16_t parameter_length = 0U;
    uint16_t payload_length = 0U;
    if (return_code == NULL)
        return EDGE_S7_INVALID_ARGUMENT;
    *return_code = 0U;
    const edge_s7_result result = validate_s7(frame, frame_size, reference, S7_WRITE_VAR,
                                              &offset, &parameter_length, &payload_length);
    if (result != EDGE_S7_OK)
        return result;
    if (parameter_length != 2U || frame[offset + 1U] != 1U || payload_length != 1U)
        return EDGE_S7_WRONG_RESPONSE;
    *return_code = frame[offset + parameter_length];
    return *return_code == 0xffU ? EDGE_S7_OK : EDGE_S7_ACCESS_DENIED;
}
