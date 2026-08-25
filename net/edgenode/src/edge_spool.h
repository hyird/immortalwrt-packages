#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge_memory.h"

#define EDGE_SPOOL_ROOT "/tmp/edgenode"

typedef struct {
    char directory[160];
    uint8_t platform_id[16];
    edge_memory_config_set staging_config;
    edge_memory_config_set active_config;
    edge_memory_outbox outbox;
} edge_spool;

/* Loads active config and unacknowledged Envelope files left by a process restart. */
bool edge_spool_init(edge_spool *spool, const uint8_t platform_id[16],
                     size_t outbox_maximum_bytes);
void edge_spool_free(edge_spool *spool);

bool edge_spool_config_begin(edge_spool *spool, uint64_t revision,
                             uint32_t item_count, const uint8_t digest[32]);
bool edge_spool_config_put(edge_spool *spool, uint64_t revision, uint32_t index,
                           const uint8_t digest[32], const uint8_t *payload,
                           size_t payload_size);
bool edge_spool_config_commit(edge_spool *spool, uint64_t revision,
                              const uint8_t digest[32]);

bool edge_spool_outbox_put(edge_spool *spool, const uint8_t message_id[16],
                           const uint8_t *envelope, size_t envelope_size);
bool edge_spool_outbox_put_priority(edge_spool *spool,
                                    const uint8_t message_id[16],
                                    const uint8_t *envelope, size_t envelope_size,
                                    bool priority);
const edge_memory_message *edge_spool_outbox_first(edge_spool *spool);
const edge_memory_message *edge_spool_outbox_next(edge_spool *spool,
                                                  uint64_t now_ms);
bool edge_spool_outbox_timed_out(const edge_spool *spool,
                                 uint64_t now_ms, uint32_t timeout_ms);
bool edge_spool_outbox_retry(edge_spool *spool, const uint8_t message_id[16]);
void edge_spool_outbox_reset(edge_spool *spool);
bool edge_spool_outbox_ack(edge_spool *spool, const uint8_t message_id[16]);
/* Enforces the global 15% tmpfs reserve and reconciles files removed by rolling cleanup. */
bool edge_spool_maintain(edge_spool *spool);
