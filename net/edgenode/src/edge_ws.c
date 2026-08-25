#include "edge_ws.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <pb_encode.h>

#include "edge_capability.h"
#include "edge_firmware.h"
#include "edge_modem.h"
#include "edge_network.h"
#include "edge_process.h"
#include "edge_sha256.h"
#include "edge_terminal.h"
#include "log.h"

#define EDGE_SOFTWARE_VERSION "0.3.20"
#define EDGE_OUTBOX_WINDOW 16U
#define EDGE_CONNECT_TIMEOUT_SEC 30U
#define EDGE_APPLICATION_HANDSHAKE_TIMEOUT_MS 30000U
#define EDGE_APPLICATION_MIN_TIMEOUT_MS 15000U
#define EDGE_APPLICATION_MAX_TIMEOUT_MS 90000U
#define EDGE_OUTBOX_ACK_TIMEOUT_MS 60000U
#define EDGE_LIVENESS_CHECK_INTERVAL_SEC 1.0
#define EDGE_TERMINAL_OUTPUT_POLL_INTERVAL 0.01
#define EDGE_TERMINAL_ACK_TIMEOUT_MS 15000U

static void send_pending_modem_result(edge_ws_session *session);

static void reset_terminal_flow(edge_ws_session *session) {
    memset(session->terminal_output, 0, sizeof(session->terminal_output));
    session->terminal_output_size = 0U;
    session->terminal_output_sequence = 0U;
    session->terminal_output_acked_sequence = 0U;
    session->terminal_output_deadline_ms = 0U;
    session->terminal_input_ack_sequence = 0U;
    session->terminal_output_pending = false;
    session->terminal_output_sent = false;
    session->terminal_input_ack_pending = false;
}

static edge_ws_session *session_from_client(struct uwsc_client *client) {
    return (edge_ws_session *)((uint8_t *)client - offsetof(edge_ws_session, client));
}

static edge_ws_session *session_from_reconnect(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reconnect_timer));
}

static edge_ws_session *session_from_liveness(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, liveness_timer));
}

static edge_ws_session *session_from_heartbeat(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, heartbeat_timer));
}

static edge_ws_session *session_from_firmware(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, firmware_timer));
}

static edge_ws_session *session_from_modem(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, modem_timer));
}

static edge_ws_session *session_from_modem_io(struct ev_io *watcher) {
    return (edge_ws_session *)((uint8_t *)watcher - offsetof(edge_ws_session, modem_io));
}

static edge_ws_session *session_from_modem_child(struct ev_child *watcher) {
    return (edge_ws_session *)((uint8_t *)watcher - offsetof(edge_ws_session, modem_child));
}

static edge_ws_session *session_from_network(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, network_timer));
}

static edge_ws_session *session_from_reload(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, reload_timer));
}

static edge_ws_session *session_from_terminal(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, terminal_timer));
}

static edge_ws_session *session_from_acquisition(struct ev_timer *timer) {
    return (edge_ws_session *)((uint8_t *)timer - offsetof(edge_ws_session, acquisition_timer));
}

static edge_ws_session *session_from_acquisition_io(struct ev_io *watcher) {
    return (edge_ws_session *)((uint8_t *)watcher - offsetof(edge_ws_session, acquisition_io));
}

static void sync_acquisition_io(edge_ws_session *session) {
    const int fd = edge_acquisition_event_fd(session->acquisition);
    if (ev_is_active(&session->acquisition_io) && session->acquisition_io.fd == fd)
        return;
    ev_io_stop(session->app->loop, &session->acquisition_io);
    if (fd >= 0) {
        ev_io_set(&session->acquisition_io, fd, EV_READ);
        ev_io_start(session->app->loop, &session->acquisition_io);
    }
}

static int64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static uint32_t application_timeout_ms(const edge_ws_session *session) {
    const uint32_t heartbeat = session->heartbeat_interval_sec != 0U
                                   ? session->heartbeat_interval_sec
                                   : session->app->config->heartbeat_interval_sec;
    uint64_t timeout = (uint64_t)heartbeat * 3000U;
    if (timeout < EDGE_APPLICATION_MIN_TIMEOUT_MS)
        timeout = EDGE_APPLICATION_MIN_TIMEOUT_MS;
    if (timeout > EDGE_APPLICATION_MAX_TIMEOUT_MS)
        timeout = EDGE_APPLICATION_MAX_TIMEOUT_MS;
    return (uint32_t)timeout;
}

static bool random_bytes(uint8_t *output, size_t size) {
    FILE *input = fopen("/dev/urandom", "rb");
    if (input == NULL)
        return false;
    const bool ok = fread(output, 1U, size, input) == size;
    fclose(input);
    return ok;
}

static void safe_copy(char *output, size_t capacity, const char *input) {
    if (capacity == 0U)
        return;
    snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static bool init_envelope(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    uint8_t random[10];
    if (!random_bytes(random, sizeof(random)))
        return false;
    return edge_protocol_init_envelope(
        envelope, session->config->id,
        session->enrolled ? session->node_id : NULL,
        session->session_epoch, ++session->sequence, now_ms(), random);
}

static bool send_envelope(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    size_t wire_size = 0U;
    const char *error = NULL;
    if (!session->websocket_open ||
        !edge_protocol_encode(envelope, session->app->wire, sizeof(session->app->wire),
                              &wire_size, &error)) {
        syslog(LOG_ERR, "cannot encode edge envelope: %s", error != NULL ? error : "closed");
        return false;
    }
    return session->client.send(&session->client, session->app->wire, wire_size,
                                UWSC_OP_BINARY) == 0;
}

static bool acquisition_telemetry(void *context,
                                  const iot_edge_v1_TelemetryRecord *record) {
    edge_ws_session *session = context;
    if (record == NULL || record->record_id.size != 16U)
        return false;
    uint8_t record_id[16];
    memcpy(record_id, record->record_id.bytes, sizeof(record_id));
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_telemetry_batch_tag;
    envelope->payload.telemetry_batch.records_count = 1U;
    envelope->payload.telemetry_batch.records[0] = *record;
    return edge_ws_app_enqueue(session->app, session->config->id, record_id, envelope);
}

static bool acquisition_command_result(void *context,
                                       const iot_edge_v1_CommandResult *result) {
    edge_ws_session *session = context;
    if (result == NULL || result->command_id.size != 16U)
        return false;
    uint8_t command_id[16];
    memcpy(command_id, result->command_id.bytes, sizeof(command_id));
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_command_result_tag;
    envelope->payload.command_result = *result;
    return edge_ws_app_enqueue(session->app, session->config->id, command_id, envelope);
}

static void send_outbox_window(edge_ws_session *session) {
    if (!session->enrolled || !session->websocket_open)
        return;
    while (session->spool.outbox.in_flight < EDGE_OUTBOX_WINDOW) {
        const edge_memory_message *message =
            edge_spool_outbox_next(&session->spool, monotonic_ms());
        if (message == NULL)
            return;
        uint8_t message_id[16];
        memcpy(message_id, message->message_id, sizeof(message_id));
        iot_edge_v1_Envelope *envelope = &session->app->envelope;
        const char *error = NULL;
        if (!edge_protocol_decode(message->payload, message->payload_size, envelope,
                                  &error)) {
            syslog(LOG_ERR, "cannot decode tmpfs outbox envelope: %s",
                   error != NULL ? error : "unknown error");
            (void)edge_spool_outbox_ack(&session->spool, message_id);
            continue;
        }
        edge_protocol_set_bytes(&envelope->node_id, sizeof(envelope->node_id.bytes),
                                session->node_id, sizeof(session->node_id));
        envelope->session_epoch = session->session_epoch;
        envelope->sequence = ++session->sequence;
        if (!send_envelope(session, envelope)) {
            (void)edge_spool_outbox_retry(&session->spool, message_id);
            return;
        }
    }
}

static bool send_hello(edge_ws_session *session) {
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_hello_tag;
    iot_edge_v1_Hello *hello = &envelope->payload.hello;
    safe_copy(hello->imei, sizeof(hello->imei), session->app->config->imei);
    safe_copy(hello->model, sizeof(hello->model), session->app->config->model);
    safe_copy(hello->software_version, sizeof(hello->software_version), EDGE_SOFTWARE_VERSION);
    gethostname(hello->hostname, sizeof(hello->hostname) - 1U);
    struct utsname system;
    if (uname(&system) == 0) {
        safe_copy(hello->architecture, sizeof(hello->architecture), system.machine);
        safe_copy(hello->openwrt_release, sizeof(hello->openwrt_release), system.release);
    }
    hello->last_applied_config_version = session->active_revision;
    hello->supported_protocol_versions_count = 1U;
    hello->supported_protocol_versions[0] = EDGENODE_PROTOCOL_VERSION;
    hello->supports_tcp = true;
    hello->supports_serial = true;
    hello->supports_network_config = session->config->network_owner;
    hello->supports_terminal = edge_capability_has_terminal();
    hello->supports_firmware_update = session->config->bootstrap;
    hello->supports_device_config = true;
    hello->network_config_version = 3U;
    hello->supports_logs = true;
    safe_copy(hello->log_level, sizeof(hello->log_level), edge_log_level());
    hello->supports_modem_control =
        session->config->bootstrap && session->app->config->modem_at_port[0] != '\0';
    edge_modem_info modem;
    bool modem_available = false;
    hello->signal_csq = 99U;
    hello->signal_rssi_dbm = -1;
    hello->mobile_registration_status = -1;
    if (edge_modem_read_status(session->app->config->modem_status_path,
                               &modem, &modem_available)) {
        hello->modem_available = modem_available;
        safe_copy(hello->iccid, sizeof(hello->iccid), modem.iccid);
        hello->signal_csq = (uint32_t)modem.csq;
        hello->signal_rssi_dbm = modem.rssi_dbm;
        hello->signal_percent = modem.signal_percent;
        hello->mobile_registered = modem.registered;
        hello->mobile_registration_status = modem.registration_status;
        hello->sim_state = modem.sim_state;
        safe_copy(hello->apn, sizeof(hello->apn), modem.apn);
        safe_copy(hello->mobile_operator, sizeof(hello->mobile_operator),
                  modem.mobile_operator);
        hello->mobile_connected = modem.connected;
        safe_copy(hello->mobile_ipv4, sizeof(hello->mobile_ipv4), modem.mobile_ipv4);
    }
    const bool sent = send_envelope(session, envelope);
    return sent;
}

static bool send_capability_report(edge_ws_session *session) {
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return false;
    envelope->which_payload = iot_edge_v1_Envelope_capability_report_tag;
    iot_edge_v1_CapabilityReport *report = &envelope->payload.capability_report;
    safe_copy(report->network_stack, sizeof(report->network_stack), "netifd");
    report->ttyd_available = edge_capability_has_terminal();
    (void)edge_capability_collect_network(report, session->app->config->wan_interface);
    if (session->app->config->serial_port[0] != '\0') {
        report->serial_ports_count = 1U;
        iot_edge_v1_SerialCapability *serial = &report->serial_ports[0];
        safe_copy(serial->path, sizeof(serial->path), session->app->config->serial_port);
        safe_copy(serial->display_name, sizeof(serial->display_name),
                  session->app->config->serial_port);
        serial->available = access(session->app->config->serial_port, R_OK | W_OK) == 0;
        serial->rs485 = session->app->config->serial_rs485;
    }
    return send_envelope(session, envelope);
}

static void handle_log_request(edge_ws_session *session,
                               const iot_edge_v1_Envelope *input) {
    iot_edge_v1_LogRequest request = input->payload.log_request;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    if (input->message_id.size == 16U)
        edge_protocol_set_bytes(&output->causation_id,
                                sizeof(output->causation_id.bytes),
                                input->message_id.bytes, 16U);
    output->which_payload = iot_edge_v1_Envelope_log_result_tag;
    edge_log_query(&request, &output->payload.log_result);
    if (!send_envelope(session, output))
        edge_log_write("warn", "ws", "log result send failed", "");
}

static void handle_log_level_request(edge_ws_session *session,
                                     const iot_edge_v1_Envelope *input) {
    const iot_edge_v1_LogLevelRequest request = input->payload.log_level_request;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    edge_log_write("info", "log", "log level request received", request.level);
    if (input->message_id.size == 16U)
        edge_protocol_set_bytes(&output->causation_id,
                                sizeof(output->causation_id.bytes),
                                input->message_id.bytes, 16U);
    output->which_payload = iot_edge_v1_Envelope_log_level_result_tag;
    iot_edge_v1_LogLevelResult *result = &output->payload.log_level_result;
    if (request.request_id.size == 16U)
        edge_protocol_set_bytes(&result->request_id,
                                sizeof(result->request_id.bytes),
                                request.request_id.bytes, 16U);
    result->success = edge_log_set_level(request.level);
    safe_copy(result->level, sizeof(result->level), edge_log_level());
    safe_copy(result->message, sizeof(result->message),
              result->success ? "ok" : "invalid log level");
    if (result->success)
        edge_log_write("info", "log", "log level changed", result->level);
    else
        edge_log_write("warn", "log", "invalid log level", request.level);
    if (!send_envelope(session, output))
        edge_log_write("warn", "ws", "log level result send failed", "");
}

static bool send_device_status(edge_ws_session *session) {
    if (session == NULL || !session->websocket_open || !session->enrolled ||
        session->runtime_config.endpoint_count == 0U)
        return true;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_device_status_report_tag;
    edge_acquisition_status(session->acquisition, &output->payload.device_status_report);
    if (output->payload.device_status_report.devices_count == 0U)
        return true;
    return send_envelope(session, output);
}

static void arm_reconnect_timer(edge_ws_session *session) {
    const uint32_t delay_ms = edge_retry_delay_ms(&session->retry, monotonic_ms());
    if (delay_ms == UINT32_MAX)
        return;
    ev_timer_stop(session->app->loop, &session->reconnect_timer);
    ev_timer_set(&session->reconnect_timer,
                 delay_ms == 0U ? 0.001 : (ev_tstamp)delay_ms / 1000.0, 0.0);
    ev_timer_start(session->app->loop, &session->reconnect_timer);
}

static void schedule_reconnect(edge_ws_session *session) {
    edge_spool_outbox_reset(&session->spool);
    session->websocket_open = false;
    session->enrolled = false;
    session->client_active = false;
    session->network_probe_nonce = 0U;
    ev_timer_stop(session->app->loop, &session->liveness_timer);
    ev_timer_stop(session->app->loop, &session->heartbeat_timer);
    ev_timer_stop(session->app->loop, &session->network_timer);
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    if (session->terminal_open) {
        edge_terminal_close(session->terminal_id);
        session->terminal_open = false;
    }
    reset_terminal_flow(session);
    edge_retry_failed(&session->retry, monotonic_ms());
    arm_reconnect_timer(session);
}

static void websocket_open(struct uwsc_client *client) {
    edge_ws_session *session = session_from_client(client);
    session->websocket_open = true;
    edge_retry_transport_connected(&session->retry, monotonic_ms(),
                                   EDGE_APPLICATION_HANDSHAKE_TIMEOUT_MS);
    ev_timer_stop(session->app->loop, &session->reconnect_timer);
    ev_timer_stop(session->app->loop, &session->liveness_timer);
    ev_timer_set(&session->liveness_timer, EDGE_LIVENESS_CHECK_INTERVAL_SEC,
                 EDGE_LIVENESS_CHECK_INTERVAL_SEC);
    ev_timer_start(session->app->loop, &session->liveness_timer);
    if (!send_hello(session)) {
        client->send_close(client, UWSC_CLOSE_STATUS_UNEXPECTED_CONDITION, "hello failed");
        return;
    }
    session->heartbeat_interval_sec = session->app->config->heartbeat_interval_sec;
    syslog(LOG_INFO, "platform %s WebSocket connected", session->config->name);
    char detail[96];
    snprintf(detail, sizeof(detail), "platform=%s", session->config->name);
    edge_log_write("info", "ws", "platform connected", detail);
}

static void websocket_error(struct uwsc_client *client, int error, const char *message) {
    edge_ws_session *session = session_from_client(client);
    syslog(LOG_WARNING, "platform %s WebSocket error %d: %s", session->config->name,
           error, message != NULL ? message : "");
    char detail[128];
    snprintf(detail, sizeof(detail), "platform=%s code=%d message=%s",
             session->config->name, error, message != NULL ? message : "");
    edge_log_write("warn", "ws", "platform websocket error", detail);
    schedule_reconnect(session);
}

static void websocket_close(struct uwsc_client *client, int code, const char *reason) {
    edge_ws_session *session = session_from_client(client);
    syslog(LOG_WARNING, "platform %s WebSocket closed %d: %s", session->config->name,
           code, reason != NULL ? reason : "");
    char detail[128];
    snprintf(detail, sizeof(detail), "platform=%s code=%d reason=%s",
             session->config->name, code, reason != NULL ? reason : "");
    edge_log_write("warn", "ws", "platform websocket closed", detail);
    schedule_reconnect(session);
}

static bool valid_origin(const edge_ws_session *session,
                         const iot_edge_v1_Envelope *envelope) {
    if (envelope->platform_id.size != 16U ||
        memcmp(envelope->platform_id.bytes, session->config->id, 16U) != 0)
        return false;
    if (session->enrolled &&
        (envelope->node_id.size != 16U ||
         memcmp(envelope->node_id.bytes, session->node_id, 16U) != 0 ||
         envelope->session_epoch != session->session_epoch))
        return false;
    return true;
}

static bool encode_config_item(const iot_edge_v1_ConfigItem *item, uint8_t *output,
                               size_t capacity, size_t *output_size) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, iot_edge_v1_ConfigItem_fields, item))
        return false;
    *output_size = stream.bytes_written;
    return true;
}

static bool verify_config_item(iot_edge_v1_ConfigItem *item, uint8_t *encoded,
                               size_t capacity, size_t *encoded_size) {
    if (item->sha256.size != 32U)
        return false;
    uint8_t expected[32];
    memcpy(expected, item->sha256.bytes, sizeof(expected));
    item->sha256.size = 0U;
    size_t canonical_size = 0U;
    if (!encode_config_item(item, encoded, capacity, &canonical_size)) {
        memcpy(item->sha256.bytes, expected, sizeof(expected));
        item->sha256.size = sizeof(expected);
        return false;
    }
    uint8_t actual[32];
    if (edge_sha256(encoded, canonical_size, actual, 0) != 0 ||
        memcmp(expected, actual, sizeof(actual)) != 0) {
        memcpy(item->sha256.bytes, expected, sizeof(expected));
        item->sha256.size = sizeof(expected);
        return false;
    }
    memcpy(item->sha256.bytes, expected, sizeof(expected));
    item->sha256.size = sizeof(expected);
    return encode_config_item(item, encoded, capacity, encoded_size);
}

static void send_config_result(edge_ws_session *session, uint64_t revision,
                               const uint8_t digest[32], bool applied,
                               const char *code, const char *message) {
    uint8_t digest_copy[32];
    memcpy(digest_copy, digest, sizeof(digest_copy));
    iot_edge_v1_Envelope *out = &session->app->envelope;
    if (!init_envelope(session, out))
        return;
    if (applied) {
        out->which_payload = iot_edge_v1_Envelope_config_applied_tag;
        out->payload.config_applied.revision = revision;
        edge_protocol_set_bytes(&out->payload.config_applied.sha256,
                                sizeof(out->payload.config_applied.sha256.bytes), digest_copy, 32U);
        out->payload.config_applied.endpoint_count = session->runtime_config.endpoint_count;
        out->payload.config_applied.device_count = session->runtime_config.device_count;
    } else {
        out->which_payload = iot_edge_v1_Envelope_config_rejected_tag;
        out->payload.config_rejected.revision = revision;
        safe_copy(out->payload.config_rejected.code,
                  sizeof(out->payload.config_rejected.code), code);
        safe_copy(out->payload.config_rejected.message,
                  sizeof(out->payload.config_rejected.message), message);
    }
    send_envelope(session, out);
}

static void handle_config(edge_ws_session *session, iot_edge_v1_Envelope *envelope) {
    if (envelope->which_payload == iot_edge_v1_Envelope_config_begin_tag) {
        iot_edge_v1_ConfigBegin *begin = &envelope->payload.config_begin;
        if (begin->sha256.size != 32U || begin->revision <= session->active_revision ||
            !edge_spool_config_begin(&session->spool, begin->revision,
                                     begin->item_count, begin->sha256.bytes))
            send_config_result(session, begin->revision, begin->sha256.bytes, false,
                               "config_begin_invalid", "configuration begin was rejected");
        return;
    }
    if (envelope->which_payload == iot_edge_v1_Envelope_config_item_tag) {
        iot_edge_v1_ConfigItem *item = &envelope->payload.config_item;
        uint8_t encoded[iot_edge_v1_ConfigItem_size];
        size_t encoded_size = 0U;
        if (!verify_config_item(item, encoded, sizeof(encoded), &encoded_size) ||
            !edge_spool_config_put(&session->spool, item->revision, item->index,
                                   item->sha256.bytes, encoded, encoded_size))
            send_config_result(session, item->revision, item->sha256.bytes, false,
                               "config_item_invalid", "configuration item was rejected");
        return;
    }
    iot_edge_v1_ConfigCommit *commit = &envelope->payload.config_commit;
    edge_runtime_config candidate = {0};
    edge_acquisition *candidate_acquisition = NULL;
    char apply_error[256] = "configuration digest or item count failed";
    if (commit->sha256.size != 32U ||
        !edge_runtime_config_load(&candidate, &session->spool.staging_config,
                                  apply_error, sizeof(apply_error))) {
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_commit_invalid", apply_error);
        return;
    }
    candidate_acquisition = edge_acquisition_create(acquisition_telemetry,
                                                     acquisition_command_result, session);
    if (candidate_acquisition == NULL ||
        !edge_acquisition_apply(candidate_acquisition, &candidate, monotonic_ms(),
                                apply_error, sizeof(apply_error))) {
        edge_acquisition_destroy(candidate_acquisition);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_runtime_invalid", apply_error);
        return;
    }
    ev_io_stop(session->app->loop, &session->acquisition_io);
    edge_acquisition_stop(session->acquisition);
    if (!edge_acquisition_start(candidate_acquisition,
                                apply_error, sizeof(apply_error))) {
        char restart_error[128] = {0};
        if (!edge_acquisition_start(session->acquisition, restart_error,
                                    sizeof(restart_error)))
            edge_log_write("error", "acquisition",
                           "previous acquisition worker could not restart",
                           restart_error);
        sync_acquisition_io(session);
        edge_acquisition_destroy(candidate_acquisition);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_runtime_invalid", apply_error);
        return;
    }
    if (!edge_spool_config_commit(&session->spool, commit->revision,
                                  commit->sha256.bytes)) {
        edge_acquisition_destroy(candidate_acquisition);
        char restart_error[128] = {0};
        if (!edge_acquisition_start(session->acquisition, restart_error,
                                    sizeof(restart_error)))
            edge_log_write("error", "acquisition",
                           "previous acquisition worker could not restart",
                           restart_error);
        sync_acquisition_io(session);
        edge_runtime_config_free(&candidate);
        send_config_result(session, commit->revision, commit->sha256.bytes, false,
                           "config_commit_invalid", "configuration tmpfs commit failed");
        return;
    }
    edge_acquisition_destroy(session->acquisition);
    edge_runtime_config_free(&session->runtime_config);
    session->acquisition = candidate_acquisition;
    session->runtime_config = candidate;
    session->active_revision = commit->revision;
    sync_acquisition_io(session);
    send_config_result(session, commit->revision, commit->sha256.bytes, true, NULL, NULL);
    send_device_status(session);
}

static void handle_device_command(edge_ws_session *session,
                                  const iot_edge_v1_CommandRequest *request) {
    uint8_t command_id[16] = {0};
    uint8_t device_id[16] = {0};
    if (request->command_id.size == 16U)
        memcpy(command_id, request->command_id.bytes, sizeof(command_id));
    if (request->device_id.size == 16U)
        memcpy(device_id, request->device_id.bytes, sizeof(device_id));
    char error[257] = {0};
    if (edge_acquisition_command(session->acquisition, request, error, sizeof(error)))
        return;
    iot_edge_v1_CommandResult result = iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&result.command_id, sizeof(result.command_id.bytes),
                            command_id, sizeof(command_id));
    edge_protocol_set_bytes(&result.device_id, sizeof(result.device_id.bytes),
                            device_id, sizeof(device_id));
    result.state = iot_edge_v1_CommandState_COMMAND_STATE_REJECTED;
    result.completed_at_ms = now_ms();
    safe_copy(result.message, sizeof(result.message),
              error[0] != '\0' ? error : "edge command rejected");
    (void)acquisition_command_result(session, &result);
}

static void send_pong(edge_ws_session *session, const iot_edge_v1_Envelope *input) {
    const uint64_t nonce = input->payload.ping.nonce;
    uint8_t message_id[16];
    memcpy(message_id, input->message_id.bytes, sizeof(message_id));
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_pong_tag;
    output->payload.pong.nonce = nonce;
    edge_protocol_set_bytes(&output->causation_id, sizeof(output->causation_id.bytes),
                            message_id, sizeof(message_id));
    send_envelope(session, output);
}

static void send_firmware_result(edge_ws_session *session, const uint8_t request_id[16],
                                  iot_edge_v1_FirmwareUpdateState state,
                                  const char *message, uint64_t downloaded_bytes,
                                  uint64_t total_bytes, uint32_t progress_percent) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_firmware_update_result_tag;
    edge_protocol_set_bytes(&output->payload.firmware_update_result.request_id,
                            sizeof(output->payload.firmware_update_result.request_id.bytes),
                            request_id, 16U);
    output->payload.firmware_update_result.state = state;
    safe_copy(output->payload.firmware_update_result.message,
              sizeof(output->payload.firmware_update_result.message), message);
    output->payload.firmware_update_result.downloaded_bytes = downloaded_bytes;
    output->payload.firmware_update_result.total_bytes = total_bytes;
    output->payload.firmware_update_result.progress_percent = progress_percent;
    send_envelope(session, output);
}

static void send_network_result(edge_ws_session *session,
                                const uint8_t request_id[16], bool success,
                                bool rolled_back, const char *message) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_network_config_result_tag;
    edge_protocol_set_bytes(&output->payload.network_config_result.request_id,
                            sizeof(output->payload.network_config_result.request_id.bytes),
                            request_id, 16U);
    output->payload.network_config_result.success = success;
    output->payload.network_config_result.rolled_back = rolled_back;
    safe_copy(output->payload.network_config_result.message,
              sizeof(output->payload.network_config_result.message), message);
    send_envelope(session, output);
}

static bool send_modem_result(edge_ws_session *session, const uint8_t request_id[16],
                              iot_edge_v1_ModemControlAction action,
                              iot_edge_v1_ModemControlState state,
                              const char *message, const char *apn) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_modem_control_result_tag;
    edge_protocol_set_bytes(&output->payload.modem_control_result.request_id,
                            sizeof(output->payload.modem_control_result.request_id.bytes),
                            request_id, 16U);
    output->payload.modem_control_result.action = action;
    output->payload.modem_control_result.state = state;
    safe_copy(output->payload.modem_control_result.message,
              sizeof(output->payload.modem_control_result.message), message);
    safe_copy(output->payload.modem_control_result.apn,
              sizeof(output->payload.modem_control_result.apn), apn);
    return send_envelope(session, output);
}

typedef struct {
    uint8_t request_id[16];
    iot_edge_v1_ModemControlAction action;
    bool success;
    char message[257];
    char apn[101];
} edge_modem_worker_result;

static void modem_worker(edge_ws_session *session, int worker_fd,
                         const iot_edge_v1_ModemControlRequest *request) {
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        _exit(EXIT_FAILURE);
    edge_process_close_inherited_fds(worker_fd);
    edge_modem_worker_result result = {0};
    memcpy(result.request_id, request->request_id.bytes, sizeof(result.request_id));
    result.action = request->action;
    safe_copy(result.apn, sizeof(result.apn), request->apn);
    result.success = edge_modem_control(session->app->config->modem_at_port, request,
                                        result.message, sizeof(result.message));
    edge_modem_info modem;
    const bool available = edge_modem_probe(session->app->config->modem_at_port,
                                             session->app->config->wan_interface,
                                             &modem);
    (void)edge_modem_write_status(session->app->config->modem_status_path,
                                  &modem, available);
    for (;;) {
        const ssize_t sent = send(worker_fd, &result, sizeof(result), MSG_NOSIGNAL);
        if (sent == (ssize_t)sizeof(result))
            break;
        if (sent < 0 && errno == EINTR)
            continue;
        break;
    }
    close(worker_fd);
    _exit(result.success ? EXIT_SUCCESS : EXIT_FAILURE);
}

static void handle_network_config(edge_ws_session *session,
                                  const iot_edge_v1_NetworkConfigRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size == 16U)
        memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    bool success =
        edge_network_prepare(request, session->app->config->wan_interface,
                             message, sizeof(message));
    if (success &&
        !edge_network_activate(request->rollback_timeout_sec, request_id)) {
        success = false;
        safe_copy(message, sizeof(message),
                  "could not activate network rollback watchdog");
    }
    if (success)
        safe_copy(message, sizeof(message),
                  "network configuration saved locally and is being applied");
    send_network_result(session, request_id, success, false, message);
    if (success) {
        ev_timer_stop(session->app->loop, &session->network_timer);
        ev_timer_set(&session->network_timer, 2.0, 0.0);
        ev_timer_start(session->app->loop, &session->network_timer);
    }
}

static void handle_firmware_update(edge_ws_session *session,
                                   const iot_edge_v1_FirmwareUpdateRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size == 16U)
        memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    const bool accepted = edge_firmware_start(session->config->id, request,
                                               message, sizeof(message));
    send_firmware_result(
        session, request_id,
        accepted ? iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_ACCEPTED
                 : iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
        accepted ? "firmware update accepted" : message, 0U, request->size_bytes, 0U);
    if (accepted) {
        memcpy(session->firmware_request_id, request_id,
               sizeof(session->firmware_request_id));
        session->firmware_total_bytes = request->size_bytes;
        session->firmware_operation_active = true;
        ev_timer_stop(session->app->loop, &session->firmware_timer);
        ev_timer_set(&session->firmware_timer, 0.25, 0.5);
        ev_timer_start(session->app->loop, &session->firmware_timer);
    }
}

static void handle_modem_control(edge_ws_session *session,
                                 const iot_edge_v1_ModemControlRequest *request) {
    uint8_t request_id[16] = {0};
    if (request->request_id.size != 16U) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "invalid modem request id", request->apn);
        return;
    }
    memcpy(request_id, request->request_id.bytes, sizeof(request_id));
    char message[257] = {0};
    if (!edge_modem_validate_control(request, message, sizeof(message))) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                message, request->apn);
        return;
    }
    if (session->modem_worker_pid > 0 || session->modem_result_pending) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "another modem operation is still pending", request->apn);
        return;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK,
                   0, sockets) != 0) {
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "cannot start modem worker", request->apn);
        return;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        (void)send_modem_result(session, request_id, request->action,
                                iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED,
                                "cannot start modem worker", request->apn);
        return;
    }
    if (child == 0) {
        close(sockets[0]);
        modem_worker(session, sockets[1], request);
    }
    close(sockets[1]);
    session->modem_worker_pid = child;
    session->modem_worker_fd = sockets[0];
    session->modem_result_received = false;
    memcpy(session->modem_result_request_id, request_id,
           sizeof(session->modem_result_request_id));
    session->modem_result_action = request->action;
    session->modem_result_state =
        iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED;
    safe_copy(session->modem_result_message,
              sizeof(session->modem_result_message),
              "modem worker exited before returning a result");
    safe_copy(session->modem_result_apn, sizeof(session->modem_result_apn),
              request->apn);
    (void)send_modem_result(session, request_id, request->action,
                            iot_edge_v1_ModemControlState_MODEM_CONTROL_ACCEPTED,
                            "modem control accepted", request->apn);
    ev_io_stop(session->app->loop, &session->modem_io);
    ev_io_set(&session->modem_io, session->modem_worker_fd, EV_READ);
    ev_io_start(session->app->loop, &session->modem_io);
    ev_child_stop(session->app->loop, &session->modem_child);
    ev_child_set(&session->modem_child, child, 0);
    ev_child_start(session->app->loop, &session->modem_child);
    ev_timer_stop(session->app->loop, &session->modem_timer);
    ev_timer_set(&session->modem_timer, 60.0, 0.0);
    ev_timer_start(session->app->loop, &session->modem_timer);
}

static bool send_terminal_opened(edge_ws_session *session,
                                 const uint8_t terminal_id[16]) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_opened_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_opened.terminal_id,
               sizeof(output->payload.terminal_opened.terminal_id.bytes),
               terminal_id, 16U) &&
           send_envelope(session, output);
}

static void send_terminal_close(edge_ws_session *session, const uint8_t terminal_id[16],
                                int32_t exit_code, const char *reason) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return;
    output->which_payload = iot_edge_v1_Envelope_terminal_close_tag;
    edge_protocol_set_bytes(&output->payload.terminal_close.terminal_id,
                            sizeof(output->payload.terminal_close.terminal_id.bytes),
                            terminal_id, 16U);
    output->payload.terminal_close.exit_code = exit_code;
    safe_copy(output->payload.terminal_close.reason,
              sizeof(output->payload.terminal_close.reason), reason);
    send_envelope(session, output);
}

static bool send_terminal_data_ack(edge_ws_session *session,
                                   const uint8_t terminal_id[16],
                                   uint64_t sequence) {
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_data_ack_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_data_ack.terminal_id,
               sizeof(output->payload.terminal_data_ack.terminal_id.bytes),
               terminal_id, 16U) &&
           (output->payload.terminal_data_ack.sequence = sequence) != 0U &&
           send_envelope(session, output);
}

static void stage_terminal_input_ack(edge_ws_session *session, uint64_t sequence) {
    if (sequence == 0U)
        return;
    session->terminal_input_ack_sequence = sequence;
    session->terminal_input_ack_pending = true;
    if (send_terminal_data_ack(session, session->terminal_id, sequence))
        session->terminal_input_ack_pending = false;
}

static bool send_terminal_output(edge_ws_session *session) {
    if (!session->terminal_output_pending || session->terminal_output_size == 0U ||
        session->terminal_output_sequence == 0U)
        return false;
    iot_edge_v1_Envelope *output = &session->app->envelope;
    if (!init_envelope(session, output))
        return false;
    output->which_payload = iot_edge_v1_Envelope_terminal_data_tag;
    return edge_protocol_set_bytes(
               &output->payload.terminal_data.terminal_id,
               sizeof(output->payload.terminal_data.terminal_id.bytes),
               session->terminal_id, sizeof(session->terminal_id)) &&
           edge_protocol_set_bytes(
               &output->payload.terminal_data.data,
               sizeof(output->payload.terminal_data.data.bytes),
               session->terminal_output, session->terminal_output_size) &&
           (output->payload.terminal_data.sequence =
                session->terminal_output_sequence) != 0U &&
           send_envelope(session, output);
}

static void fail_terminal(edge_ws_session *session, const char *reason) {
    uint8_t terminal_id[16];
    memcpy(terminal_id, session->terminal_id, sizeof(terminal_id));
    edge_terminal_close(terminal_id);
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    session->terminal_open = false;
    send_terminal_close(session, terminal_id, -1, reason);
    reset_terminal_flow(session);
}

static void handle_terminal_open(edge_ws_session *session,
                                 const iot_edge_v1_TerminalOpen *request) {
    uint8_t terminal_id[16] = {0};
    if (request->terminal_id.size == 16U)
        memcpy(terminal_id, request->terminal_id.bytes, sizeof(terminal_id));
    char error[129] = {0};
    if (!edge_terminal_open(request, error, sizeof(error))) {
        send_terminal_close(session, terminal_id, -1, error);
        return;
    }
    reset_terminal_flow(session);
    memcpy(session->terminal_id, terminal_id, sizeof(session->terminal_id));
    session->terminal_open = true;
    if (!send_terminal_opened(session, terminal_id)) {
        edge_terminal_close(terminal_id);
        session->terminal_open = false;
        reset_terminal_flow(session);
        return;
    }
    ev_timer_stop(session->app->loop, &session->terminal_timer);
    ev_timer_set(&session->terminal_timer, EDGE_TERMINAL_OUTPUT_POLL_INTERVAL,
                 EDGE_TERMINAL_OUTPUT_POLL_INTERVAL);
    ev_timer_start(session->app->loop, &session->terminal_timer);
}

static void websocket_message(struct uwsc_client *client, void *data, size_t size, bool binary) {
    edge_ws_session *session = session_from_client(client);
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    const char *error = NULL;
    if (!binary || !edge_protocol_decode(data, size, envelope, &error) ||
        !valid_origin(session, envelope)) {
        syslog(LOG_WARNING, "platform %s sent invalid nanopb envelope: %s",
               session->config->name, error != NULL ? error : "wrong origin");
        client->send_close(client, UWSC_CLOSE_STATUS_PROTOCOL_ERR, "invalid envelope");
        return;
    }
    edge_retry_application_alive(&session->retry, monotonic_ms(),
                                 application_timeout_ms(session));

    switch (envelope->which_payload) {
    case iot_edge_v1_Envelope_hello_ack_tag: {
        iot_edge_v1_HelloAck *ack = &envelope->payload.hello_ack;
        if (ack->assigned_node_id.size != 16U ||
            ack->negotiated_protocol_version != EDGENODE_PROTOCOL_VERSION ||
            ack->session_epoch == 0U) {
            client->send_close(client, UWSC_CLOSE_STATUS_PROTOCOL_ERR, "invalid hello ack");
            return;
        }
        memcpy(session->node_id, ack->assigned_node_id.bytes, 16U);
        session->session_epoch = ack->session_epoch;
        session->enrolled = true;
        syslog(LOG_INFO, "platform %s enrollment approved on existing WebSocket",
               session->config->name);
        if (session->config->network_owner)
            edge_network_confirm();
        const unsigned heartbeat = ack->heartbeat_interval_sec != 0U
                                       ? ack->heartbeat_interval_sec
                                       : session->app->config->heartbeat_interval_sec;
        session->heartbeat_interval_sec = (uint16_t)heartbeat;
        edge_retry_application_ready(&session->retry, monotonic_ms(),
                                     application_timeout_ms(session));
        ev_timer_stop(session->app->loop, &session->heartbeat_timer);
        ev_timer_set(&session->heartbeat_timer, (ev_tstamp)heartbeat, (ev_tstamp)heartbeat);
        ev_timer_start(session->app->loop, &session->heartbeat_timer);
        send_capability_report(session);
        send_device_status(session);
        if (session->config->network_owner) {
            uint8_t rolled_back_request_id[16];
            if (edge_network_take_rollback(rolled_back_request_id))
                send_network_result(session, rolled_back_request_id, false, true,
                                    "network configuration rolled back after reconnect timeout");
        }
        send_outbox_window(session);
        send_pending_modem_result(session);
        break;
    }
    case iot_edge_v1_Envelope_heartbeat_ack_tag:
        if (session->enrolled && envelope->payload.heartbeat_ack.request_capability_report)
            send_capability_report(session);
        if (session->enrolled && envelope->payload.heartbeat_ack.request_device_status)
            send_device_status(session);
        break;
    case iot_edge_v1_Envelope_config_begin_tag:
    case iot_edge_v1_Envelope_config_item_tag:
    case iot_edge_v1_Envelope_config_commit_tag:
        if (session->enrolled)
            handle_config(session, envelope);
        break;
    case iot_edge_v1_Envelope_telemetry_ack_tag:
        for (pb_size_t index = 0; index < envelope->payload.telemetry_ack.accepted_record_ids_count;
             ++index) {
            const iot_edge_v1_TelemetryAck_accepted_record_ids_t *id =
                &envelope->payload.telemetry_ack.accepted_record_ids[index];
            if (id->size == 16U)
                edge_spool_outbox_ack(&session->spool, id->bytes);
        }
        send_outbox_window(session);
        break;
    case iot_edge_v1_Envelope_raw_packet_ack_tag:
        if (envelope->payload.raw_packet_ack.packet_id.size == 16U) {
            edge_spool_outbox_ack(
                &session->spool, envelope->payload.raw_packet_ack.packet_id.bytes);
            send_outbox_window(session);
        }
        break;
    case iot_edge_v1_Envelope_command_result_ack_tag:
        if (envelope->payload.command_result_ack.command_id.size == 16U) {
            edge_spool_outbox_ack(
                &session->spool, envelope->payload.command_result_ack.command_id.bytes);
            send_outbox_window(session);
        }
        break;
    case iot_edge_v1_Envelope_command_request_tag:
        if (session->enrolled)
            handle_device_command(session, &envelope->payload.command_request);
        break;
    case iot_edge_v1_Envelope_ping_tag:
        send_pong(session, envelope);
        break;
    case iot_edge_v1_Envelope_pong_tag:
        if (session->network_probe_nonce != 0U &&
            envelope->payload.pong.nonce == session->network_probe_nonce) {
            session->network_probe_nonce = 0U;
            edge_network_confirm();
            send_capability_report(session);
        }
        break;
    case iot_edge_v1_Envelope_network_config_request_tag:
        if (session->enrolled && session->config->network_owner)
            handle_network_config(session, &envelope->payload.network_config_request);
        break;
    case iot_edge_v1_Envelope_firmware_update_request_tag:
        if (session->enrolled && session->config->bootstrap)
            handle_firmware_update(session, &envelope->payload.firmware_update_request);
        break;
    case iot_edge_v1_Envelope_modem_control_request_tag:
        if (session->enrolled && session->config->bootstrap)
            handle_modem_control(session, &envelope->payload.modem_control_request);
        break;
    case iot_edge_v1_Envelope_log_request_tag:
        if (session->enrolled)
            handle_log_request(session, envelope);
        break;
    case iot_edge_v1_Envelope_log_level_request_tag:
        if (session->enrolled)
            handle_log_level_request(session, envelope);
        break;
    case iot_edge_v1_Envelope_terminal_open_tag:
        if (session->enrolled && session->config->bootstrap &&
            edge_capability_has_terminal())
            handle_terminal_open(session, &envelope->payload.terminal_open);
        break;
    case iot_edge_v1_Envelope_terminal_data_tag:
        if (session->terminal_open) {
            uint64_t acked_sequence = 0U;
            const edge_terminal_input_result result =
                edge_terminal_write(&envelope->payload.terminal_data,
                                    &acked_sequence);
            if (result == EDGE_TERMINAL_INPUT_ACKED)
                stage_terminal_input_ack(session, acked_sequence);
            else if (result == EDGE_TERMINAL_INPUT_ERROR)
                fail_terminal(session, "terminal input sequence or write failed");
        }
        break;
    case iot_edge_v1_Envelope_terminal_data_ack_tag:
        if (session->terminal_open) {
            const iot_edge_v1_TerminalDataAck *ack =
                &envelope->payload.terminal_data_ack;
            if (ack->terminal_id.size != 16U || ack->sequence == 0U ||
                memcmp(ack->terminal_id.bytes, session->terminal_id,
                       sizeof(session->terminal_id)) != 0 ||
                (!session->terminal_output_pending &&
                 ack->sequence != session->terminal_output_acked_sequence) ||
                (session->terminal_output_pending &&
                 ack->sequence != session->terminal_output_sequence)) {
                fail_terminal(session, "terminal output acknowledgement mismatch");
            } else if (session->terminal_output_pending) {
                session->terminal_output_acked_sequence = ack->sequence;
                session->terminal_output_size = 0U;
                session->terminal_output_deadline_ms = 0U;
                session->terminal_output_pending = false;
                session->terminal_output_sent = false;
            }
        }
        break;
    case iot_edge_v1_Envelope_terminal_resize_tag:
        if (session->terminal_open &&
            !edge_terminal_resize(&envelope->payload.terminal_resize))
            fail_terminal(session, "terminal resize failed");
        break;
    case iot_edge_v1_Envelope_terminal_close_tag:
        if (session->terminal_open) {
            if (envelope->payload.terminal_close.terminal_id.size != 16U ||
                memcmp(envelope->payload.terminal_close.terminal_id.bytes,
                       session->terminal_id, sizeof(session->terminal_id)) != 0) {
                fail_terminal(session, "terminal close identity mismatch");
            } else {
                edge_terminal_close(session->terminal_id);
                ev_timer_stop(session->app->loop, &session->terminal_timer);
                session->terminal_open = false;
                reset_terminal_flow(session);
            }
        }
        break;
    case iot_edge_v1_Envelope_enrollment_pending_tag:
        syslog(LOG_INFO, "platform %s enrollment pending", session->config->name);
        break;
    case iot_edge_v1_Envelope_enrollment_rejected_tag:
        syslog(LOG_WARNING, "platform %s enrollment rejected", session->config->name);
        client->send_close(client, UWSC_CLOSE_STATUS_POLICY_VIOLATION,
                           "enrollment rejected");
        break;
    default:
        break;
    }
}

static void heartbeat_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_heartbeat(timer);
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return;
    envelope->which_payload = iot_edge_v1_Envelope_heartbeat_tag;
    iot_edge_v1_Heartbeat *heartbeat = &envelope->payload.heartbeat;
    heartbeat->signal_csq = 99U;
    heartbeat->signal_rssi_dbm = -1;
    heartbeat->mobile_registration_status = -1;
    heartbeat->supports_modem_control =
        session->config->bootstrap && session->app->config->modem_at_port[0] != '\0';
    safe_copy(heartbeat->log_level, sizeof(heartbeat->log_level), edge_log_level());
    edge_modem_info modem;
    bool modem_available = false;
    if (edge_modem_read_status(session->app->config->modem_status_path,
                               &modem, &modem_available)) {
        heartbeat->modem_available = modem_available;
        safe_copy(heartbeat->iccid, sizeof(heartbeat->iccid), modem.iccid);
        heartbeat->signal_csq = (uint32_t)modem.csq;
        heartbeat->signal_rssi_dbm = modem.rssi_dbm;
        heartbeat->signal_percent = modem.signal_percent;
        heartbeat->mobile_registered = modem.registered;
        heartbeat->mobile_registration_status = modem.registration_status;
        heartbeat->sim_state = modem.sim_state;
        safe_copy(heartbeat->apn, sizeof(heartbeat->apn), modem.apn);
        safe_copy(heartbeat->mobile_operator, sizeof(heartbeat->mobile_operator),
                  modem.mobile_operator);
        heartbeat->mobile_connected = modem.connected;
        safe_copy(heartbeat->mobile_ipv4, sizeof(heartbeat->mobile_ipv4),
                  modem.mobile_ipv4);
    }
    struct sysinfo info;
    if (sysinfo(&info) == 0)
        heartbeat->uptime_sec = (uint64_t)info.uptime;
    heartbeat->active_config_version = session->active_revision;
    heartbeat->managed_endpoint_count = session->runtime_config.endpoint_count;
    heartbeat->managed_device_count = session->runtime_config.device_count;
    edge_spool_maintain(&session->spool);
    heartbeat->outbox_records = session->spool.outbox.count;
    heartbeat->outbox_bytes = session->spool.outbox.bytes;
    send_envelope(session, envelope);
    send_outbox_window(session);
}

static void firmware_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_firmware(timer);
    if (!session->enrolled)
        return;
    iot_edge_v1_FirmwareUpdateResult result = iot_edge_v1_FirmwareUpdateResult_init_zero;
    if (!edge_firmware_read_status(session->config->id, &result)) {
        if (session->firmware_operation_active && !edge_firmware_active()) {
            send_firmware_result(
                session, session->firmware_request_id,
                iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                "firmware worker exited unexpectedly", 0U,
                session->firmware_total_bytes, 0U);
            session->firmware_operation_active = false;
            ev_timer_stop(session->app->loop, &session->firmware_timer);
        }
        return;
    }
    uint8_t request_id[16] = {0};
    if (result.request_id.size == 16U)
        memcpy(request_id, result.request_id.bytes, sizeof(request_id));
    memcpy(session->firmware_request_id, request_id,
           sizeof(session->firmware_request_id));
    session->firmware_total_bytes = result.total_bytes;
    session->firmware_operation_active = true;
    const iot_edge_v1_FirmwareUpdateState state = result.state;
    char message[257];
    safe_copy(message, sizeof(message), result.message);
    send_firmware_result(session, request_id, state, message,
                         result.downloaded_bytes, result.total_bytes,
                         result.progress_percent);
    if (state == iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED) {
        session->firmware_operation_active = false;
        ev_timer_stop(session->app->loop, &session->firmware_timer);
    }
}

static void send_pending_modem_result(edge_ws_session *session) {
    if (session->modem_result_pending && session->websocket_open && session->enrolled &&
        send_modem_result(session, session->modem_result_request_id,
                          session->modem_result_action, session->modem_result_state,
                          session->modem_result_message, session->modem_result_apn))
        session->modem_result_pending = false;
}

static void read_modem_worker_result(edge_ws_session *session) {
    if (session->modem_worker_fd >= 0) {
        edge_modem_worker_result result;
        const ssize_t size = recv(session->modem_worker_fd, &result, sizeof(result), 0);
        if (size == (ssize_t)sizeof(result)) {
            memcpy(session->modem_result_request_id, result.request_id,
                   sizeof(session->modem_result_request_id));
            session->modem_result_action = result.action;
            session->modem_result_state =
                result.success
                    ? iot_edge_v1_ModemControlState_MODEM_CONTROL_SUCCEEDED
                    : iot_edge_v1_ModemControlState_MODEM_CONTROL_FAILED;
            safe_copy(session->modem_result_message,
                      sizeof(session->modem_result_message), result.message);
            safe_copy(session->modem_result_apn, sizeof(session->modem_result_apn),
                      result.apn);
            session->modem_result_received = true;
            session->modem_result_pending = true;
            ev_io_stop(session->app->loop, &session->modem_io);
            close(session->modem_worker_fd);
            session->modem_worker_fd = -1;
        } else if (size == 0 ||
                   (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                    errno != EINTR)) {
            ev_io_stop(session->app->loop, &session->modem_io);
            close(session->modem_worker_fd);
            session->modem_worker_fd = -1;
        }
    }
}

static void modem_io(struct ev_loop *loop, struct ev_io *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_modem_io(watcher);
    read_modem_worker_result(session);
    send_pending_modem_result(session);
}

static void modem_child(struct ev_loop *loop, struct ev_child *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_modem_child(watcher);
    ev_child_stop(session->app->loop, &session->modem_child);
    ev_timer_stop(session->app->loop, &session->modem_timer);
    read_modem_worker_result(session);
    session->modem_worker_pid = 0;
    if (session->modem_worker_fd >= 0) {
        ev_io_stop(session->app->loop, &session->modem_io);
        close(session->modem_worker_fd);
        session->modem_worker_fd = -1;
    }
    if (!session->modem_result_received)
        session->modem_result_pending = true;
    send_pending_modem_result(session);
}

static void modem_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_modem(timer);
    ev_timer_stop(session->app->loop, &session->modem_timer);
    if (session->modem_worker_pid > 0) {
        safe_copy(session->modem_result_message,
                  sizeof(session->modem_result_message),
                  "modem operation timed out");
        (void)kill(session->modem_worker_pid, SIGKILL);
    }
}

static void network_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_network(timer);
    ev_timer_stop(session->app->loop, &session->network_timer);
    if (!session->websocket_open || !session->enrolled)
        return;
    iot_edge_v1_Envelope *envelope = &session->app->envelope;
    if (!init_envelope(session, envelope))
        return;
    envelope->which_payload = iot_edge_v1_Envelope_ping_tag;
    const int64_t timestamp = now_ms();
    session->network_probe_nonce =
        timestamp > 0 ? (uint64_t)timestamp : session->sequence;
    envelope->payload.ping.nonce = session->network_probe_nonce;
    if (!send_envelope(session, envelope))
        session->network_probe_nonce = 0U;
}

static void reload_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_reload(timer);
    ev_timer_stop(session->app->loop, &session->reload_timer);
    raise(SIGHUP);
}

static void terminal_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_terminal(timer);
    if (!session->terminal_open)
        return;
    if (session->terminal_input_ack_pending &&
        send_terminal_data_ack(session, session->terminal_id,
                               session->terminal_input_ack_sequence))
        session->terminal_input_ack_pending = false;

    uint64_t input_acked_sequence = 0U;
    const edge_terminal_input_result input_result =
        edge_terminal_flush(&input_acked_sequence);
    if (input_result == EDGE_TERMINAL_INPUT_ERROR) {
        fail_terminal(session, "terminal input write failed");
        return;
    }
    if (input_result == EDGE_TERMINAL_INPUT_ACKED)
        stage_terminal_input_ack(session, input_acked_sequence);

    const uint64_t now = monotonic_ms();
    if (session->terminal_output_pending) {
        if (now >= session->terminal_output_deadline_ms) {
            fail_terminal(session, "terminal output acknowledgement timed out");
            return;
        }
        if (!session->terminal_output_sent && send_terminal_output(session))
            session->terminal_output_sent = true;
        return;
    }

    uint8_t terminal_id[16] = {0};
    bool closed = false;
    int32_t exit_code = 0;
    const ssize_t size =
        edge_terminal_read(terminal_id, session->terminal_output,
                           sizeof(session->terminal_output), &closed, &exit_code);
    if (size > 0) {
        if (session->terminal_output_sequence == UINT64_MAX) {
            fail_terminal(session, "terminal output sequence exhausted");
            return;
        }
        session->terminal_output_size = (size_t)size;
        ++session->terminal_output_sequence;
        session->terminal_output_pending = true;
        session->terminal_output_sent = send_terminal_output(session);
        session->terminal_output_deadline_ms = now + EDGE_TERMINAL_ACK_TIMEOUT_MS;
    }
    if (closed) {
        ev_timer_stop(session->app->loop, &session->terminal_timer);
        session->terminal_open = false;
        send_terminal_close(session, terminal_id, exit_code, "terminal closed");
        reset_terminal_flow(session);
    }
}

static void acquisition_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_acquisition(timer);
    edge_acquisition_tick(session->acquisition, monotonic_ms());
    sync_acquisition_io(session);
}

static void acquisition_io(struct ev_loop *loop, struct ev_io *watcher, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_acquisition_io(watcher);
    edge_acquisition_tick(session->acquisition, monotonic_ms());
    sync_acquisition_io(session);
}

static bool make_transport_url(const char *base, char *output, size_t capacity) {
    if (strncmp(base, "https://", 8U) != 0)
        return false;
    const char *host = base + 8U;
    size_t host_size = strlen(host);
    while (host_size != 0U && host[host_size - 1U] == '/')
        --host_size;
    const int size = snprintf(output, capacity, "wss://%.*s/edge/v1/connect",
                              (int)host_size, host);
    return host_size != 0U && size > 0 && (size_t)size < capacity;
}

static void start_connection(edge_ws_session *session) {
    edge_retry_attempt_started(&session->retry, monotonic_ms());
    if (!make_transport_url(session->config->url, session->transport_url,
                            sizeof(session->transport_url))) {
        syslog(LOG_WARNING, "platform %s has an invalid WebSocket base URL",
               session->config->name);
        schedule_reconnect(session);
        return;
    }
    syslog(LOG_INFO, "platform %s WebSocket connecting to %s", session->config->name,
           session->transport_url);
    if (uwsc_init(&session->client, session->app->loop, session->transport_url,
                  session->app->config->heartbeat_interval_sec, NULL) != 0) {
        syslog(LOG_WARNING, "platform %s WebSocket connect initialization failed",
               session->config->name);
        schedule_reconnect(session);
        return;
    }
    /* The packaged libuwsc requires the CA bundle and verifies the peer hostname. */
    session->client.onopen = websocket_open;
    session->client.onmessage = websocket_message;
    session->client.onerror = websocket_error;
    session->client.onclose = websocket_close;
    session->client_active = true;
    arm_reconnect_timer(session);
}

static void reconnect_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_reconnect(timer);
    const uint64_t current_ms = monotonic_ms();
    if (edge_retry_attempt_timed_out(&session->retry, current_ms)) {
        syslog(LOG_WARNING, "platform %s WebSocket connection attempt timed out",
               session->config->name);
        if (session->client_active && session->client.free != NULL)
            session->client.free(&session->client);
        session->client_active = false;
        schedule_reconnect(session);
        return;
    }
    if (!edge_retry_should_start(&session->retry, current_ms)) {
        arm_reconnect_timer(session);
        return;
    }
    start_connection(session);
}

static void liveness_timer(struct ev_loop *loop, struct ev_timer *timer, int events) {
    (void)loop;
    (void)events;
    edge_ws_session *session = session_from_liveness(timer);
    if (!session->websocket_open)
        return;
    const uint64_t current_ms = monotonic_ms();
    const bool application_stalled =
        edge_retry_application_timed_out(&session->retry, current_ms);
    const bool acknowledgement_stalled =
        edge_spool_outbox_timed_out(&session->spool, current_ms,
                                    EDGE_OUTBOX_ACK_TIMEOUT_MS);
    if (!application_stalled && !acknowledgement_stalled)
        return;
    const char *reason = application_stalled
                             ? "application heartbeat timed out"
                             : "outbox acknowledgement timed out";
    syslog(LOG_WARNING, "platform %s %s; reconnecting",
           session->config->name, reason);
    char detail[160];
    snprintf(detail, sizeof(detail), "platform=%s reason=%s",
             session->config->name, reason);
    edge_log_write("warn", "ws", "platform session stalled", detail);
    if (session->client_active && session->client.free != NULL)
        session->client.free(&session->client);
    session->client_active = false;
    schedule_reconnect(session);
}

bool edge_ws_app_init(edge_ws_app *app, struct ev_loop *loop,
                      const edge_app_config *config) {
    if (app == NULL || loop == NULL || config == NULL)
        return false;
    memset(app, 0, sizeof(*app));
    edge_log_init();
    app->loop = loop;
    app->config = config;
    for (size_t index = 0; index < config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        session->app = app;
        session->config = &config->platforms[index];
        session->modem_worker_fd = -1;
        if (!edge_spool_init(&session->spool, session->config->id,
                             session->config->outbox_max_bytes)) {
            for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                edge_acquisition_destroy(app->sessions[cleanup].acquisition);
                edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                edge_spool_free(&app->sessions[cleanup].spool);
            }
            return false;
        }
        session->active_revision = session->spool.active_config.revision;
        if (session->active_revision != 0U) {
            char error[256];
            if (!edge_runtime_config_load(&session->runtime_config,
                                          &session->spool.active_config,
                                          error, sizeof(error))) {
                syslog(LOG_ERR, "platform %s active config rejected: %s",
                       session->config->name, error);
                for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                    edge_acquisition_destroy(app->sessions[cleanup].acquisition);
                    edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                    edge_spool_free(&app->sessions[cleanup].spool);
                }
                return false;
            }
        }
        session->acquisition = edge_acquisition_create(
            acquisition_telemetry, acquisition_command_result, session);
        char acquisition_error[256] = {0};
        if (session->acquisition == NULL ||
            !edge_acquisition_apply(session->acquisition, &session->runtime_config,
                                    monotonic_ms(), acquisition_error,
                                    sizeof(acquisition_error)) ||
            !edge_acquisition_start(session->acquisition, acquisition_error,
                                    sizeof(acquisition_error))) {
            syslog(LOG_ERR, "platform %s acquisition config rejected: %s",
                   session->config->name, acquisition_error);
            for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                edge_acquisition_destroy(app->sessions[cleanup].acquisition);
                edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                edge_spool_free(&app->sessions[cleanup].spool);
            }
            return false;
        }
        ev_timer_init(&session->reconnect_timer, reconnect_timer, 0.0, 0.0);
        if (!edge_retry_init(&session->retry,
                             (uint32_t)session->config->reconnect_interval_sec * 1000U,
                             EDGE_CONNECT_TIMEOUT_SEC * 1000U)) {
            for (size_t cleanup = 0; cleanup <= index; ++cleanup) {
                edge_acquisition_destroy(app->sessions[cleanup].acquisition);
                edge_runtime_config_free(&app->sessions[cleanup].runtime_config);
                edge_spool_free(&app->sessions[cleanup].spool);
            }
            return false;
        }
        ev_timer_init(&session->liveness_timer, liveness_timer, 0.0, 0.0);
        ev_timer_init(&session->heartbeat_timer, heartbeat_timer, 0.0, 0.0);
        ev_timer_init(&session->firmware_timer, firmware_timer, 0.0, 0.0);
        ev_timer_init(&session->modem_timer, modem_timer, 0.0, 0.0);
        ev_io_init(&session->modem_io, modem_io, 0, EV_READ);
        ev_child_init(&session->modem_child, modem_child, 0, 0);
        ev_timer_init(&session->network_timer, network_timer, 0.0, 0.0);
        ev_timer_init(&session->reload_timer, reload_timer, 0.0, 0.0);
        ev_timer_init(&session->terminal_timer, terminal_timer, 0.0, 0.0);
        ev_timer_init(&session->acquisition_timer, acquisition_timer, 0.0, 0.0);
        ev_io_init(&session->acquisition_io, acquisition_io, 0, EV_READ);
    }
    return true;
}

void edge_ws_app_start(edge_ws_app *app) {
    if (app == NULL)
        return;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        if (app->sessions[index].config->bootstrap &&
            (edge_firmware_active() ||
             edge_firmware_has_status(app->sessions[index].config->id))) {
            app->sessions[index].firmware_operation_active = true;
            ev_timer_set(&app->sessions[index].firmware_timer, 0.25, 0.5);
            ev_timer_start(app->loop, &app->sessions[index].firmware_timer);
        }
        ev_timer_set(&app->sessions[index].acquisition_timer, 0.0, 1.0);
        ev_timer_start(app->loop, &app->sessions[index].acquisition_timer);
        sync_acquisition_io(&app->sessions[index]);
        start_connection(&app->sessions[index]);
    }
}

void edge_ws_app_stop(edge_ws_app *app) {
    if (app == NULL)
        return;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        edge_ws_session *session = &app->sessions[index];
        ev_timer_stop(app->loop, &session->reconnect_timer);
        ev_timer_stop(app->loop, &session->liveness_timer);
        ev_timer_stop(app->loop, &session->heartbeat_timer);
        ev_timer_stop(app->loop, &session->firmware_timer);
        ev_timer_stop(app->loop, &session->modem_timer);
        ev_timer_stop(app->loop, &session->network_timer);
        ev_timer_stop(app->loop, &session->reload_timer);
        ev_timer_stop(app->loop, &session->terminal_timer);
        ev_timer_stop(app->loop, &session->acquisition_timer);
        ev_io_stop(app->loop, &session->modem_io);
        ev_child_stop(app->loop, &session->modem_child);
        ev_io_stop(app->loop, &session->acquisition_io);
        if (session->terminal_open) {
            edge_terminal_close(session->terminal_id);
            session->terminal_open = false;
        }
        reset_terminal_flow(session);
        if (session->modem_worker_pid > 0) {
            (void)kill(session->modem_worker_pid, SIGTERM);
            bool reaped = false;
            for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
                pid_t waited;
                do {
                    waited = waitpid(session->modem_worker_pid, NULL, WNOHANG);
                } while (waited < 0 && errno == EINTR);
                if (waited == session->modem_worker_pid ||
                    (waited < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                usleep(5000U);
            }
            if (!reaped) {
                (void)kill(session->modem_worker_pid, SIGKILL);
                while (waitpid(session->modem_worker_pid, NULL, 0) < 0 &&
                       errno == EINTR) {
                }
            }
            session->modem_worker_pid = 0;
        }
        if (session->modem_worker_fd >= 0) {
            close(session->modem_worker_fd);
            session->modem_worker_fd = -1;
        }
        if (session->client_active)
            session->client.free(&session->client);
        edge_spool_free(&session->spool);
        edge_acquisition_destroy(session->acquisition);
        session->acquisition = NULL;
        edge_runtime_config_free(&session->runtime_config);
        session->client_active = false;
        session->websocket_open = false;
    }
}

bool edge_ws_app_enqueue(edge_ws_app *app, const uint8_t origin_platform_id[16],
                         const uint8_t ack_id[16],
                         const iot_edge_v1_Envelope *envelope) {
    if (app == NULL || origin_platform_id == NULL || ack_id == NULL || envelope == NULL ||
        envelope->platform_id.size != 16U ||
        memcmp(envelope->platform_id.bytes, origin_platform_id, 16U) != 0)
        return false;
    edge_ws_session *session = NULL;
    for (size_t index = 0; index < app->config->platform_count; ++index) {
        if (memcmp(app->sessions[index].config->id, origin_platform_id, 16U) == 0) {
            session = &app->sessions[index];
            break;
        }
    }
    if (session == NULL)
        return false;
    size_t wire_size = 0U;
    const char *error = NULL;
    if (!edge_protocol_encode(envelope, app->wire, sizeof(app->wire), &wire_size, &error) ||
        !edge_spool_outbox_put_priority(
            &session->spool, ack_id, app->wire, wire_size,
            envelope->which_payload == iot_edge_v1_Envelope_command_result_tag)) {
        syslog(LOG_ERR, "cannot queue nanopb message for origin platform %s: %s",
               session->config->name, error != NULL ? error : "tmpfs spool rejected");
        return false;
    }
    send_outbox_window(session);
    return true;
}
