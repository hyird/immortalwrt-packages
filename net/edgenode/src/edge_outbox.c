#include "edge_memory.h"

#include <stdlib.h>
#include <string.h>

void edge_memory_outbox_init(edge_memory_outbox *outbox, size_t maximum_bytes) {
    if (outbox == NULL)
        return;
    memset(outbox, 0, sizeof(*outbox));
    outbox->maximum_bytes = maximum_bytes;
}

void edge_memory_outbox_free(edge_memory_outbox *outbox) {
    if (outbox == NULL)
        return;
    edge_memory_message *current = outbox->head;
    while (current != NULL) {
        edge_memory_message *next = current->next;
        free(current);
        current = next;
    }
    const size_t maximum = outbox->maximum_bytes;
    memset(outbox, 0, sizeof(*outbox));
    outbox->maximum_bytes = maximum;
}

bool edge_memory_outbox_put(edge_memory_outbox *outbox, const uint8_t message_id[16],
                            const uint8_t *payload, size_t payload_size) {
    return edge_memory_outbox_put_priority(outbox, message_id, payload, payload_size,
                                           false);
}

bool edge_memory_outbox_put_priority(edge_memory_outbox *outbox,
                                     const uint8_t message_id[16],
                                     const uint8_t *payload, size_t payload_size,
                                     bool priority) {
    if (outbox == NULL || message_id == NULL || payload == NULL || payload_size == 0U ||
        payload_size > outbox->maximum_bytes ||
        outbox->bytes > outbox->maximum_bytes - payload_size)
        return false;
    for (edge_memory_message *existing = outbox->head; existing != NULL;
         existing = existing->next)
        if (memcmp(existing->message_id, message_id, 16U) == 0)
            return true;

    edge_memory_message *item = malloc(sizeof(*item) + payload_size);
    if (item == NULL)
        return false;
    item->next = NULL;
    memcpy(item->message_id, message_id, 16U);
    item->payload_size = payload_size;
    item->sent_at_ms = 0U;
    item->priority = priority;
    item->in_flight = false;
    memcpy(item->payload, payload, payload_size);

    if (!priority) {
        if (outbox->tail != NULL)
            outbox->tail->next = item;
        else
            outbox->head = item;
        outbox->tail = item;
    } else {
        edge_memory_message *previous = NULL;
        edge_memory_message *current = outbox->head;
        while (current != NULL && current->priority) {
            previous = current;
            current = current->next;
        }
        item->next = current;
        if (previous != NULL)
            previous->next = item;
        else
            outbox->head = item;
        if (current == NULL)
            outbox->tail = item;
    }
    outbox->bytes += payload_size;
    ++outbox->count;
    return true;
}

const edge_memory_message *edge_memory_outbox_first(const edge_memory_outbox *outbox) {
    return outbox != NULL ? outbox->head : NULL;
}

const edge_memory_message *edge_memory_outbox_oldest_regular(
    const edge_memory_outbox *outbox) {
    if (outbox == NULL)
        return NULL;
    for (edge_memory_message *item = outbox->head; item != NULL; item = item->next)
        if (!item->priority)
            return item;
    return NULL;
}

const edge_memory_message *edge_memory_outbox_next(edge_memory_outbox *outbox,
                                                   uint64_t now_ms) {
    if (outbox == NULL)
        return NULL;
    for (edge_memory_message *item = outbox->head; item != NULL; item = item->next) {
        if (item->in_flight)
            continue;
        item->in_flight = true;
        item->sent_at_ms = now_ms;
        ++outbox->in_flight;
        return item;
    }
    return NULL;
}

bool edge_memory_outbox_timed_out(const edge_memory_outbox *outbox,
                                  uint64_t now_ms, uint32_t timeout_ms) {
    if (outbox == NULL || timeout_ms == 0U)
        return false;
    for (const edge_memory_message *item = outbox->head; item != NULL;
         item = item->next) {
        if (item->in_flight && now_ms >= item->sent_at_ms &&
            now_ms - item->sent_at_ms >= timeout_ms)
            return true;
    }
    return false;
}

bool edge_memory_outbox_retry(edge_memory_outbox *outbox,
                              const uint8_t message_id[16]) {
    if (outbox == NULL || message_id == NULL)
        return false;
    for (edge_memory_message *item = outbox->head; item != NULL; item = item->next) {
        if (memcmp(item->message_id, message_id, 16U) != 0)
            continue;
        if (item->in_flight) {
            item->in_flight = false;
            item->sent_at_ms = 0U;
            --outbox->in_flight;
        }
        return true;
    }
    return false;
}

void edge_memory_outbox_reset(edge_memory_outbox *outbox) {
    if (outbox == NULL)
        return;
    for (edge_memory_message *item = outbox->head; item != NULL; item = item->next) {
        item->in_flight = false;
        item->sent_at_ms = 0U;
    }
    outbox->in_flight = 0U;
}

bool edge_memory_outbox_ack(edge_memory_outbox *outbox, const uint8_t message_id[16]) {
    if (outbox == NULL || message_id == NULL)
        return false;
    edge_memory_message *previous = NULL;
    edge_memory_message *current = outbox->head;
    while (current != NULL && memcmp(current->message_id, message_id, 16U) != 0) {
        previous = current;
        current = current->next;
    }
    if (current == NULL)
        return false;
    if (previous != NULL)
        previous->next = current->next;
    else
        outbox->head = current->next;
    if (outbox->tail == current)
        outbox->tail = previous;
    outbox->bytes -= current->payload_size;
    --outbox->count;
    if (current->in_flight)
        --outbox->in_flight;
    free(current);
    return true;
}
