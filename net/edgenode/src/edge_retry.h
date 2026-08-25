#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EDGE_RETRY_READY,
    EDGE_RETRY_WAITING,
    EDGE_RETRY_CONNECTING,
    EDGE_RETRY_AWAITING_APPLICATION,
    EDGE_RETRY_CONNECTED,
} edge_retry_phase;

typedef struct {
    edge_retry_phase phase;
    uint64_t deadline_ms;
    uint32_t retry_delay_ms;
    uint32_t connect_timeout_ms;
} edge_retry;

bool edge_retry_init(edge_retry *retry, uint32_t retry_delay_ms,
                     uint32_t connect_timeout_ms);
bool edge_retry_should_start(const edge_retry *retry, uint64_t now_ms);
void edge_retry_attempt_started(edge_retry *retry, uint64_t now_ms);
void edge_retry_transport_connected(edge_retry *retry, uint64_t now_ms,
                                    uint32_t application_timeout_ms);
void edge_retry_application_alive(edge_retry *retry, uint64_t now_ms,
                                  uint32_t application_timeout_ms);
void edge_retry_application_ready(edge_retry *retry, uint64_t now_ms,
                                  uint32_t application_timeout_ms);
void edge_retry_failed(edge_retry *retry, uint64_t now_ms);
bool edge_retry_attempt_timed_out(const edge_retry *retry, uint64_t now_ms);
bool edge_retry_application_timed_out(const edge_retry *retry, uint64_t now_ms);
uint32_t edge_retry_delay_ms(const edge_retry *retry, uint64_t now_ms);
