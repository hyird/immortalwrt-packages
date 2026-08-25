#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge_device_runtime.h"
#include "edge_modbus.h"
#include "edge_s7.h"

static void require_true(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void test_modbus(void) {
    edge_modbus_request read = {.transport = EDGE_MODBUS_TCP,
                                 .transaction_id = 0x1234,
                                 .unit_id = 1,
                                 .function = 3,
                                 .address = 2,
                                 .quantity = 2};
    uint8_t frame[EDGE_MODBUS_MAX_FRAME];
    size_t size = 0U;
    require_true(edge_modbus_build_read(&read, frame, sizeof(frame), &size) == EDGE_MODBUS_OK,
                 "Modbus TCP read build failed");
    const uint8_t expected_read[] = {0x12, 0x34, 0, 0, 0, 6, 1, 3, 0, 2, 0, 2};
    require_true(size == sizeof(expected_read) && memcmp(frame, expected_read, size) == 0,
                 "Modbus TCP read frame differs from wire contract");

    const uint8_t read_response[] = {0x12, 0x34, 0, 0, 0, 7, 1, 3, 4, 0x11, 0x22, 0x33, 0x44};
    uint8_t value[8];
    size_t value_size = 0U;
    uint8_t exception = 0U;
    require_true(edge_modbus_parse_response(&read, read_response, sizeof(read_response),
                                             NULL, 0U, value, sizeof(value), &value_size,
                                             &exception) == EDGE_MODBUS_OK,
                 "Modbus TCP read response parse failed");
    require_true(value_size == 4U && value[0] == 0x11U && value[3] == 0x44U,
                 "Modbus read response data is wrong");

    edge_modbus_request write = read;
    write.transaction_id = 9U;
    write.function = 6U;
    write.quantity = 1U;
    const uint8_t desired[] = {0xab, 0xcd};
    require_true(edge_modbus_build_write(&write, desired, sizeof(desired), frame,
                                          sizeof(frame), &size) == EDGE_MODBUS_OK,
                 "Modbus FC06 build failed");
    require_true(edge_modbus_parse_response(&write, frame, size, desired, sizeof(desired),
                                             value, sizeof(value), &value_size,
                                             &exception) == EDGE_MODBUS_OK,
                 "Modbus FC06 echo validation failed");
    const uint8_t wrong[] = {0xab, 0xce};
    require_true(edge_modbus_parse_response(&write, frame, size, wrong, sizeof(wrong),
                                             value, sizeof(value), &value_size,
                                             &exception) == EDGE_MODBUS_WRONG_RESPONSE,
                 "Modbus accepted a write response with the wrong echoed value");

    edge_modbus_request rtu = {.transport = EDGE_MODBUS_RTU,
                                .unit_id = 1,
                                .function = 3,
                                .address = 0,
                                .quantity = 1};
    require_true(edge_modbus_build_read(&rtu, frame, sizeof(frame), &size) == EDGE_MODBUS_OK,
                 "Modbus RTU read build failed");
    require_true(size == 8U && edge_modbus_crc16(frame, 6U) ==
                                  (uint16_t)(frame[6] | ((uint16_t)frame[7] << 8U)),
                 "Modbus RTU CRC is wrong");
}

static void test_s7(void) {
    uint8_t frame[EDGE_S7_MAX_FRAME];
    size_t size = edge_s7_build_cotp_connect(0x0100U, 0x0101U, frame, sizeof(frame));
    const uint8_t expected_cotp[] = {0x03, 0x00, 0x00, 0x16, 0x11, 0xe0, 0x00, 0x00,
                                     0x00, 0x01, 0x00, 0xc0, 0x01, 0x0a, 0xc1, 0x02,
                                     0x01, 0x00, 0xc2, 0x02, 0x01, 0x01};
    require_true(size == sizeof(expected_cotp) && memcmp(frame, expected_cotp, size) == 0,
                 "S7 COTP connect request is wrong");
    const uint8_t cotp_confirm[] = {0x03, 0x00, 0x00, 0x0b, 0x06, 0xd0,
                                    0x00, 0x01, 0x00, 0x06, 0x00};
    require_true(edge_s7_parse_cotp_confirm(cotp_confirm, sizeof(cotp_confirm)) == EDGE_S7_OK,
                 "S7 COTP confirmation rejected");

    size = edge_s7_build_setup(7U, EDGE_S7_DEFAULT_PDU_LENGTH, frame, sizeof(frame));
    require_true(size == 25U && frame[11] == 0U && frame[12] == 7U &&
                     frame[23] == 0x01U && frame[24] == 0xe0U,
                 "S7 setup request is wrong");
    const uint8_t setup[] = {0x03, 0x00, 0x00, 0x1b, 0x02, 0xf0, 0x80, 0x32, 0x03,
                             0x00, 0x00, 0x00, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00,
                             0x00, 0xf0, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0xe0};
    uint16_t pdu = 0U;
    require_true(edge_s7_parse_setup(setup, sizeof(setup), 7U, &pdu) == EDGE_S7_OK &&
                     pdu == EDGE_S7_DEFAULT_PDU_LENGTH,
                 "S7 setup response parse failed");

    const edge_s7_address address = {.area = EDGE_S7_AREA_DB,
                                     .db_number = 1,
                                     .start_byte = 2,
                                     .start_bit = 0,
                                     .size = 2,
                                     .bit_access = false};
    size = edge_s7_build_read(8U, &address, frame, sizeof(frame));
    require_true(size == 31U && frame[17] == 4U && frame[18] == 1U &&
                     frame[25] == 0U && frame[26] == 1U && frame[27] == 0x84U &&
                     frame[30] == 16U,
                 "S7 ReadVar request is wrong");
    const uint8_t read_response[] = {0x03, 0x00, 0x00, 0x1b, 0x02, 0xf0, 0x80, 0x32, 0x03,
                                     0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x06, 0x00,
                                     0x00, 0x04, 0x01, 0xff, 0x04, 0x00, 0x10, 0x12, 0x34};
    uint8_t data[8];
    size_t data_size = 0U;
    uint8_t return_code = 0U;
    require_true(edge_s7_parse_read(read_response, sizeof(read_response), 8U, data,
                                    sizeof(data), &data_size, &return_code) == EDGE_S7_OK,
                 "S7 ReadVar response parse failed");
    require_true(data_size == 2U && data[0] == 0x12U && data[1] == 0x34U,
                 "S7 ReadVar returned the wrong bytes");

    const uint8_t desired[] = {0x12, 0x34};
    size = edge_s7_build_write(9U, &address, desired, sizeof(desired), frame, sizeof(frame));
    require_true(size == 37U && frame[17] == 5U && frame[31] == 0U &&
                     frame[32] == 4U && frame[35] == 0x12U && frame[36] == 0x34U,
                 "S7 WriteVar request is wrong");
    const uint8_t write_response[] = {0x03, 0x00, 0x00, 0x16, 0x02, 0xf0, 0x80, 0x32, 0x03,
                                      0x00, 0x00, 0x00, 0x09, 0x00, 0x02, 0x00, 0x01, 0x00,
                                      0x00, 0x05, 0x01, 0xff};
    require_true(edge_s7_parse_write(write_response, sizeof(write_response), 9U,
                                     &return_code) == EDGE_S7_OK,
                 "S7 WriteVar response parse failed");
}

typedef struct {
    unsigned connects;
    unsigned handshakes;
    unsigned reads;
    unsigned writes;
    unsigned disconnects;
    unsigned reports;
    unsigned completions;
    edge_command_result last_command_result;
    edge_io_result next_read_result;
    uint8_t last_platform[16];
} fake_device;

static edge_io_result fake_connect(void *context) {
    ++((fake_device *)context)->connects;
    return EDGE_IO_OK;
}

static edge_io_result fake_handshake(void *context) {
    ++((fake_device *)context)->handshakes;
    return EDGE_IO_OK;
}

static edge_io_result fake_read(void *context, edge_device_sample *sample) {
    fake_device *fake = context;
    ++fake->reads;
    const edge_io_result result = fake->next_read_result;
    fake->next_read_result = EDGE_IO_OK;
    if (result != EDGE_IO_OK)
        return result;
    sample->bytes[0] = (uint8_t)fake->reads;
    sample->size = 1U;
    return EDGE_IO_OK;
}

static edge_io_result fake_write(void *context, const edge_write_command *command,
                                 edge_device_sample *actual) {
    fake_device *fake = context;
    ++fake->writes;
    memcpy(actual->bytes, command->value, command->value_size);
    actual->size = command->value_size;
    return EDGE_IO_OK;
}

static void fake_disconnect(void *context) {
    ++((fake_device *)context)->disconnects;
}

static void fake_report(void *context, const uint8_t platform_id[16],
                        const uint8_t device_id[16], const edge_device_sample *sample) {
    fake_device *fake = context;
    (void)device_id;
    require_true(sample->size != 0U, "runtime reported an empty sample");
    ++fake->reports;
    memcpy(fake->last_platform, platform_id, 16U);
}

static void fake_complete(void *context, const uint8_t platform_id[16],
                          const uint8_t device_id[16], const uint8_t command_id[16],
                          edge_command_result result, const edge_device_sample *actual) {
    fake_device *fake = context;
    (void)platform_id;
    (void)device_id;
    (void)command_id;
    require_true(actual != NULL, "successful command omitted readback");
    ++fake->completions;
    fake->last_command_result = result;
}

static void test_fixed_io_and_reporting(void) {
    uint8_t platform_id[16] = {1U};
    uint8_t device_id[16] = {2U};
    fake_device fake = {0};
    edge_device_driver driver = {.connect = fake_connect,
                                 .handshake = fake_handshake,
                                 .read = fake_read,
                                 .write_readback = fake_write,
                                 .disconnect = fake_disconnect,
                                 .report = fake_report,
                                 .command_complete = fake_complete};
    edge_device_runtime runtime;
    require_true(!edge_device_runtime_init(&runtime, EDGE_DEVICE_S7, platform_id, device_id,
                                           2000U, 3U, 0U, &driver, &fake),
                 "runtime accepted a non-one-second DTU interval");
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_S7, platform_id, device_id,
                                          EDGE_DTU_IO_PERIOD_MS, 3U, 0U, &driver, &fake),
                 "runtime initialization failed");

    edge_device_runtime_tick(&runtime, 0U, 1000000);
    edge_device_runtime_tick(&runtime, 500U, 1000500);
    require_true(fake.reads == 1U && fake.reports == 0U,
                 "DTU read ran outside the fixed one-second cadence");

    edge_write_command command = {.value = {0x55U}, .value_size = 1U};
    command.command_id[0] = 3U;
    require_true(edge_device_runtime_enqueue_write(&runtime, &command),
                 "write command enqueue failed");
    edge_device_runtime_tick(&runtime, 1000U, 1001000);
    edge_device_runtime_tick(&runtime, 2000U, 1002000);
    require_true(fake.reads == 3U && fake.writes == 1U && fake.completions == 1U &&
                     fake.last_command_result == EDGE_COMMAND_SUCCEEDED,
                 "one-second read/write loop did not run or verify readback");
    require_true(fake.reports == 0U, "report interval incorrectly changed DTU cadence");

    edge_device_runtime_tick(&runtime, 3000U, 1003000);
    require_true(fake.reads == 4U && fake.reports == 1U &&
                     memcmp(fake.last_platform, platform_id, 16U) == 0,
                 "runtime did not report on the independent platform interval");

    fake.next_read_result = EDGE_IO_NO_RESPONSE;
    edge_device_runtime_tick(&runtime, 4000U, 1004000);
    require_true(fake.disconnects == 1U, "silent S7 device did not close TCP state");
    edge_device_runtime_tick(&runtime, 5000U, 1005000);
    require_true(fake.connects == 2U && fake.handshakes == 2U,
                 "S7 did not reconnect and repeat both handshakes on the next cycle");
    edge_device_runtime_close(&runtime);

    fake_device modbus = {0};
    edge_device_runtime modbus_runtime;
    require_true(edge_device_runtime_init(&modbus_runtime, EDGE_DEVICE_MODBUS,
                                           platform_id, device_id,
                                           EDGE_DTU_IO_PERIOD_MS, 3U, 0U,
                                           &driver, &modbus),
                 "Modbus runtime initialization failed");
    edge_device_runtime_tick(&modbus_runtime, 0U, 1000000);
    modbus.next_read_result = EDGE_IO_NO_RESPONSE;
    edge_device_runtime_tick(&modbus_runtime, 1000U, 1001000);
    require_true(modbus.disconnects == 0U && modbus.connects == 1U,
                 "Modbus timeout unexpectedly closed the TCP state");
    edge_device_runtime_tick(&modbus_runtime, 2000U, 1002000);
    require_true(modbus.connects == 1U && modbus.reads == 3U,
                 "Modbus did not reuse the existing connection after a timeout");
    modbus.next_read_result = EDGE_IO_OFFLINE;
    edge_device_runtime_tick(&modbus_runtime, 3000U, 1003000);
    require_true(modbus.disconnects == 1U,
                 "Modbus transport failure did not close the broken connection");
    edge_device_runtime_tick(&modbus_runtime, 4000U, 1004000);
    require_true(modbus.connects == 2U,
                 "Modbus transport failure did not reconnect before the next request");
    edge_device_runtime_close(&modbus_runtime);
    require_true(modbus.disconnects == 2U,
                 "Modbus runtime did not close during explicit shutdown");
}

int main(void) {
    test_modbus();
    test_s7();
    test_fixed_io_and_reporting();
    puts("edge runtime tests passed");
    return 0;
}
