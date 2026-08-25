#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_MAX_CONFIG_ITEMS 512U

typedef struct {
    uint8_t digest[32];
    uint8_t *payload;
    size_t payload_size;
    bool present;
} edge_memory_config_item;

typedef struct {
    uint64_t revision;
    uint8_t digest[32];
    uint32_t item_count;
    uint32_t received_count;
    edge_memory_config_item *items;
} edge_memory_config_set;

typedef struct edge_memory_message {
    struct edge_memory_message *next;
    uint8_t message_id[16];
    size_t payload_size;
    uint64_t sent_at_ms;
    bool priority;
    bool in_flight;
    uint8_t payload[];
} edge_memory_message;

typedef struct {
    size_t maximum_bytes;
    size_t bytes;
    size_t count;
    size_t in_flight;
    edge_memory_message *head;
    edge_memory_message *tail;
} edge_memory_outbox;

void edge_memory_config_free(edge_memory_config_set *set);
bool edge_memory_config_begin(edge_memory_config_set *staging, uint64_t revision,
                              uint32_t item_count, const uint8_t digest[32]);
bool edge_memory_config_put(edge_memory_config_set *staging, uint64_t revision,
                            uint32_t index, const uint8_t digest[32],
                            const uint8_t *payload, size_t payload_size);
bool edge_memory_config_commit(edge_memory_config_set *active,
                               edge_memory_config_set *staging,
                               uint64_t revision, const uint8_t digest[32]);

void edge_memory_outbox_init(edge_memory_outbox *outbox, size_t maximum_bytes);
void edge_memory_outbox_free(edge_memory_outbox *outbox);
bool edge_memory_outbox_put(edge_memory_outbox *outbox, const uint8_t message_id[16],
                            const uint8_t *payload, size_t payload_size);
bool edge_memory_outbox_put_priority(edge_memory_outbox *outbox,
                                     const uint8_t message_id[16],
                                     const uint8_t *payload, size_t payload_size,
                                     bool priority);
const edge_memory_message *edge_memory_outbox_first(const edge_memory_outbox *outbox);
const edge_memory_message *edge_memory_outbox_oldest_regular(
    const edge_memory_outbox *outbox);
const edge_memory_message *edge_memory_outbox_next(edge_memory_outbox *outbox,
                                                   uint64_t now_ms);
bool edge_memory_outbox_timed_out(const edge_memory_outbox *outbox,
                                  uint64_t now_ms, uint32_t timeout_ms);
bool edge_memory_outbox_retry(edge_memory_outbox *outbox,
                              const uint8_t message_id[16]);
void edge_memory_outbox_reset(edge_memory_outbox *outbox);
bool edge_memory_outbox_ack(edge_memory_outbox *outbox, const uint8_t message_id[16]);
