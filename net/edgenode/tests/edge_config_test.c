#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_encode.h>

#include "edge_runtime_config.h"

static void require_true(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge config test failed: %s\n", message);
        exit(1);
    }
}

static void copy_text(char *output, size_t capacity, const char *input) {
    snprintf(output, capacity, "%s", input);
}

static size_t encode_item(const iot_edge_v1_ConfigItem *item, uint8_t *output,
                          size_t capacity) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    require_true(pb_encode(&stream, iot_edge_v1_ConfigItem_fields, item),
                 "cannot encode config item");
    return stream.bytes_written;
}

static void set_id(void *field, size_t capacity, const uint8_t id[16]) {
    const pb_size_t size = 16U;
    memcpy(field, &size, sizeof(size));
    require_true(capacity >= 16U, "invalid bytes capacity");
    memcpy((uint8_t *)field + sizeof(size), id, 16U);
}

static void test_valid_snapshot(void) {
    const uint8_t id[16] = {1U, 2U, 3U};
    iot_edge_v1_ConfigItem values[3] = {
        iot_edge_v1_ConfigItem_init_zero,
        iot_edge_v1_ConfigItem_init_zero,
        iot_edge_v1_ConfigItem_init_zero};

    values[0].revision = 7U;
    values[0].index = 0U;
    values[0].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    values[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    set_id(&values[0].item.endpoint.endpoint_id,
           sizeof(values[0].item.endpoint.endpoint_id.bytes), id);
    copy_text(values[0].item.endpoint.name, sizeof(values[0].item.endpoint.name),
              "PLC endpoint");
    values[0].item.endpoint.transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    values[0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    values[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    copy_text(values[0].item.endpoint.ip, sizeof(values[0].item.endpoint.ip), "192.168.1.8");
    values[0].item.endpoint.port = 502U;
    values[0].item.endpoint.enabled = true;

    values[1].revision = 7U;
    values[1].index = 1U;
    values[1].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DEVICE;
    values[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    set_id(&values[1].item.device.device_id,
           sizeof(values[1].item.device.device_id.bytes), id);
    set_id(&values[1].item.device.endpoint_id,
           sizeof(values[1].item.device.endpoint_id.bytes), id);
    copy_text(values[1].item.device.device_code, sizeof(values[1].item.device.device_code),
              "PLC01");
    values[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    values[1].item.device.io_interval_ms = 1000U;
    values[1].item.device.report_interval_sec = 5U;
    values[1].item.device.modbus_slave_id = 1U;
    copy_text(values[1].item.device.modbus_mode, sizeof(values[1].item.device.modbus_mode),
              "TCP");
    values[1].item.device.enabled = true;

    values[2].revision = 7U;
    values[2].index = 2U;
    values[2].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_MODBUS_REGISTER;
    values[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    set_id(&values[2].item.modbus_register.device_id,
           sizeof(values[2].item.modbus_register.device_id.bytes), id);
    copy_text(values[2].item.modbus_register.element_id,
              sizeof(values[2].item.modbus_register.element_id),
              "00000000-0000-7000-8000-000000000001");
    copy_text(values[2].item.modbus_register.name,
              sizeof(values[2].item.modbus_register.name), "Pressure");
    copy_text(values[2].item.modbus_register.register_type,
              sizeof(values[2].item.modbus_register.register_type), "HOLDING_REGISTER");
    copy_text(values[2].item.modbus_register.data_type,
              sizeof(values[2].item.modbus_register.data_type), "FLOAT32");
    copy_text(values[2].item.modbus_register.byte_order,
              sizeof(values[2].item.modbus_register.byte_order),
              "LITTLE_ENDIAN_BYTE_SWAP");
    values[2].item.modbus_register.quantity = 2U;
    values[2].item.modbus_register.scale = 1.0;
    values[2].item.modbus_register.decimals = 2;
    values[2].item.modbus_register.writable = true;

    uint8_t payloads[3][iot_edge_v1_ConfigItem_size];
    edge_memory_config_item items[3] = {0};
    for (size_t index = 0U; index < 3U; ++index) {
        items[index].payload = payloads[index];
        items[index].payload_size = encode_item(&values[index], payloads[index],
                                                sizeof(payloads[index]));
        items[index].present = true;
    }
    edge_memory_config_set snapshot = {
        .revision = 7U, .item_count = 3U, .received_count = 3U, .items = items};
    edge_runtime_config runtime = {0};
    char error[256] = {0};
    require_true(edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)), error);
    require_true(runtime.endpoint_count == 1U && runtime.device_count == 1U,
                 "snapshot counts are wrong");
    require_true(strcmp(runtime.items[2].item.modbus_register.byte_order,
                        "LITTLE_ENDIAN_BYTE_SWAP") == 0,
                 "nanopb byte order was truncated");
    edge_runtime_config_free(&runtime);
}

static void test_unknown_item_rejected(void) {
    iot_edge_v1_ConfigItem value = iot_edge_v1_ConfigItem_init_zero;
    value.revision = 9U;
    value.index = 0U;
    uint8_t payload[iot_edge_v1_ConfigItem_size];
    edge_memory_config_item item = {.payload = payload,
                                    .payload_size = encode_item(&value, payload, sizeof(payload)),
                                    .present = true};
    edge_memory_config_set snapshot = {
        .revision = 9U, .item_count = 1U, .received_count = 1U, .items = &item};
    edge_runtime_config runtime = {0};
    char error[128] = {0};
    require_true(!edge_runtime_config_load(&runtime, &snapshot, error, sizeof(error)),
                 "unknown config item was accepted");
}

int main(void) {
    test_valid_snapshot();
    test_unknown_item_rejected();
    puts("edge config tests passed");
    return 0;
}
