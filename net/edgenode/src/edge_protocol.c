#include "edge_protocol.h"

#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"

bool edge_protocol_validate_imei(const char *imei) {
    if (imei == NULL || strlen(imei) != 15U)
        return false;

    unsigned sum = 0;
    for (size_t index = 0; index < 15U; ++index) {
        const unsigned char ch = (unsigned char)imei[index];
        if (ch < (unsigned char)'0' || ch > (unsigned char)'9')
            return false;

        unsigned digit = (unsigned)(ch - (unsigned char)'0');
        if ((index & 1U) != 0U) {
            digit *= 2U;
            if (digit > 9U)
                digit -= 9U;
        }
        sum += digit;
    }
    return (sum % 10U) == 0U;
}

void edge_protocol_uuid_v7(uint64_t now_ms, const uint8_t random_bytes[10],
                           uint8_t output[16]) {
    output[0] = (uint8_t)(now_ms >> 40U);
    output[1] = (uint8_t)(now_ms >> 32U);
    output[2] = (uint8_t)(now_ms >> 24U);
    output[3] = (uint8_t)(now_ms >> 16U);
    output[4] = (uint8_t)(now_ms >> 8U);
    output[5] = (uint8_t)now_ms;
    output[6] = (uint8_t)(0x70U | (random_bytes[0] & 0x0fU));
    output[7] = random_bytes[1];
    output[8] = (uint8_t)(0x80U | (random_bytes[2] & 0x3fU));
    memcpy(output + 9, random_bytes + 3, 7U);
}

bool edge_protocol_set_bytes(void *field, size_t field_capacity, const uint8_t *data,
                             size_t data_size) {
    if (field == NULL || data_size > field_capacity || (data_size != 0U && data == NULL))
        return false;

    const pb_size_t encoded_size = (pb_size_t)data_size;
    /* PB_BYTES_ARRAY_T always stores pb_size_t followed by its byte array. Use
     * memcpy so this generic helper does not violate strict-aliasing under LTO. */
    memcpy(field, &encoded_size, sizeof(encoded_size));
    if (data_size != 0U)
        memcpy((uint8_t *)field + sizeof(encoded_size), data, data_size);
    return true;
}

bool edge_protocol_init_envelope(iot_edge_v1_Envelope *envelope,
                                 const uint8_t platform_id[16], const uint8_t *node_id,
                                 uint64_t session_epoch, uint64_t sequence, int64_t now_ms,
                                 const uint8_t random_bytes[10]) {
    if (envelope == NULL || platform_id == NULL || random_bytes == NULL || now_ms < 0)
        return false;

    *envelope = (iot_edge_v1_Envelope)iot_edge_v1_Envelope_init_zero;
    envelope->protocol_version = EDGENODE_PROTOCOL_VERSION;
    envelope->session_epoch = session_epoch;
    envelope->created_at_ms = now_ms;
    envelope->sequence = sequence;

    uint8_t message_id[16];
    edge_protocol_uuid_v7((uint64_t)now_ms, random_bytes, message_id);
    if (!edge_protocol_set_bytes(&envelope->message_id, sizeof(envelope->message_id.bytes),
                                 message_id, sizeof(message_id)) ||
        !edge_protocol_set_bytes(&envelope->platform_id, sizeof(envelope->platform_id.bytes),
                                 platform_id, 16U))
        return false;

    if (node_id != NULL &&
        !edge_protocol_set_bytes(&envelope->node_id, sizeof(envelope->node_id.bytes), node_id,
                                 16U))
        return false;
    return true;
}

bool edge_protocol_encode(const iot_edge_v1_Envelope *envelope, uint8_t *output,
                          size_t output_capacity, size_t *output_size, const char **error) {
    if (output_size != NULL)
        *output_size = 0U;
    if (error != NULL)
        *error = NULL;
    if (envelope == NULL || output == NULL || output_size == NULL ||
        output_capacity > EDGENODE_MAX_WS_MESSAGE) {
        if (error != NULL)
            *error = "invalid encode arguments";
        return false;
    }

    pb_ostream_t stream = pb_ostream_from_buffer(output, output_capacity);
    if (!pb_encode(&stream, iot_edge_v1_Envelope_fields, envelope)) {
        if (error != NULL)
            *error = PB_GET_ERROR(&stream);
        return false;
    }
    *output_size = stream.bytes_written;
    return true;
}

bool edge_protocol_decode(const uint8_t *input, size_t input_size,
                          iot_edge_v1_Envelope *envelope, const char **error) {
    if (error != NULL)
        *error = NULL;
    if (input == NULL || envelope == NULL || input_size == 0U ||
        input_size > EDGENODE_MAX_WS_MESSAGE) {
        if (error != NULL)
            *error = "invalid or oversized WebSocket message";
        return false;
    }

    *envelope = (iot_edge_v1_Envelope)iot_edge_v1_Envelope_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(input, input_size);
    if (!pb_decode(&stream, iot_edge_v1_Envelope_fields, envelope)) {
        if (error != NULL)
            *error = PB_GET_ERROR(&stream);
        return false;
    }
    if (envelope->protocol_version != EDGENODE_PROTOCOL_VERSION ||
        envelope->message_id.size != 16U || envelope->platform_id.size != 16U) {
        if (error != NULL)
            *error = "invalid envelope identity or protocol version";
        return false;
    }
    return true;
}
