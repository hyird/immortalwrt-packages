#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "edge_acquisition.h"

static uint64_t monotonic_ms(void) {
    struct timespec value;
    assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void set_id(void *field, const uint8_t id[16]) {
    const pb_size_t size = 16U;
    memcpy(field, &size, sizeof(size));
    memcpy((uint8_t *)field + sizeof(size), id, 16U);
}

static void copy_text(char *output, size_t capacity, const char *input) {
    snprintf(output, capacity, "%s", input);
}

static bool telemetry(void *context, const iot_edge_v1_TelemetryRecord *record) {
    (void)context;
    (void)record;
    return true;
}

static bool command(void *context, const iot_edge_v1_CommandResult *result) {
    (void)context;
    (void)result;
    return true;
}

static edge_runtime_config make_config(iot_edge_v1_ConfigItem values[3]) {
    const uint8_t endpoint_id[16] = {1U};
    const uint8_t device_id[16] = {2U};
    values[0] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    values[0].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    values[0].which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    set_id(&values[0].item.endpoint.endpoint_id, endpoint_id);
    copy_text(values[0].item.endpoint.name, sizeof(values[0].item.endpoint.name),
              "offline endpoint");
    values[0].item.endpoint.transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    values[0].item.endpoint.mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    values[0].item.endpoint.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    copy_text(values[0].item.endpoint.ip, sizeof(values[0].item.endpoint.ip),
              "127.0.0.1");
    values[0].item.endpoint.port = 1U;
    values[0].item.endpoint.enabled = true;

    values[1] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    values[1].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DEVICE;
    values[1].which_item = iot_edge_v1_ConfigItem_device_tag;
    set_id(&values[1].item.device.device_id, device_id);
    set_id(&values[1].item.device.endpoint_id, endpoint_id);
    copy_text(values[1].item.device.device_code,
              sizeof(values[1].item.device.device_code), "OFFLINE01");
    values[1].item.device.protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    values[1].item.device.io_interval_ms = 1000U;
    values[1].item.device.report_interval_sec = 5U;
    values[1].item.device.modbus_slave_id = 1U;
    copy_text(values[1].item.device.modbus_mode,
              sizeof(values[1].item.device.modbus_mode), "TCP");
    values[1].item.device.enabled = true;

    values[2] = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    values[2].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_MODBUS_REGISTER;
    values[2].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    set_id(&values[2].item.modbus_register.device_id, device_id);
    copy_text(values[2].item.modbus_register.element_id,
              sizeof(values[2].item.modbus_register.element_id), "holding-1");
    copy_text(values[2].item.modbus_register.register_type,
              sizeof(values[2].item.modbus_register.register_type),
              "HOLDING_REGISTER");
    copy_text(values[2].item.modbus_register.data_type,
              sizeof(values[2].item.modbus_register.data_type), "UINT16");
    copy_text(values[2].item.modbus_register.byte_order,
              sizeof(values[2].item.modbus_register.byte_order), "BIG_ENDIAN");
    values[2].item.modbus_register.quantity = 1U;
    values[2].item.modbus_register.scale = 1.0;

    return (edge_runtime_config){
        .revision = 1U,
        .items = values,
        .item_count = 3U,
        .endpoint_count = 1U,
        .device_count = 1U,
    };
}

static void wait_for_status(edge_acquisition *acquisition) {
    const int fd = edge_acquisition_event_fd(acquisition);
    assert(fd >= 0);
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    assert(poll(&descriptor, 1U, 3000) == 1);
    edge_acquisition_tick(acquisition, monotonic_ms());
    iot_edge_v1_DeviceStatusReport report =
        iot_edge_v1_DeviceStatusReport_init_zero;
    edge_acquisition_status(acquisition, &report);
    assert(report.devices_count == 1U);
    assert(report.devices[0].device_id.size == 16U);
    assert(report.devices[0].device_id.bytes[0] == 2U);
    assert(strcmp(report.devices[0].state, "reconnecting") == 0);
}

int main(void) {
    iot_edge_v1_ConfigItem values[3];
    edge_runtime_config config = make_config(values);
    edge_acquisition *acquisition = edge_acquisition_create(telemetry, command, NULL);
    assert(acquisition != NULL);
    char error[256] = {0};
    assert(edge_acquisition_apply(acquisition, &config, monotonic_ms(),
                                  error, sizeof(error)));
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    wait_for_status(acquisition);
    edge_acquisition_stop(acquisition);
    assert(edge_acquisition_event_fd(acquisition) == -1);
    assert(edge_acquisition_start(acquisition, error, sizeof(error)));
    wait_for_status(acquisition);
    edge_acquisition_destroy(acquisition);
    return 0;
}
