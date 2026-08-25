#include "edge_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uci.h>

#include "edge_protocol.h"

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0U)
        snprintf(error, size, "%s", message);
}

static int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

bool edge_config_parse_uuid(const char *text, uint8_t output[16]) {
    static const size_t hyphens[] = {8U, 13U, 18U, 23U};
    if (text == NULL || output == NULL || strlen(text) != 36U)
        return false;
    for (size_t index = 0; index < sizeof(hyphens) / sizeof(hyphens[0]); ++index)
        if (text[hyphens[index]] != '-')
            return false;
    size_t byte = 0U;
    for (size_t index = 0U; index < 36U;) {
        if (text[index] == '-') {
            ++index;
            continue;
        }
        if (index + 1U >= 36U || byte >= 16U)
            return false;
        const int high = hex_digit(text[index]);
        const int low = hex_digit(text[index + 1U]);
        if (high < 0 || low < 0)
            return false;
        output[byte++] = (uint8_t)((high << 4U) | low);
        index += 2U;
    }
    return byte == 16U;
}

void edge_config_format_uuid(const uint8_t value[16], char output[37]) {
    static const char digits[] = "0123456789abcdef";
    size_t out = 0U;
    for (size_t index = 0; index < 16U; ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U)
            output[out++] = '-';
        output[out++] = digits[value[index] >> 4U];
        output[out++] = digits[value[index] & 0x0fU];
    }
    output[out] = '\0';
}

static bool copy_option(struct uci_context *context, struct uci_section *section,
                        const char *name, char *output, size_t capacity,
                        bool required, char *error, size_t error_size) {
    const char *value = uci_lookup_option_string(context, section, name);
    if (value == NULL || value[0] == '\0') {
        if (required) {
            char message[128];
            snprintf(message, sizeof(message), "missing UCI option: %s", name);
            set_error(error, error_size, message);
            return false;
        }
        /* Preserve caller defaults for optional values. Zero-initialized outputs stay empty. */
        return true;
    }
    if (strlen(value) >= capacity) {
        char message[128];
        snprintf(message, sizeof(message), "UCI option too long: %s", name);
        set_error(error, error_size, message);
        return false;
    }
    memcpy(output, value, strlen(value) + 1U);
    return true;
}

static bool bool_option(struct uci_context *context, struct uci_section *section,
                        const char *name, bool fallback) {
    const char *value = uci_lookup_option_string(context, section, name);
    if (value == NULL)
        return fallback;
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
           strcmp(value, "yes") == 0 || strcmp(value, "on") == 0;
}

static unsigned long number_option(struct uci_context *context, struct uci_section *section,
                                   const char *name, unsigned long fallback,
                                   unsigned long minimum, unsigned long maximum) {
    const char *value = uci_lookup_option_string(context, section, name);
    if (value == NULL || value[0] == '\0')
        return fallback;
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum)
        return fallback;
    return parsed;
}

static bool duplicate_platform(const edge_app_config *config, const uint8_t id[16]) {
    for (size_t index = 0; index < config->platform_count; ++index)
        if (memcmp(config->platforms[index].id, id, 16U) == 0)
            return true;
    return false;
}

bool edge_config_valid_platform_url(const char *value) {
    if (value == NULL)
        return false;
    if (strncmp(value, "https://", 8U) != 0)
        return false;
    const char *host = value + 8U;
    if (host[0] == '\0' || host[0] == '/')
        return false;
    bool trailing_slashes = false;
    for (const unsigned char *cursor = (const unsigned char *)host;
         *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            trailing_slashes = true;
            continue;
        }
        if (trailing_slashes || *cursor <= 0x20U || *cursor == 0x7fU ||
            *cursor == '@' || *cursor == '?' || *cursor == '#' || *cursor == '\\')
            return false;
    }
    return true;
}

bool edge_config_load(edge_app_config *config, char *error, size_t error_size) {
    if (config == NULL) {
        set_error(error, error_size, "config output is null");
        return false;
    }
    memset(config, 0, sizeof(*config));
    config->heartbeat_interval_sec = 30U;
    uint8_t bootstrap_id[16];
    if (!edge_config_parse_uuid(EDGE_BOOTSTRAP_PLATFORM_ID, bootstrap_id)) {
        set_error(error, error_size, "invalid default platform id");
        return false;
    }

    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "edgenode", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        set_error(error, error_size, "cannot load /etc/config/edgenode");
        return false;
    }

    bool have_node = false;
    bool have_hardware = false;
    bool have_modem = false;
    struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        if (strcmp(section->type, "node") == 0) {
            if (have_node ||
                !copy_option(context, section, "imei", config->imei, sizeof(config->imei),
                             true, error, error_size) ||
                !copy_option(context, section, "model", config->model, sizeof(config->model),
                             true, error, error_size))
                goto fail;
            if (!edge_protocol_validate_imei(config->imei)) {
                set_error(error, error_size, "node IMEI must be 15 digits with valid Luhn check");
                goto fail;
            }
            config->heartbeat_interval_sec = (uint16_t)number_option(
                context, section, "heartbeat_interval_sec", 30U, 5U, 3600U);
            have_node = true;
            continue;
        }
        if (strcmp(section->type, "hardware") == 0) {
            if (have_hardware ||
                !copy_option(context, section, "serial_port", config->serial_port,
                             sizeof(config->serial_port), true, error, error_size) ||
                !copy_option(context, section, "lan_interface", config->lan_interface,
                             sizeof(config->lan_interface), true, error, error_size) ||
                !copy_option(context, section, "wan_interface", config->wan_interface,
                             sizeof(config->wan_interface), true, error, error_size))
                goto fail;
            config->bridge = bool_option(context, section, "bridge", false);
            config->serial_rs485 = bool_option(context, section, "serial_rs485", false);
            have_hardware = true;
            continue;
        }
        if (strcmp(section->type, "modem") == 0) {
            if (have_modem ||
                !copy_option(context, section, "at_port", config->modem_at_port,
                             sizeof(config->modem_at_port), true, error, error_size) ||
                !copy_option(context, section, "status_path", config->modem_status_path,
                             sizeof(config->modem_status_path), true, error, error_size))
                goto fail;
            have_modem = true;
            continue;
        }
        if (strcmp(section->type, "platform") != 0 ||
            !bool_option(context, section, "enabled", true))
            continue;
        if (config->platform_count >= EDGE_MAX_PLATFORMS) {
            set_error(error, error_size, "too many enabled platform sections");
            goto fail;
        }
        char id[37];
        uint8_t parsed_id[16];
        if (!copy_option(context, section, "id", id, sizeof(id), true, error, error_size) ||
            !edge_config_parse_uuid(id, parsed_id)) {
            set_error(error, error_size, "platform id must be a UUID");
            goto fail;
        }
        edge_platform_config *platform = &config->platforms[config->platform_count];
        memcpy(platform->id, parsed_id, sizeof(platform->id));
        if (!copy_option(context, section, "url", platform->url, sizeof(platform->url),
                         true, error, error_size))
            goto fail;
        if (duplicate_platform(config, platform->id)) {
            set_error(error, error_size, "platform id must be unique");
            goto fail;
        }
        if (!edge_config_valid_platform_url(platform->url)) {
            set_error(error, error_size,
                      "platform url must be an https base URL without a path");
            goto fail;
        }
        snprintf(platform->name, sizeof(platform->name), "%s", section->e.name);
        if (!copy_option(context, section, "name", platform->name,
                         sizeof(platform->name), false, error, error_size))
            goto fail;
        platform->enabled = true;
        platform->bootstrap =
            memcmp(platform->id, bootstrap_id, sizeof(bootstrap_id)) == 0;
        platform->network_owner = platform->bootstrap;
        platform->priority = (uint16_t)number_option(context, section, "priority", 100U, 0U, 65535U);
        platform->reconnect_interval_sec = (uint16_t)number_option(
            context, section, "reconnect_interval_sec", 5U, 1U, 3600U);
        platform->outbox_max_bytes = (uint32_t)number_option(
            context, section, "outbox_max_bytes", 256U * 1024U,
            16U * 1024U, 8U * 1024U * 1024U);
        ++config->platform_count;
    }

    if (!have_node) {
        set_error(error, error_size, "one node section is required");
        goto fail;
    }
    uci_unload(context, package);
    uci_free_context(context);
    return true;

fail:
    uci_unload(context, package);
    uci_free_context(context);
    return false;
}
