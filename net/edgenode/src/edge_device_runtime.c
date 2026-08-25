#include "edge_device_runtime.h"

#include <string.h>

static uint64_t advance_deadline(uint64_t current, uint64_t period, uint64_t now) {
    if (current > now)
        return current;
    const uint64_t elapsed = now - current;
    const uint64_t steps = elapsed / period + 1U;
    if (steps > (UINT64_MAX - current) / period)
        return now + period;
    return current + steps * period;
}

static void close_connection(edge_device_runtime *runtime) {
    if ((runtime->connected || runtime->handshaken) && runtime->driver.disconnect != NULL)
        runtime->driver.disconnect(runtime->driver_context);
    runtime->connected = false;
    runtime->handshaken = false;
}

bool edge_device_runtime_init(edge_device_runtime *runtime,
                              edge_device_protocol protocol,
                              const uint8_t platform_id[16],
                              const uint8_t device_id[16],
                              uint32_t io_interval_ms,
                              uint32_t report_interval_sec,
                              uint64_t now_ms,
                              const edge_device_driver *driver,
                              void *driver_context) {
    if (runtime == NULL || platform_id == NULL || device_id == NULL || driver == NULL ||
        driver->connect == NULL || driver->read == NULL || driver->report == NULL ||
        driver->command_complete == NULL || report_interval_sec == 0U ||
        (protocol != EDGE_DEVICE_MODBUS && protocol != EDGE_DEVICE_S7) ||
        (io_interval_ms != 0U && io_interval_ms != EDGE_DTU_IO_PERIOD_MS) ||
        (protocol == EDGE_DEVICE_S7 && driver->handshake == NULL))
        return false;

    memset(runtime, 0, sizeof(*runtime));
    runtime->protocol = protocol;
    memcpy(runtime->platform_id, platform_id, 16U);
    memcpy(runtime->device_id, device_id, 16U);
    runtime->report_interval_sec = report_interval_sec;
    runtime->next_io_at_ms = now_ms;
    runtime->next_report_at_ms = now_ms + (uint64_t)report_interval_sec * 1000U;
    runtime->driver = *driver;
    runtime->driver_context = driver_context;
    return true;
}

bool edge_device_runtime_enqueue_write(edge_device_runtime *runtime,
                                       const edge_write_command *command) {
    if (runtime == NULL || command == NULL || command->value_size == 0U ||
        command->value_size > EDGE_DEVICE_VALUE_MAX ||
        runtime->write_count >= EDGE_DEVICE_WRITE_QUEUE)
        return false;
    const uint8_t tail = (uint8_t)((runtime->write_head + runtime->write_count) %
                                   EDGE_DEVICE_WRITE_QUEUE);
    runtime->writes[tail] = *command;
    ++runtime->write_count;
    return true;
}

static edge_io_result ensure_ready(edge_device_runtime *runtime) {
    if (!runtime->connected) {
        const edge_io_result connected = runtime->driver.connect(runtime->driver_context);
        if (connected != EDGE_IO_OK)
            return connected;
        runtime->connected = true;
    }
    if (runtime->protocol == EDGE_DEVICE_S7 && !runtime->handshaken) {
        const edge_io_result handshaken = runtime->driver.handshake(runtime->driver_context);
        if (handshaken != EDGE_IO_OK)
            return handshaken;
        runtime->handshaken = true;
    }
    return EDGE_IO_OK;
}

static void complete_write(edge_device_runtime *runtime, edge_command_result result,
                           const edge_device_sample *actual) {
    const edge_write_command *command = &runtime->writes[runtime->write_head];
    runtime->driver.command_complete(runtime->driver_context, runtime->platform_id,
                                     runtime->device_id, command->command_id, result, actual);
    runtime->write_head = (uint8_t)((runtime->write_head + 1U) % EDGE_DEVICE_WRITE_QUEUE);
    --runtime->write_count;
}

static bool same_value(const edge_write_command *command, const edge_device_sample *actual) {
    return command->value_size == actual->size &&
           memcmp(command->value, actual->bytes, actual->size) == 0;
}

static void handle_no_response(edge_device_runtime *runtime) {
    /*
     * S7 must not reuse a timed-out COTP/S7 session; reset both TCP and
     * handshake state so the next tick reconnects and negotiates from the
     * beginning. Modbus connections are deliberately kept open across a
     * timeout: some gateways send a banner or delayed response on the same
     * TCP stream and must not be forced through a reconnect loop.
     */
    if (runtime->protocol == EDGE_DEVICE_S7)
        close_connection(runtime);
}

static void handle_offline(edge_device_runtime *runtime) {
    /* A transport failure is different from a protocol timeout: the next
     * cycle must establish a new connection before sending another request. */
    close_connection(runtime);
}

void edge_device_runtime_tick(edge_device_runtime *runtime, uint64_t schedule_ms,
                              int64_t observed_at_ms) {
    if (runtime == NULL)
        return;

    if (schedule_ms >= runtime->next_io_at_ms) {
        runtime->next_io_at_ms = advance_deadline(runtime->next_io_at_ms,
                                                  EDGE_DTU_IO_PERIOD_MS, schedule_ms);
        edge_io_result result = ensure_ready(runtime);
        if (result == EDGE_IO_OK && runtime->write_count != 0U) {
            edge_device_sample actual = {0};
            const edge_write_command *command = &runtime->writes[runtime->write_head];
            if (runtime->driver.write_readback == NULL) {
                complete_write(runtime, EDGE_COMMAND_FAILED, NULL);
            } else {
                result = runtime->driver.write_readback(runtime->driver_context, command, &actual);
                if (result == EDGE_IO_OK) {
                    complete_write(runtime,
                                   same_value(command, &actual)
                                       ? EDGE_COMMAND_SUCCEEDED
                                       : EDGE_COMMAND_READBACK_MISMATCH,
                                   &actual);
                } else if (result == EDGE_IO_NO_RESPONSE) {
                    complete_write(runtime, EDGE_COMMAND_TIMED_OUT, NULL);
                    handle_no_response(runtime);
                } else if (result == EDGE_IO_OFFLINE) {
                    complete_write(runtime, EDGE_COMMAND_DEVICE_OFFLINE, NULL);
                    handle_offline(runtime);
                } else {
                    complete_write(runtime, EDGE_COMMAND_FAILED, NULL);
                }
            }
        }

        if (result == EDGE_IO_OK) {
            edge_device_sample sample = {0};
            result = runtime->driver.read(runtime->driver_context, &sample);
            if (result == EDGE_IO_OK && sample.size <= EDGE_DEVICE_VALUE_MAX) {
                sample.sampled_at_ms = observed_at_ms;
                runtime->latest = sample;
                runtime->has_sample = true;
            } else if (result == EDGE_IO_NO_RESPONSE) {
                handle_no_response(runtime);
            } else if (result == EDGE_IO_OFFLINE) {
                handle_offline(runtime);
            }
        } else if (result == EDGE_IO_NO_RESPONSE) {
            handle_no_response(runtime);
        } else if (result == EDGE_IO_OFFLINE) {
            handle_offline(runtime);
        }
    }

    const uint64_t report_period = (uint64_t)runtime->report_interval_sec * 1000U;
    if (schedule_ms >= runtime->next_report_at_ms) {
        runtime->next_report_at_ms = advance_deadline(runtime->next_report_at_ms,
                                                      report_period, schedule_ms);
        if (runtime->has_sample)
            runtime->driver.report(runtime->driver_context, runtime->platform_id,
                                   runtime->device_id, &runtime->latest);
    }
}

void edge_device_runtime_close(edge_device_runtime *runtime) {
    if (runtime == NULL)
        return;
    close_connection(runtime);
}
