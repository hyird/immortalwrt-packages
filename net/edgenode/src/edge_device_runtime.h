#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_DTU_IO_PERIOD_MS 1000U
#define EDGE_DEVICE_VALUE_MAX 512U
#define EDGE_DEVICE_WRITE_QUEUE 4U

typedef enum {
    EDGE_DEVICE_MODBUS = 1,
    EDGE_DEVICE_S7 = 2,
} edge_device_protocol;

typedef enum {
    EDGE_IO_OK = 0,
    EDGE_IO_NO_RESPONSE,
    EDGE_IO_OFFLINE,
    EDGE_IO_PROTOCOL_ERROR,
} edge_io_result;

typedef enum {
    EDGE_COMMAND_SUCCEEDED = 0,
    EDGE_COMMAND_READBACK_MISMATCH,
    EDGE_COMMAND_DEVICE_OFFLINE,
    EDGE_COMMAND_TIMED_OUT,
    EDGE_COMMAND_FAILED,
} edge_command_result;

typedef struct {
    uint8_t command_id[16];
    char element_id[65];
    uint8_t value[EDGE_DEVICE_VALUE_MAX];
    size_t value_size;
} edge_write_command;

typedef struct {
    uint8_t bytes[EDGE_DEVICE_VALUE_MAX];
    size_t size;
    int64_t sampled_at_ms;
} edge_device_sample;

typedef struct {
    edge_io_result (*connect)(void *context);
    edge_io_result (*handshake)(void *context);
    edge_io_result (*read)(void *context, edge_device_sample *sample);
    edge_io_result (*write_readback)(void *context, const edge_write_command *command,
                                     edge_device_sample *actual);
    void (*disconnect)(void *context);
    void (*report)(void *context, const uint8_t platform_id[16],
                   const uint8_t device_id[16], const edge_device_sample *sample);
    void (*command_complete)(void *context, const uint8_t platform_id[16],
                             const uint8_t device_id[16], const uint8_t command_id[16],
                             edge_command_result result,
                             const edge_device_sample *actual);
} edge_device_driver;

typedef struct {
    edge_device_protocol protocol;
    uint8_t platform_id[16];
    uint8_t device_id[16];
    uint32_t report_interval_sec;
    uint64_t next_io_at_ms;
    uint64_t next_report_at_ms;
    bool connected;
    bool handshaken;
    bool has_sample;
    edge_device_sample latest;
    edge_write_command writes[EDGE_DEVICE_WRITE_QUEUE];
    uint8_t write_head;
    uint8_t write_count;
    edge_device_driver driver;
    void *driver_context;
} edge_device_runtime;

bool edge_device_runtime_init(edge_device_runtime *runtime,
                              edge_device_protocol protocol,
                              const uint8_t platform_id[16],
                              const uint8_t device_id[16],
                              uint32_t io_interval_ms,
                              uint32_t report_interval_sec,
                              uint64_t now_ms,
                              const edge_device_driver *driver,
                              void *driver_context);

bool edge_device_runtime_enqueue_write(edge_device_runtime *runtime,
                                       const edge_write_command *command);

/* Call at least once per second. Slow calls never trigger a catch-up burst. */
void edge_device_runtime_tick(edge_device_runtime *runtime, uint64_t schedule_ms,
                              int64_t observed_at_ms);

void edge_device_runtime_close(edge_device_runtime *runtime);
