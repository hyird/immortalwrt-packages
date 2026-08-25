#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#include "edge_device_runtime.h"
#include "edge_memory.h"
#include "edge_modbus.h"
#include "edge_protocol.h"
#include "edge_retry.h"
#include "edge_s7.h"

typedef struct {
    uint64_t connects;
    uint64_t reads;
    uint64_t writes;
    uint64_t reports;
    uint64_t completions;
    uint64_t disconnects;
} fake_driver_state;

typedef struct {
    uint64_t private_bytes;
    uint64_t working_set;
    uint64_t peak_working_set;
    uint32_t handles;
} process_sample;

static uint64_t random_state = UINT64_C(0x6a09e667f3bcc909);

static void fail(const char *message) {
    fprintf(stderr, "STABILITY FAIL: %s\n", message);
    exit(EXIT_FAILURE);
}

static void require_true(bool condition, const char *message) {
    if (!condition)
        fail(message);
}

static uint64_t next_random(void) {
    uint64_t value = random_state;
    value ^= value << 13U;
    value ^= value >> 7U;
    value ^= value << 17U;
    random_state = value;
    return value;
}

static void make_id(uint64_t cycle, uint32_t index, uint8_t output[16]) {
    for (size_t offset = 0U; offset < 8U; ++offset) {
        output[offset] = (uint8_t)(cycle >> (offset * 8U));
        output[8U + offset] = (uint8_t)(((uint64_t)index << 32U | cycle) >>
                                        (offset * 8U));
    }
}

static edge_io_result fake_connect(void *context) {
    fake_driver_state *state = context;
    ++state->connects;
    return EDGE_IO_OK;
}

static edge_io_result fake_handshake(void *context) {
    (void)context;
    return EDGE_IO_OK;
}

static edge_io_result fake_read(void *context, edge_device_sample *sample) {
    fake_driver_state *state = context;
    ++state->reads;
    sample->size = 8U;
    memcpy(sample->bytes, &state->reads, sample->size);
    return EDGE_IO_OK;
}

static edge_io_result fake_write_readback(void *context,
                                          const edge_write_command *command,
                                          edge_device_sample *actual) {
    fake_driver_state *state = context;
    ++state->writes;
    actual->size = command->value_size;
    memcpy(actual->bytes, command->value, command->value_size);
    return EDGE_IO_OK;
}

static void fake_disconnect(void *context) {
    fake_driver_state *state = context;
    ++state->disconnects;
}

static void fake_report(void *context, const uint8_t platform_id[16],
                        const uint8_t device_id[16],
                        const edge_device_sample *sample) {
    fake_driver_state *state = context;
    require_true(platform_id[0] == 1U && device_id[0] == 2U && sample->size == 8U,
                 "device report content changed");
    ++state->reports;
}

static void fake_command_complete(void *context, const uint8_t platform_id[16],
                                  const uint8_t device_id[16],
                                  const uint8_t command_id[16],
                                  edge_command_result result,
                                  const edge_device_sample *actual) {
    fake_driver_state *state = context;
    require_true(platform_id[0] == 1U && device_id[0] == 2U &&
                     command_id[0] == 3U && result == EDGE_COMMAND_SUCCEEDED &&
                     actual != NULL && actual->size == 2U,
                 "write acknowledgement content changed");
    ++state->completions;
}

static void stress_protocol(uint64_t cycle) {
    uint8_t platform_id[16] = {0};
    uint8_t random_bytes[10];
    for (size_t index = 0U; index < sizeof(random_bytes); ++index)
        random_bytes[index] = (uint8_t)next_random();
    platform_id[0] = 1U;

    iot_edge_v1_Envelope envelope;
    require_true(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U,
                                             cycle + 1U,
                                             (int64_t)(1784688000000ULL + cycle),
                                             random_bytes),
                 "protocol envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_heartbeat_tag;
    envelope.payload.heartbeat.uptime_sec = cycle;
    strcpy(envelope.payload.heartbeat.iccid, "89860012345678901234");
    envelope.payload.heartbeat.signal_csq = (uint32_t)(cycle % 32U);
    envelope.payload.heartbeat.signal_rssi_dbm = -113 +
        (int32_t)(envelope.payload.heartbeat.signal_csq * 2U);
    envelope.payload.heartbeat.signal_percent =
        envelope.payload.heartbeat.signal_csq * 100U / 31U;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require_true(edge_protocol_encode(&envelope, encoded, sizeof(encoded),
                                      &encoded_size, &error),
                 error != NULL ? error : "protocol encode failed");
    iot_edge_v1_Envelope decoded;
    require_true(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
                 error != NULL ? error : "protocol decode failed");
    require_true(decoded.sequence == cycle + 1U &&
                     decoded.which_payload == iot_edge_v1_Envelope_heartbeat_tag &&
                     decoded.payload.heartbeat.uptime_sec == cycle &&
                     strcmp(decoded.payload.heartbeat.iccid,
                            "89860012345678901234") == 0,
                 "protobuf round trip changed an envelope");
}

static void stress_outbox(uint64_t cycle) {
    edge_memory_outbox outbox;
    edge_memory_outbox_init(&outbox, 64U * 1024U);
    uint8_t ids[64][16];
    uint8_t payload[256];
    for (size_t index = 0U; index < sizeof(payload); ++index)
        payload[index] = (uint8_t)next_random();

    for (uint32_t index = 0U; index < 64U; ++index) {
        make_id(cycle, index, ids[index]);
        const size_t payload_size = 32U + (size_t)(next_random() % 225U);
        require_true(edge_memory_outbox_put_priority(
                         &outbox, ids[index], payload, payload_size,
                         index % 11U == 0U),
                     "outbox rejected a valid message");
        if (index % 13U == 0U) {
            const size_t count = outbox.count;
            require_true(edge_memory_outbox_put_priority(
                             &outbox, ids[index], payload, payload_size, false) &&
                             outbox.count == count,
                         "outbox duplicate changed the queue");
        }
    }
    require_true(outbox.count == 64U, "outbox count drifted during enqueue");

    uint64_t now_ms = cycle * 1000U;
    size_t delivered = 0U;
    while (outbox.count != 0U) {
        const edge_memory_message *message =
            edge_memory_outbox_next(&outbox, now_ms);
        require_true(message != NULL, "outbox delivery window stalled");
        uint8_t message_id[16];
        memcpy(message_id, message->message_id, sizeof(message_id));
        if ((delivered + cycle) % 17U == 0U) {
            require_true(edge_memory_outbox_retry(&outbox, message_id),
                         "outbox retry lost a message");
            message = edge_memory_outbox_next(&outbox, now_ms + 1U);
            require_true(message != NULL &&
                             memcmp(message->message_id, message_id, 16U) == 0,
                         "outbox retry changed delivery ordering");
        }
        require_true(edge_memory_outbox_ack(&outbox, message_id),
                     "outbox acknowledgement lost a message");
        ++delivered;
        ++now_ms;
    }
    require_true(delivered == 64U && outbox.bytes == 0U &&
                     outbox.in_flight == 0U && outbox.head == NULL &&
                     outbox.tail == NULL,
                 "outbox retained state after all acknowledgements");
    edge_memory_outbox_free(&outbox);
}

static void stress_retry(uint64_t cycle) {
    edge_retry retry;
    const uint64_t started = cycle * 100000U;
    require_true(edge_retry_init(&retry, 5000U, 30000U),
                 "retry initialization failed");
    edge_retry_attempt_started(&retry, started);
    require_true(!edge_retry_attempt_timed_out(&retry, started + 29999U) &&
                     edge_retry_attempt_timed_out(&retry, started + 30000U),
                 "transport timeout boundary changed");
    edge_retry_transport_connected(&retry, started + 10U, 30000U);
    edge_retry_application_alive(&retry, started + 100U, 30000U);
    require_true(edge_retry_application_timed_out(&retry, started + 30010U),
                 "pre-Hello traffic bypassed the handshake watchdog");
    edge_retry_application_ready(&retry, started + 30011U, 60000U);
    edge_retry_application_alive(&retry, started + 40000U, 60000U);
    require_true(!edge_retry_application_timed_out(&retry, started + 99999U) &&
                     edge_retry_application_timed_out(&retry, started + 100000U),
                 "application liveness deadline changed");
    edge_retry_failed(&retry, started + 100000U);
    require_true(!edge_retry_should_start(&retry, started + 104999U) &&
                     edge_retry_should_start(&retry, started + 105000U),
                 "reconnect scheduling boundary changed");
}

static void stress_wire_protocols(uint64_t cycle) {
    edge_modbus_request request = {
        .transport = EDGE_MODBUS_TCP,
        .transaction_id = (uint16_t)cycle,
        .unit_id = (uint8_t)(cycle % 247U + 1U),
        .function = 3U,
        .address = (uint16_t)(cycle % 60000U),
        .quantity = 2U,
    };
    uint8_t frame[EDGE_MODBUS_MAX_FRAME];
    size_t frame_size = 0U;
    require_true(edge_modbus_build_read(&request, frame, sizeof(frame),
                                        &frame_size) == EDGE_MODBUS_OK &&
                     frame_size == 12U,
                 "Modbus request build failed");
    uint8_t response[] = {
        frame[0], frame[1], 0U, 0U, 0U, 7U, request.unit_id, 3U, 4U,
        (uint8_t)cycle, (uint8_t)(cycle >> 8U),
        (uint8_t)(cycle >> 16U), (uint8_t)(cycle >> 24U),
    };
    uint8_t value[4];
    size_t value_size = 0U;
    uint8_t exception = 0U;
    require_true(edge_modbus_parse_response(
                     &request, response, sizeof(response), NULL, 0U, value,
                     sizeof(value), &value_size, &exception) == EDGE_MODBUS_OK &&
                     value_size == sizeof(value),
                 "Modbus response validation failed");
    response[1] ^= 1U;
    require_true(edge_modbus_parse_response(
                     &request, response, sizeof(response), NULL, 0U, value,
                     sizeof(value), &value_size, &exception) ==
                     EDGE_MODBUS_WRONG_RESPONSE,
                 "Modbus accepted a mismatched transaction");

    edge_s7_address address = {
        .area = EDGE_S7_AREA_DB,
        .db_number = (uint16_t)(cycle % 32U + 1U),
        .start_byte = (uint32_t)(cycle % 4096U),
        .start_bit = 0U,
        .size = 4U,
        .bit_access = false,
    };
    frame_size = edge_s7_build_read((uint16_t)cycle, &address, frame,
                                    sizeof(frame));
    require_true(frame_size > 0U,
                 "S7 request build failed");
    size_t expected_frame = 0U;
    require_true(edge_s7_frame_length(frame, frame_size, &expected_frame) &&
                     expected_frame == frame_size,
                 "S7 TPKT framing failed");
}

static void stress_device_runtime(uint64_t cycle) {
    const uint8_t platform_id[16] = {1U};
    const uint8_t device_id[16] = {2U};
    fake_driver_state state = {0};
    const edge_device_driver driver = {
        .connect = fake_connect,
        .handshake = fake_handshake,
        .read = fake_read,
        .write_readback = fake_write_readback,
        .disconnect = fake_disconnect,
        .report = fake_report,
        .command_complete = fake_command_complete,
    };
    edge_device_runtime runtime;
    require_true(edge_device_runtime_init(&runtime, EDGE_DEVICE_MODBUS,
                                          platform_id, device_id, 1000U, 2U,
                                          cycle * 4000U, &driver, &state),
                 "device runtime initialization failed");
    edge_write_command command = {0};
    command.command_id[0] = 3U;
    strcpy(command.element_id, "remote-io-ao1");
    command.value[0] = 0x12U;
    command.value[1] = 0x34U;
    command.value_size = 2U;
    require_true(edge_device_runtime_enqueue_write(&runtime, &command),
                 "device write enqueue failed");
    for (uint64_t tick = 0U; tick < 4U; ++tick)
        edge_device_runtime_tick(&runtime, cycle * 4000U + tick * 1000U,
                                 (int64_t)(cycle * 4000U + tick * 1000U));
    require_true(state.connects == 1U && state.reads == 4U &&
                     state.writes == 1U && state.completions == 1U &&
                     state.reports == 1U,
                 "device lifecycle counters changed");
    edge_device_runtime_close(&runtime);
    require_true(state.disconnects == 1U, "device runtime did not disconnect");
}

static uint64_t run_stress(uint64_t iterations) {
    uint64_t operations = 0U;
    for (uint64_t cycle = 0U; cycle < iterations; ++cycle) {
        stress_retry(cycle);
        stress_wire_protocols(cycle);
        operations += 12U;
        if (cycle % 4U == 0U) {
            stress_protocol(cycle);
            operations += 2U;
        }
        if (cycle % 8U == 0U) {
            stress_outbox(cycle);
            operations += 64U;
        }
        if (cycle % 16U == 0U) {
            stress_device_runtime(cycle);
            operations += 12U;
        }
    }
    return operations;
}

static process_sample sample_process(void) {
    process_sample sample = {0};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&counters,
                              sizeof(counters)))
        fail("GetProcessMemoryInfo failed");
    sample.private_bytes = (uint64_t)counters.PrivateUsage;
    sample.working_set = (uint64_t)counters.WorkingSetSize;
    sample.peak_working_set = (uint64_t)counters.PeakWorkingSetSize;
    DWORD handles = 0U;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles))
        fail("GetProcessHandleCount failed");
    sample.handles = handles;
#endif
    return sample;
}

static double monotonic_seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency))
        fail("QueryPerformanceCounter failed");
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        fail("clock_gettime failed");
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
#endif
}

static uint64_t parse_iterations(int argc, char **argv) {
    uint64_t iterations = 100000U;
    if (argc == 1)
        return iterations;
    if (argc != 3 || strcmp(argv[1], "--iterations") != 0)
        fail("usage: edgenode-stability-x64 [--iterations COUNT]");
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || parsed == 0U)
        fail("invalid iteration count");
    return (uint64_t)parsed;
}

int main(int argc, char **argv) {
    const uint64_t iterations = parse_iterations(argc, argv);
    const uint64_t warmup = iterations < 1000U ? iterations : 1000U;
    (void)run_stress(warmup);
#ifdef _WIN32
    (void)EmptyWorkingSet(GetCurrentProcess());
#endif
    const process_sample before = sample_process();
    const double started = monotonic_seconds();
    const uint64_t operations = run_stress(iterations);
    const double elapsed = monotonic_seconds() - started;
#ifdef _WIN32
    (void)EmptyWorkingSet(GetCurrentProcess());
#endif
    const process_sample after = sample_process();

    require_true(after.handles == before.handles,
                 "process handle count grew during the stress run");
    require_true(after.private_bytes <= before.private_bytes + 4U * 1024U * 1024U,
                 "private memory grew by more than 4 MiB after warmup");

    printf("STABILITY PASS\n");
    printf("architecture=x64 iterations=%" PRIu64 " operations=%" PRIu64 "\n",
           iterations, operations);
    printf("elapsed_seconds=%.3f operations_per_second=%.0f\n", elapsed,
           elapsed > 0.0 ? (double)operations / elapsed : 0.0);
    printf("private_bytes_before=%" PRIu64 " private_bytes_after=%" PRIu64
           " delta=%" PRId64 "\n",
           before.private_bytes, after.private_bytes,
           (int64_t)(after.private_bytes - before.private_bytes));
    printf("working_set_after=%" PRIu64 " peak_working_set=%" PRIu64
           " handles_before=%" PRIu32 " handles_after=%" PRIu32 "\n",
           after.working_set, after.peak_working_set,
           before.handles, after.handles);
    return EXIT_SUCCESS;
}
