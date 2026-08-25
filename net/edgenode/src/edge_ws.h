#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include <ev.h>
#include <uwsc/uwsc.h>

#include "edge_config.h"
#include "edge_acquisition.h"
#include "edge_protocol.h"
#include "edge_retry.h"
#include "edge_runtime_config.h"
#include "edge_spool.h"

typedef struct edge_ws_app edge_ws_app;

typedef struct {
    struct uwsc_client client;
    struct ev_timer reconnect_timer;
    struct ev_timer liveness_timer;
    struct ev_timer heartbeat_timer;
    struct ev_timer firmware_timer;
    struct ev_timer modem_timer;
    struct ev_io modem_io;
    struct ev_child modem_child;
    struct ev_timer network_timer;
    struct ev_timer reload_timer;
    struct ev_timer terminal_timer;
    struct ev_timer acquisition_timer;
    struct ev_io acquisition_io;
    edge_ws_app *app;
    const edge_platform_config *config;
    char transport_url[EDGE_URL_MAX + 32U];
    uint8_t node_id[16];
    uint8_t terminal_id[16];
    uint64_t session_epoch;
    uint64_t active_revision;
    uint64_t network_probe_nonce;
    uint8_t firmware_request_id[16];
    uint64_t firmware_total_bytes;
    pid_t modem_worker_pid;
    int modem_worker_fd;
    uint8_t modem_result_request_id[16];
    iot_edge_v1_ModemControlAction modem_result_action;
    iot_edge_v1_ModemControlState modem_result_state;
    char modem_result_message[257];
    char modem_result_apn[101];
    edge_spool spool;
    edge_runtime_config runtime_config;
    edge_acquisition *acquisition;
    uint64_t sequence;
    edge_retry retry;
    uint16_t heartbeat_interval_sec;
    bool client_active;
    bool websocket_open;
    bool enrolled;
    bool terminal_open;
    bool firmware_operation_active;
    bool modem_result_pending;
    bool modem_result_received;
} edge_ws_session;

struct edge_ws_app {
    struct ev_loop *loop;
    const edge_app_config *config;
    edge_ws_session sessions[EDGE_MAX_PLATFORMS];
    iot_edge_v1_Envelope envelope;
    uint8_t wire[EDGENODE_MAX_WS_MESSAGE];
};

bool edge_ws_app_init(edge_ws_app *app, struct ev_loop *loop,
                      const edge_app_config *config);
void edge_ws_app_start(edge_ws_app *app);
void edge_ws_app_stop(edge_ws_app *app);

/*
 * Queues a complete Envelope only in the origin platform's tmpfs outbox. ack_id
 * is the TelemetryRecord record_id or RawPacket packet_id acknowledged upstream.
 */
bool edge_ws_app_enqueue(edge_ws_app *app, const uint8_t origin_platform_id[16],
                         const uint8_t ack_id[16],
                         const iot_edge_v1_Envelope *envelope);
