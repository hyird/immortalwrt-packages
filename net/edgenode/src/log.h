#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

#define EDGE_LOG_RESULT_LIMIT 48U

void edge_log_init(void);

bool edge_log_set_level(const char *level);
const char *edge_log_level(void);
bool edge_log_enabled(const char *level);

void edge_log_write(const char *level, const char *source, const char *message,
                    const char *detail);

void edge_log_packet(const char *source, const char *direction, const char *device,
                     const uint8_t *data, size_t size);

void edge_log_query(const iot_edge_v1_LogRequest *request,
                    iot_edge_v1_LogResult *result);
