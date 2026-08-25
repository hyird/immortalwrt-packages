#include "edge_modbus.h"

#include <string.h>

static void put_be16(uint8_t *output, uint16_t value) {
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static uint16_t get_be16(const uint8_t *input) {
    return (uint16_t)(((uint16_t)input[0] << 8U) | input[1]);
}

uint16_t edge_modbus_crc16(const uint8_t *data, size_t size) {
    uint16_t crc = 0xffffU;
    if (data == NULL && size != 0U)
        return 0U;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8U; ++bit)
            crc = (crc & 1U) != 0U ? (uint16_t)((crc >> 1U) ^ 0xa001U)
                                   : (uint16_t)(crc >> 1U);
    }
    return crc;
}

static bool valid_base(const edge_modbus_request *request) {
    return request != NULL &&
           (request->transport == EDGE_MODBUS_TCP || request->transport == EDGE_MODBUS_RTU) &&
           request->unit_id <= 247U;
}

static bool valid_read(const edge_modbus_request *request) {
    if (!valid_base(request))
        return false;
    if (request->function == 1U || request->function == 2U)
        return request->quantity >= 1U && request->quantity <= 2000U;
    if (request->function == 3U || request->function == 4U)
        return request->quantity >= 1U && request->quantity <= 125U;
    return false;
}

static edge_modbus_result wrap_frame(const edge_modbus_request *request,
                                     const uint8_t *pdu, size_t pdu_size,
                                     uint8_t *output, size_t capacity,
                                     size_t *output_size) {
    const size_t prefix = request->transport == EDGE_MODBUS_TCP ? 7U : 1U;
    const size_t suffix = request->transport == EDGE_MODBUS_RTU ? 2U : 0U;
    const size_t total = prefix + pdu_size + suffix;
    if (capacity < total)
        return EDGE_MODBUS_OUTPUT_TOO_SMALL;

    if (request->transport == EDGE_MODBUS_TCP) {
        put_be16(output, request->transaction_id);
        output[2] = 0U;
        output[3] = 0U;
        put_be16(output + 4U, (uint16_t)(pdu_size + 1U));
        output[6] = request->unit_id;
    } else {
        output[0] = request->unit_id;
    }
    memcpy(output + prefix, pdu, pdu_size);
    if (request->transport == EDGE_MODBUS_RTU) {
        const uint16_t crc = edge_modbus_crc16(output, prefix + pdu_size);
        output[prefix + pdu_size] = (uint8_t)crc;
        output[prefix + pdu_size + 1U] = (uint8_t)(crc >> 8U);
    }
    *output_size = total;
    return EDGE_MODBUS_OK;
}

edge_modbus_result edge_modbus_build_read(const edge_modbus_request *request,
                                          uint8_t *output, size_t capacity,
                                          size_t *output_size) {
    if (output_size != NULL)
        *output_size = 0U;
    if (!valid_read(request) || output == NULL || output_size == NULL)
        return EDGE_MODBUS_INVALID_ARGUMENT;
    uint8_t pdu[5] = {request->function, 0U, 0U, 0U, 0U};
    put_be16(pdu + 1U, request->address);
    put_be16(pdu + 3U, request->quantity);
    return wrap_frame(request, pdu, sizeof(pdu), output, capacity, output_size);
}

edge_modbus_result edge_modbus_build_write(const edge_modbus_request *request,
                                           const uint8_t *data, size_t data_size,
                                           uint8_t *output, size_t capacity,
                                           size_t *output_size) {
    uint8_t pdu[254];
    size_t pdu_size = 0U;
    if (output_size != NULL)
        *output_size = 0U;
    if (!valid_base(request) || data == NULL || output == NULL || output_size == NULL)
        return EDGE_MODBUS_INVALID_ARGUMENT;

    if (request->function == 5U && request->quantity == 1U && data_size == 1U && data[0] <= 1U) {
        pdu[0] = 5U;
        put_be16(pdu + 1U, request->address);
        pdu[3] = data[0] != 0U ? 0xffU : 0U;
        pdu[4] = 0U;
        pdu_size = 5U;
    } else if (request->function == 6U && request->quantity == 1U && data_size == 2U) {
        pdu[0] = 6U;
        put_be16(pdu + 1U, request->address);
        memcpy(pdu + 3U, data, 2U);
        pdu_size = 5U;
    } else if (request->function == 15U && request->quantity >= 1U &&
               request->quantity <= 1968U &&
               data_size == (size_t)((request->quantity + 7U) / 8U) && data_size <= 246U) {
        pdu[0] = 15U;
        put_be16(pdu + 1U, request->address);
        put_be16(pdu + 3U, request->quantity);
        pdu[5] = (uint8_t)data_size;
        memcpy(pdu + 6U, data, data_size);
        pdu_size = 6U + data_size;
    } else if (request->function == 16U && request->quantity >= 1U &&
               request->quantity <= 123U && data_size == (size_t)request->quantity * 2U) {
        pdu[0] = 16U;
        put_be16(pdu + 1U, request->address);
        put_be16(pdu + 3U, request->quantity);
        pdu[5] = (uint8_t)data_size;
        memcpy(pdu + 6U, data, data_size);
        pdu_size = 6U + data_size;
    } else {
        return EDGE_MODBUS_INVALID_ARGUMENT;
    }
    return wrap_frame(request, pdu, pdu_size, output, capacity, output_size);
}

edge_modbus_result edge_modbus_parse_response(const edge_modbus_request *request,
                                              const uint8_t *frame, size_t frame_size,
                                              const uint8_t *expected_write_data,
                                              size_t expected_write_data_size,
                                              uint8_t *data, size_t capacity,
                                              size_t *data_size,
                                              uint8_t *exception_code) {
    const uint8_t *pdu;
    size_t pdu_size;
    if (data_size != NULL)
        *data_size = 0U;
    if (exception_code != NULL)
        *exception_code = 0U;
    if (!valid_base(request) || frame == NULL || data_size == NULL || exception_code == NULL)
        return EDGE_MODBUS_INVALID_ARGUMENT;

    if (request->transport == EDGE_MODBUS_TCP) {
        if (frame_size < 9U)
            return EDGE_MODBUS_TRUNCATED;
        const uint16_t length = get_be16(frame + 4U);
        if (get_be16(frame) != request->transaction_id || frame[2] != 0U || frame[3] != 0U ||
            frame[6] != request->unit_id || length < 2U || frame_size != (size_t)length + 6U)
            return EDGE_MODBUS_WRONG_RESPONSE;
        pdu = frame + 7U;
        pdu_size = frame_size - 7U;
    } else {
        if (frame_size < 5U)
            return EDGE_MODBUS_TRUNCATED;
        const uint16_t expected_crc = edge_modbus_crc16(frame, frame_size - 2U);
        if (frame[frame_size - 2U] != (uint8_t)expected_crc ||
            frame[frame_size - 1U] != (uint8_t)(expected_crc >> 8U))
            return EDGE_MODBUS_BAD_CRC;
        if (frame[0] != request->unit_id)
            return EDGE_MODBUS_WRONG_RESPONSE;
        pdu = frame + 1U;
        pdu_size = frame_size - 3U;
    }

    if (pdu_size >= 2U && pdu[0] == (uint8_t)(request->function | 0x80U)) {
        *exception_code = pdu[1];
        return EDGE_MODBUS_EXCEPTION;
    }
    if (pdu_size == 0U || pdu[0] != request->function)
        return EDGE_MODBUS_WRONG_RESPONSE;

    if (request->function >= 1U && request->function <= 4U) {
        const size_t expected = request->function <= 2U
                                    ? (size_t)((request->quantity + 7U) / 8U)
                                    : (size_t)request->quantity * 2U;
        if (pdu_size != expected + 2U || pdu[1] != expected)
            return EDGE_MODBUS_WRONG_RESPONSE;
        if (capacity < expected || (expected != 0U && data == NULL))
            return EDGE_MODBUS_OUTPUT_TOO_SMALL;
        memcpy(data, pdu + 2U, expected);
        *data_size = expected;
        return EDGE_MODBUS_OK;
    }

    if ((request->function == 5U || request->function == 6U ||
         request->function == 15U || request->function == 16U) &&
        pdu_size == 5U && get_be16(pdu + 1U) == request->address) {
        if (request->function == 5U) {
            if (expected_write_data == NULL || expected_write_data_size != 1U ||
                expected_write_data[0] > 1U || pdu[3] != (expected_write_data[0] ? 0xffU : 0U) ||
                pdu[4] != 0U)
                return EDGE_MODBUS_WRONG_RESPONSE;
        }
        if (request->function == 6U &&
            (expected_write_data == NULL || expected_write_data_size != 2U ||
             memcmp(pdu + 3U, expected_write_data, 2U) != 0))
            return EDGE_MODBUS_WRONG_RESPONSE;
        if ((request->function == 15U || request->function == 16U) &&
            get_be16(pdu + 3U) != request->quantity)
            return EDGE_MODBUS_WRONG_RESPONSE;
        return EDGE_MODBUS_OK;
    }
    return EDGE_MODBUS_WRONG_RESPONSE;
}
