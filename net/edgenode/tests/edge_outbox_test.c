#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "edge_memory.h"

static int failures;

static void require(bool condition, const char *message) {
    if (condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

static void make_id(uint8_t output[16], uint8_t suffix) {
    memset(output, 0, 16U);
    output[15] = suffix;
}

int main(void) {
    edge_memory_outbox outbox;
    edge_memory_outbox_init(&outbox, 1024U);

    const uint8_t payload[] = {1U, 2U, 3U};
    uint8_t telemetry_one[16];
    uint8_t telemetry_two[16];
    uint8_t command[16];
    make_id(telemetry_one, 1U);
    make_id(telemetry_two, 2U);
    make_id(command, 3U);

    require(edge_memory_outbox_put(&outbox, telemetry_one, payload, sizeof(payload)),
            "queue first telemetry");
    require(edge_memory_outbox_put(&outbox, telemetry_two, payload, sizeof(payload)),
            "queue second telemetry");
    require(edge_memory_outbox_put_priority(&outbox, command, payload,
                                            sizeof(payload), true),
            "queue priority command result");
    require(outbox.count == 3U && outbox.bytes == 9U,
            "track queued record and byte counts");

    const edge_memory_message *next = edge_memory_outbox_next(&outbox, 1000U);
    require(next != NULL && memcmp(next->message_id, command, 16U) == 0,
            "send command result before telemetry");
    next = edge_memory_outbox_next(&outbox, 2000U);
    require(next != NULL && memcmp(next->message_id, telemetry_one, 16U) == 0,
            "preserve telemetry order inside the delivery window");
    require(outbox.in_flight == 2U, "track concurrent deliveries");
    require(!edge_memory_outbox_timed_out(&outbox, 30999U, 30000U),
            "delivery timed out before its deadline");
    require(edge_memory_outbox_timed_out(&outbox, 31000U, 30000U),
            "oldest unacknowledged delivery did not time out");

    require(edge_memory_outbox_retry(&outbox, telemetry_one),
            "make a failed delivery retryable");
    require(outbox.in_flight == 1U, "release retry from the delivery window");
    next = edge_memory_outbox_next(&outbox, 32000U);
    require(next != NULL && memcmp(next->message_id, telemetry_one, 16U) == 0,
            "retry the same telemetry record");

    require(edge_memory_outbox_ack(&outbox, command), "ack priority result");
    require(outbox.count == 2U && outbox.in_flight == 1U,
            "ack removes an in-flight result");
    require(edge_memory_outbox_oldest_regular(&outbox) != NULL &&
                memcmp(edge_memory_outbox_oldest_regular(&outbox)->message_id,
                       telemetry_one, 16U) == 0,
            "find oldest evictable telemetry");

    edge_memory_outbox_reset(&outbox);
    require(outbox.in_flight == 0U, "reconnect resets all in-flight records");
    require(!edge_memory_outbox_timed_out(&outbox, UINT64_MAX, 1U),
            "reset deliveries retained stale ACK deadlines");
    edge_memory_outbox_free(&outbox);
    require(outbox.count == 0U && outbox.maximum_bytes == 1024U,
            "free preserves configured capacity");
    return failures == 0 ? 0 : 1;
}
