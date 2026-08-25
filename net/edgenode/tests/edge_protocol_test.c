#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_encode.h>

#include "edge_protocol.h"

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge protocol test failed: %s\n", message);
        exit(1);
    }
}

static void test_imei(void) {
    require(edge_protocol_validate_imei("490154203237518"), "valid IMEI rejected");
    require(!edge_protocol_validate_imei("490154203237519"), "bad check digit accepted");
    require(!edge_protocol_validate_imei("49015420323751"), "short IMEI accepted");
    require(!edge_protocol_validate_imei("49015420323751A"), "non-digit IMEI accepted");
}

static void test_hello_round_trip(void) {
    const uint8_t platform_id[16] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t random_bytes[10] = {0x10, 0x11, 0x12, 0x13, 0x14,
                                      0x15, 0x16, 0x17, 0x18, 0x19};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 1U,
                                        1784688000123LL, random_bytes),
            "envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_hello_tag;
    strcpy(envelope.payload.hello.imei, "490154203237518");
    strcpy(envelope.payload.hello.model, "openwrt-test");
    strcpy(envelope.payload.hello.software_version, "0.1.0");
    strcpy(envelope.payload.hello.iccid, "89860012345678901234");
    envelope.payload.hello.signal_csq = 23U;
    envelope.payload.hello.signal_rssi_dbm = -67;
    envelope.payload.hello.signal_percent = 74U;
    envelope.payload.hello.mobile_registered = true;
    envelope.payload.hello.mobile_registration_status = 1;
    envelope.payload.hello.supported_protocol_versions_count = 1U;
    envelope.payload.hello.supported_protocol_versions[0] = EDGENODE_PROTOCOL_VERSION;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "encode failed");
    require(encoded_size > 0U, "empty encoded envelope");

    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "decode failed");
    require(decoded.which_payload == iot_edge_v1_Envelope_hello_tag, "wrong payload tag");
    require(strcmp(decoded.payload.hello.imei, "490154203237518") == 0,
            "IMEI changed during round trip");
    require(strcmp(decoded.payload.hello.iccid, "89860012345678901234") == 0,
            "ICCID changed during round trip");
    require(decoded.payload.hello.signal_csq == 23U &&
                decoded.payload.hello.signal_rssi_dbm == -67 &&
                decoded.payload.hello.signal_percent == 74U,
            "signal state changed during round trip");
    require(decoded.payload.hello.mobile_registered &&
                decoded.payload.hello.mobile_registration_status == 1,
            "mobile registration changed during round trip");
    require(decoded.message_id.size == 16U && (decoded.message_id.bytes[6] >> 4U) == 7U,
            "message id is not UUIDv7");
    require((decoded.message_id.bytes[8] & 0xc0U) == 0x80U, "bad UUID variant");
}

static void test_heartbeat_mobile_state_round_trip(void) {
    const uint8_t platform_id[16] = {0x01};
    const uint8_t random_bytes[10] = {0x20};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 2U,
                                        1784688030123LL, random_bytes),
            "heartbeat envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_heartbeat_tag;
    iot_edge_v1_Heartbeat *heartbeat = &envelope.payload.heartbeat;
    heartbeat->uptime_sec = 300U;
    strcpy(heartbeat->iccid, "89860012345678901234");
    heartbeat->signal_csq = 23U;
    heartbeat->signal_rssi_dbm = -67;
    heartbeat->signal_percent = 74U;
    heartbeat->mobile_registered = true;
    heartbeat->mobile_registration_status = 1;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "heartbeat encode failed");

    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "heartbeat decode failed");
    require(decoded.which_payload == iot_edge_v1_Envelope_heartbeat_tag,
            "wrong heartbeat payload tag");
    const iot_edge_v1_Heartbeat *round_trip = &decoded.payload.heartbeat;
    require(strcmp(round_trip->iccid, "89860012345678901234") == 0,
            "heartbeat ICCID changed during round trip");
    require(round_trip->signal_csq == 23U && round_trip->signal_rssi_dbm == -67 &&
                round_trip->signal_percent == 74U && round_trip->mobile_registered &&
                round_trip->mobile_registration_status == 1,
            "heartbeat mobile state changed during round trip");
}

static void test_modem_profile_round_trip(void) {
    const uint8_t platform_id[16] = {0x02};
    const uint8_t random_bytes[10] = {0x30};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 3U,
                                        1784688060123LL, random_bytes),
            "modem envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_modem_control_request_tag;
    iot_edge_v1_ModemControlRequest *request =
        &envelope.payload.modem_control_request;
    memset(request->request_id.bytes, 0x01, sizeof(request->request_id.bytes));
    request->request_id.size = 16U;
    request->action =
        iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE;
    strcpy(request->apn, "private.mnc001.mcc460.gprs");
    strcpy(request->username, "edge-user");
    strcpy(request->password, "secret");
    request->pdp_type = iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4V6;
    request->auth_type = iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP_OR_CHAP;
    strcpy(request->pin_code, "1234");
    request->redial_after_apply = true;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "modem profile encode failed");
    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "modem profile decode failed");
    const iot_edge_v1_ModemControlRequest *profile =
        &decoded.payload.modem_control_request;
    require(profile->action ==
                iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE &&
                strcmp(profile->apn, "private.mnc001.mcc460.gprs") == 0 &&
                strcmp(profile->username, "edge-user") == 0 &&
                strcmp(profile->password, "secret") == 0 &&
                profile->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4V6 &&
                profile->auth_type ==
                    iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP_OR_CHAP &&
                strcmp(profile->pin_code, "1234") == 0 &&
                profile->redial_after_apply,
            "modem profile changed during round trip");
}

static void test_cpp_protobuf_wire_contract(void) {
    iot_edge_v1_NetworkConfigRequest request =
        iot_edge_v1_NetworkConfigRequest_init_zero;
    const uint8_t request_id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    require(edge_protocol_set_bytes(&request.request_id, sizeof(request.request_id.bytes),
                                    request_id, sizeof(request_id)),
            "network request id setup failed");
    request.interfaces_count = 1U;
    iot_edge_v1_NetworkInterfaceConfig *interface = &request.interfaces[0];
    strcpy(interface->name, "eth0");
    interface->mode = iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_DHCP;
    interface->operation =
        iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_UPSERT;
    strcpy(interface->logical_name, "lan");
    strcpy(interface->device, "eth0");
    strcpy(interface->previous_logical_name, "old");
    request.rollback_timeout_sec = 30U;

    uint8_t encoded[128];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    require(pb_encode(&stream, iot_edge_v1_NetworkConfigRequest_fields, &request),
            PB_GET_ERROR(&stream));
    const uint8_t cpp_protobuf_wire[] = {
        0x0a, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x12, 0x1a, 0x0a, 0x04,
        0x65, 0x74, 0x68, 0x30, 0x10, 0x01, 0x48, 0x01, 0x52, 0x03, 0x6c,
        0x61, 0x6e, 0x5a, 0x04, 0x65, 0x74, 0x68, 0x30, 0x62, 0x03, 0x6f,
        0x6c, 0x64, 0x18, 0x1e,
    };
    require(stream.bytes_written == sizeof(cpp_protobuf_wire) &&
                memcmp(encoded, cpp_protobuf_wire, sizeof(cpp_protobuf_wire)) == 0,
            "nanopb wire differs from the C++ Protobuf golden vector");
}

static void test_cpp_protobuf_config_digest_contract(void) {
    iot_edge_v1_ConfigItem item = iot_edge_v1_ConfigItem_init_zero;
    item.revision = 7U;
    item.kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_ENDPOINT;
    item.which_item = iot_edge_v1_ConfigItem_endpoint_tag;
    const uint8_t id[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    iot_edge_v1_EndpointConfig *endpoint = &item.item.endpoint;
    require(edge_protocol_set_bytes(&endpoint->endpoint_id,
                                    sizeof(endpoint->endpoint_id.bytes), id, sizeof(id)),
            "config endpoint id setup failed");
    strcpy(endpoint->name, "x");
    endpoint->transport = iot_edge_v1_Transport_TRANSPORT_ETHERNET;
    endpoint->mode = iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT;
    endpoint->protocol = iot_edge_v1_Protocol_PROTOCOL_MODBUS;
    strcpy(endpoint->ip, "1.2.3.4");
    endpoint->port = 502U;
    endpoint->enabled = true;
    strcpy(endpoint->interface_name, "eth0");

    uint8_t encoded[128];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    require(pb_encode(&stream, iot_edge_v1_ConfigItem_fields, &item),
            PB_GET_ERROR(&stream));
    const uint8_t cpp_protobuf_wire[] = {
        0x08, 0x07, 0x18, 0x01, 0x52, 0x2f, 0x0a, 0x10, 0x00, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x12, 0x01, 0x78, 0x18, 0x01, 0x20, 0x02, 0x28, 0x02,
        0x32, 0x07, 0x31, 0x2e, 0x32, 0x2e, 0x33, 0x2e, 0x34, 0x38, 0xf6,
        0x03, 0x48, 0x01, 0x52, 0x04, 0x65, 0x74, 0x68, 0x30,
    };
    require(stream.bytes_written == sizeof(cpp_protobuf_wire) &&
                memcmp(encoded, cpp_protobuf_wire, sizeof(cpp_protobuf_wire)) == 0,
            "config item wire differs from the C++ Protobuf digest contract");
}

static void test_reject_text_or_oversized_input(void) {
    iot_edge_v1_Envelope decoded;
    const char *error = NULL;
    const uint8_t json[] = "{\"type\":\"hello\"}";
    require(!edge_protocol_decode(json, sizeof(json) - 1U, &decoded, &error),
            "JSON WebSocket body was accepted as protobuf");
    require(!edge_protocol_decode(json, EDGENODE_MAX_WS_MESSAGE + 1U, &decoded, &error),
            "oversized WebSocket body was accepted");
}

int main(void) {
    test_imei();
    test_hello_round_trip();
    test_heartbeat_mobile_state_round_trip();
    test_modem_profile_round_trip();
    test_cpp_protobuf_wire_contract();
    test_cpp_protobuf_config_digest_contract();
    test_reject_text_or_oversized_input();
    puts("edge protocol tests passed");
    return 0;
}
