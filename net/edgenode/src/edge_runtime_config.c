#include "edge_runtime_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0U)
        snprintf(error, size, "%s", message);
}

static bool same_id(const void *field, const uint8_t id[16]) {
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    return size == 16U && memcmp((const uint8_t *)field + sizeof(size), id, 16U) == 0;
}

static const uint8_t *id_bytes(const void *field) {
    return (const uint8_t *)field + sizeof(pb_size_t);
}

void edge_runtime_config_free(edge_runtime_config *config) {
    if (config == NULL)
        return;
    free(config->items);
    memset(config, 0, sizeof(*config));
}

const iot_edge_v1_EndpointConfig *
edge_runtime_config_endpoint(const edge_runtime_config *config, const uint8_t id[16]) {
    if (config == NULL || id == NULL)
        return NULL;
    for (uint32_t index = 0; index < config->item_count; ++index) {
        const iot_edge_v1_ConfigItem *item = &config->items[index];
        if (item->which_item == iot_edge_v1_ConfigItem_endpoint_tag &&
            same_id(&item->item.endpoint.endpoint_id, id))
            return &item->item.endpoint;
    }
    return NULL;
}

const iot_edge_v1_DeviceConfig *
edge_runtime_config_device(const edge_runtime_config *config, const uint8_t id[16]) {
    if (config == NULL || id == NULL)
        return NULL;
    for (uint32_t index = 0; index < config->item_count; ++index) {
        const iot_edge_v1_ConfigItem *item = &config->items[index];
        if (item->which_item == iot_edge_v1_ConfigItem_device_tag &&
            same_id(&item->item.device.device_id, id))
            return &item->item.device;
    }
    return NULL;
}

static bool valid_endpoint(const iot_edge_v1_EndpointConfig *value) {
    if (value->endpoint_id.size != 16U || value->protocol ==
                                                    iot_edge_v1_Protocol_PROTOCOL_UNSPECIFIED)
        return false;
    if (value->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        return value->mode == iot_edge_v1_LinkMode_LINK_MODE_SERIAL && value->has_serial &&
               value->serial.channel[0] != '\0' && value->serial.baud_rate >= 300U &&
               value->serial.data_bits >= 5U && value->serial.data_bits <= 8U &&
               value->serial.stop_bits >= 1U && value->serial.stop_bits <= 2U;
    if (value->transport == iot_edge_v1_Transport_TRANSPORT_ETHERNET)
        return (value->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT ||
                value->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER) &&
               value->ip[0] != '\0' && value->port != 0U && value->port <= 65535U;
    return false;
}

static bool valid_device(const edge_runtime_config *config,
                         const iot_edge_v1_DeviceConfig *value) {
    if (value->device_id.size != 16U || value->endpoint_id.size != 16U ||
        value->device_code[0] == '\0' || value->report_interval_sec == 0U ||
        (value->io_interval_ms != 0U && value->io_interval_ms != 1000U) ||
        (value->protocol != iot_edge_v1_Protocol_PROTOCOL_MODBUS &&
         value->protocol != iot_edge_v1_Protocol_PROTOCOL_S7))
        return false;
    const iot_edge_v1_EndpointConfig *endpoint =
        edge_runtime_config_endpoint(config, value->endpoint_id.bytes);
    return endpoint != NULL && endpoint->protocol == value->protocol;
}

static bool valid_point(const edge_runtime_config *config,
                        const iot_edge_v1_ConfigItem *item) {
    const void *device_id = NULL;
    iot_edge_v1_Protocol expected = iot_edge_v1_Protocol_PROTOCOL_UNSPECIFIED;
    switch (item->which_item) {
    case iot_edge_v1_ConfigItem_modbus_register_tag: {
        const iot_edge_v1_ModbusRegisterConfig *point = &item->item.modbus_register;
        const bool bit = strcmp(point->register_type, "COIL") == 0 ||
                         strcmp(point->register_type, "DISCRETE_INPUT") == 0;
        const bool word = strcmp(point->register_type, "HOLDING_REGISTER") == 0 ||
                          strcmp(point->register_type, "INPUT_REGISTER") == 0;
        const bool order = strcmp(point->byte_order, "BIG_ENDIAN") == 0 ||
                           strcmp(point->byte_order, "LITTLE_ENDIAN") == 0 ||
                           strcmp(point->byte_order, "BIG_ENDIAN_BYTE_SWAP") == 0 ||
                           strcmp(point->byte_order, "LITTLE_ENDIAN_BYTE_SWAP") == 0;
        if (point->element_id[0] == '\0' || (!bit && !word) || !order ||
            point->address > 65535U || point->quantity == 0U ||
            (bit && point->quantity > 2000U) || (word && point->quantity > 125U) ||
            (bit && strcmp(point->data_type, "BOOL") != 0) ||
            (point->writable && strcmp(point->register_type, "DISCRETE_INPUT") == 0) ||
            (point->writable && strcmp(point->register_type, "INPUT_REGISTER") == 0))
            return false;
        device_id = &item->item.modbus_register.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
        break;
    }
    case iot_edge_v1_ConfigItem_s7_area_tag: {
        const iot_edge_v1_S7AreaConfig *point = &item->item.s7_area;
        if (point->element_id[0] == '\0' || point->size == 0U || point->size > 512U ||
            point->start > 0x1fffffU || point->start_bit > 7U)
            return false;
        device_id = &item->item.s7_area.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_S7;
        break;
    }
    case iot_edge_v1_ConfigItem_sl651_function_tag:
        device_id = &item->item.sl651_function.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_SL651;
        break;
    case iot_edge_v1_ConfigItem_sl651_element_tag:
        device_id = &item->item.sl651_element.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_SL651;
        break;
    case iot_edge_v1_ConfigItem_sl651_dictionary_tag:
        device_id = &item->item.sl651_dictionary.device_id;
        expected = iot_edge_v1_Protocol_PROTOCOL_SL651;
        break;
    default:
        return false;
    }
    pb_size_t size = 0U;
    memcpy(&size, device_id, sizeof(size));
    if (size != 16U)
        return false;
    const iot_edge_v1_DeviceConfig *device =
        edge_runtime_config_device(config, id_bytes(device_id));
    return device != NULL && device->protocol == expected;
}

bool edge_runtime_config_load(edge_runtime_config *output,
                              const edge_memory_config_set *snapshot,
                              char *error, size_t error_size) {
    if (output == NULL || snapshot == NULL || snapshot->revision == 0U ||
        snapshot->received_count != snapshot->item_count) {
        set_error(error, error_size, "configuration snapshot is incomplete");
        return false;
    }
    edge_runtime_config candidate = {0};
    candidate.revision = snapshot->revision;
    candidate.item_count = snapshot->item_count;
    if (candidate.item_count != 0U) {
        candidate.items = calloc(candidate.item_count, sizeof(*candidate.items));
        if (candidate.items == NULL) {
            set_error(error, error_size, "configuration memory allocation failed");
            return false;
        }
    }
    for (uint32_t index = 0; index < candidate.item_count; ++index) {
        const edge_memory_config_item *source = &snapshot->items[index];
        pb_istream_t stream = pb_istream_from_buffer(source->payload, source->payload_size);
        candidate.items[index] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
        if (!source->present ||
            !pb_decode(&stream, iot_edge_v1_ConfigItem_fields, &candidate.items[index])) {
            edge_runtime_config_free(&candidate);
            set_error(error, error_size, "configuration item decode failed");
            return false;
        }
        const iot_edge_v1_ConfigItem *item = &candidate.items[index];
        if (item->which_item == iot_edge_v1_ConfigItem_endpoint_tag)
            ++candidate.endpoint_count;
        else if (item->which_item == iot_edge_v1_ConfigItem_device_tag)
            ++candidate.device_count;
    }
    for (uint32_t index = 0; index < candidate.item_count; ++index) {
        const iot_edge_v1_ConfigItem *item = &candidate.items[index];
        bool valid = true;
        if (item->revision != candidate.revision || item->index != index)
            valid = false;
        else if (item->which_item == iot_edge_v1_ConfigItem_endpoint_tag)
            valid = valid_endpoint(&item->item.endpoint);
        else if (item->which_item == iot_edge_v1_ConfigItem_device_tag)
            valid = valid_device(&candidate, &item->item.device);
        else
            valid = valid_point(&candidate, item);
        if (!valid) {
            edge_runtime_config_free(&candidate);
            set_error(error, error_size, "configuration reference or value is invalid");
            return false;
        }
    }
    edge_runtime_config_free(output);
    *output = candidate;
    return true;
}
