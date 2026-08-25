#include "edge_retry.h"

#include <limits.h>
#include <stddef.h>

static uint64_t add_delay(uint64_t now_ms, uint32_t delay_ms) {
    if (UINT64_MAX - now_ms < delay_ms)
        return UINT64_MAX;
    return now_ms + delay_ms;
}

bool edge_retry_init(edge_retry *retry, uint32_t retry_delay_ms,
                     uint32_t connect_timeout_ms) {
    if (retry == NULL || retry_delay_ms == 0U || connect_timeout_ms == 0U)
        return false;
    *retry = (edge_retry){
        .phase = EDGE_RETRY_READY,
        .retry_delay_ms = retry_delay_ms,
        .connect_timeout_ms = connect_timeout_ms,
    };
    return true;
}

bool edge_retry_should_start(const edge_retry *retry, uint64_t now_ms) {
    return retry != NULL &&
           (retry->phase == EDGE_RETRY_READY ||
            (retry->phase == EDGE_RETRY_WAITING && now_ms >= retry->deadline_ms));
}

void edge_retry_attempt_started(edge_retry *retry, uint64_t now_ms) {
    if (retry == NULL)
        return;
    retry->phase = EDGE_RETRY_CONNECTING;
    retry->deadline_ms = add_delay(now_ms, retry->connect_timeout_ms);
}

void edge_retry_transport_connected(edge_retry *retry, uint64_t now_ms,
                                    uint32_t application_timeout_ms) {
    if (retry == NULL || application_timeout_ms == 0U)
        return;
    retry->phase = EDGE_RETRY_AWAITING_APPLICATION;
    retry->deadline_ms = add_delay(now_ms, application_timeout_ms);
}

void edge_retry_application_alive(edge_retry *retry, uint64_t now_ms,
                                  uint32_t application_timeout_ms) {
    if (retry == NULL || application_timeout_ms == 0U ||
        retry->phase != EDGE_RETRY_CONNECTED)
        return;
    retry->deadline_ms = add_delay(now_ms, application_timeout_ms);
}

void edge_retry_application_ready(edge_retry *retry, uint64_t now_ms,
                                  uint32_t application_timeout_ms) {
    if (retry == NULL || application_timeout_ms == 0U)
        return;
    retry->phase = EDGE_RETRY_CONNECTED;
    retry->deadline_ms = add_delay(now_ms, application_timeout_ms);
}

void edge_retry_failed(edge_retry *retry, uint64_t now_ms) {
    if (retry == NULL)
        return;
    retry->phase = EDGE_RETRY_WAITING;
    retry->deadline_ms = add_delay(now_ms, retry->retry_delay_ms);
}

bool edge_retry_attempt_timed_out(const edge_retry *retry, uint64_t now_ms) {
    return retry != NULL && retry->phase == EDGE_RETRY_CONNECTING &&
           now_ms >= retry->deadline_ms;
}

bool edge_retry_application_timed_out(const edge_retry *retry, uint64_t now_ms) {
    return retry != NULL &&
           (retry->phase == EDGE_RETRY_AWAITING_APPLICATION ||
            retry->phase == EDGE_RETRY_CONNECTED) &&
           now_ms >= retry->deadline_ms;
}

uint32_t edge_retry_delay_ms(const edge_retry *retry, uint64_t now_ms) {
    if (retry == NULL || retry->phase == EDGE_RETRY_READY)
        return 0U;
    if (retry->phase == EDGE_RETRY_AWAITING_APPLICATION ||
        retry->phase == EDGE_RETRY_CONNECTED)
        return UINT32_MAX;
    if (now_ms >= retry->deadline_ms)
        return 0U;
    const uint64_t remaining = retry->deadline_ms - now_ms;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}
