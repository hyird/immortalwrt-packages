#include "edge_network.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <uci.h>

#include "edge_process.h"

#define NETWORK_BACKUP "/tmp/edgenode/network.backup"
#define NETWORK_PENDING "/tmp/edgenode/network.pending"
#define NETWORK_CONFIRMED "/tmp/edgenode/network.confirmed"
#define NETWORK_ROLLED_BACK "/tmp/edgenode/network.rolled-back"

static void restore_network(void);

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message != NULL ? message : "network error");
}

static bool parse_ipv4(const char *text, uint32_t *value) {
    struct in_addr address;
    if (text == NULL || inet_pton(AF_INET, text, &address) != 1)
        return false;
    *value = ntohl(address.s_addr);
    return true;
}

bool edge_network_validate_static(const char *ip, uint32_t prefix_length,
                                  const char *gateway, char *error, size_t error_size) {
    uint32_t address = 0U;
    if (!parse_ipv4(ip, &address)) {
        set_error(error, error_size, "static IPv4 address is invalid");
        return false;
    }
    if (prefix_length == 0U || prefix_length > 30U) {
        set_error(error, error_size, "static IPv4 prefix must be between 1 and 30");
        return false;
    }
    const uint32_t mask = 0xFFFFFFFFU << (32U - prefix_length);
    const uint32_t host = address & ~mask;
    if (host == 0U || host == ~mask) {
        set_error(error, error_size, "static IPv4 cannot be network or broadcast address");
        return false;
    }
    if (gateway != NULL && gateway[0] != '\0') {
        uint32_t gateway_value = 0U;
        if (!parse_ipv4(gateway, &gateway_value) ||
            (gateway_value & mask) != (address & mask) || gateway_value == address) {
            set_error(error, error_size, "gateway must be a different host in the subnet");
            return false;
        }
        const uint32_t gateway_host = gateway_value & ~mask;
        if (gateway_host == 0U || gateway_host == ~mask) {
            set_error(error, error_size, "gateway cannot be network or broadcast address");
            return false;
        }
    }
    return true;
}

static bool valid_section_name(const char *value) {
    if (value == NULL || value[0] == '\0' || strlen(value) > 15U)
        return false;
    for (; *value != '\0'; ++value)
        if (!isalnum((unsigned char)*value) && *value != '_')
            return false;
    return true;
}

static bool valid_device_name(const char *value) {
    if (value == NULL || value[0] == '\0' || strlen(value) > 32U)
        return false;
    for (; *value != '\0'; ++value)
        if (!isalnum((unsigned char)*value) && *value != '_' && *value != '-' &&
            *value != '.' && *value != ':')
            return false;
    return true;
}

static bool protected_device(const char *name, const char *protected_name) {
    if (name == NULL || protected_name == NULL || protected_name[0] == '\0')
        return false;
    const size_t length = strlen(protected_name);
    return strcmp(name, protected_name) == 0 ||
           (strncmp(name, protected_name, length) == 0 &&
            (name[length] == '.' || name[length] == ':'));
}

static bool request_modifies(const iot_edge_v1_NetworkConfigRequest *request,
                             const char *logical_name) {
    for (pb_size_t index = 0; index < request->interfaces_count; ++index) {
        const iot_edge_v1_NetworkInterfaceConfig *config = &request->interfaces[index];
        if (strcmp(config->logical_name, logical_name) == 0 ||
            (config->previous_logical_name[0] != '\0' &&
             strcmp(config->previous_logical_name, logical_name) == 0))
            return true;
    }
    return false;
}

static bool request_uses_device(const iot_edge_v1_NetworkConfigRequest *request,
                                const char *device) {
    for (pb_size_t index = 0; index < request->interfaces_count; ++index) {
        const iot_edge_v1_NetworkInterfaceConfig *config = &request->interfaces[index];
        if (config->operation !=
            iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_UPSERT)
            continue;
        if (!config->bridge && strcmp(config->device, device) == 0)
            return true;
        for (pb_size_t port = 0; port < config->bridge_ports_count; ++port)
            if (strcmp(config->bridge_ports[port], device) == 0)
                return true;
    }
    return false;
}

static bool validate_devices(const iot_edge_v1_NetworkConfigRequest *request,
                             const char *protected_name, char *error, size_t error_size) {
    char selected[64][33] = {{0}};
    size_t selected_count = 0U;
    for (pb_size_t index = 0; index < request->interfaces_count; ++index) {
        const iot_edge_v1_NetworkInterfaceConfig *config = &request->interfaces[index];
        if (config->operation !=
            iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_UPSERT)
            continue;
        const pb_size_t count = config->bridge ? config->bridge_ports_count : 1U;
        for (pb_size_t item = 0; item < count; ++item) {
            const char *device =
                config->bridge ? config->bridge_ports[item] : config->device;
            if (!valid_device_name(device) || protected_device(device, protected_name)) {
                set_error(error, error_size,
                          "network request contains an invalid or protected 4G device");
                return false;
            }
            if (if_nametoindex(device) == 0U) {
                set_error(error, error_size, "network request contains an unknown device");
                return false;
            }
            for (size_t previous = 0; previous < selected_count; ++previous)
                if (strcmp(selected[previous], device) == 0) {
                    set_error(error, error_size,
                              "one device cannot be assigned to multiple interfaces");
                    return false;
                }
            snprintf(selected[selected_count++], sizeof(selected[0]), "%s", device);
        }
    }
    return true;
}

static size_t split_devices(const char *value, char output[][33], size_t capacity,
                            size_t count) {
    if (value == NULL)
        return count;
    while (*value != '\0' && count < capacity) {
        while (isspace((unsigned char)*value))
            ++value;
        if (*value == '\0')
            break;
        const char *end = value;
        while (*end != '\0' && !isspace((unsigned char)*end))
            ++end;
        const size_t length = (size_t)(end - value);
        if (length != 0U && length < 33U) {
            memcpy(output[count], value, length);
            output[count][length] = '\0';
            ++count;
        }
        value = end;
    }
    return count;
}

static size_t option_devices(struct uci_context *context, struct uci_section *section,
                             const char *name, char output[][33], size_t capacity,
                             size_t count) {
    struct uci_option *option = uci_lookup_option(context, section, name);
    if (option == NULL)
        return count;
    if (option->type == UCI_TYPE_STRING)
        return split_devices(option->v.string, output, capacity, count);
    if (option->type == UCI_TYPE_LIST) {
        struct uci_element *element;
        uci_foreach_element(&option->v.list, element) {
            count = split_devices(element->name, output, capacity, count);
            if (count == capacity)
                break;
        }
    }
    return count;
}

static bool validate_existing_ownership(
    const iot_edge_v1_NetworkConfigRequest *request, const char *protected_name,
    char *error, size_t error_size) {
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "network", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        set_error(error, error_size, "cannot read UCI network configuration");
        return false;
    }
    bool valid = true;
    struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        if (strcmp(section->type, "interface") != 0)
            continue;
        char devices[8][33] = {{0}};
        size_t count = option_devices(context, section, "ifname", devices, 8U, 0U);
        if (count == 0U)
            count = option_devices(context, section, "device", devices, 8U, 0U);
        bool owns_protected_device = false;
        for (size_t index = 0; index < count; ++index)
            if (protected_device(devices[index], protected_name))
                owns_protected_device = true;
        if (request_modifies(request, element->name)) {
            if (owns_protected_device) {
                set_error(error, error_size,
                          "logical interface owns the protected 4G uplink");
                valid = false;
            }
            continue;
        }
        for (size_t index = 0; index < count; ++index)
            if (request_uses_device(request, devices[index])) {
                set_error(error, error_size,
                          "device is already owned by another UCI logical interface");
                valid = false;
                break;
            }
        if (!valid)
            break;
    }
    uci_unload(context, package);
    uci_free_context(context);
    return valid;
}

static bool validate_request(const iot_edge_v1_NetworkConfigRequest *request,
                             const char *protected_name, char *error, size_t error_size) {
    if (request == NULL || request->request_id.size != 16U ||
        request->interfaces_count == 0U || request->interfaces_count > 8U) {
        set_error(error, error_size,
                  "network request must contain between one and eight interfaces");
        return false;
    }
    if (request->rollback_timeout_sec < 30U ||
        request->rollback_timeout_sec > 300U) {
        set_error(error, error_size, "rollback timeout must be between 30 and 300 seconds");
        return false;
    }
    for (pb_size_t index = 0; index < request->interfaces_count; ++index) {
        const iot_edge_v1_NetworkInterfaceConfig *config = &request->interfaces[index];
        if (!valid_section_name(config->logical_name) ||
            strcmp(config->logical_name, "loopback") == 0) {
            set_error(error, error_size, "UCI logical interface name is invalid");
            return false;
        }
        if (config->previous_logical_name[0] != '\0' &&
            (config->operation !=
                 iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_UPSERT ||
             !valid_section_name(config->previous_logical_name) ||
             strcmp(config->previous_logical_name, "loopback") == 0 ||
             strcmp(config->previous_logical_name, config->logical_name) == 0)) {
            set_error(error, error_size, "previous UCI logical interface name is invalid");
            return false;
        }
        for (pb_size_t previous = 0; previous < index; ++previous)
            if (strcmp(request->interfaces[previous].logical_name,
                       config->logical_name) == 0) {
                set_error(error, error_size, "logical interface is duplicated");
                return false;
            }
        for (pb_size_t previous = 0; previous < index; ++previous)
            if (config->previous_logical_name[0] != '\0' &&
                strcmp(request->interfaces[previous].previous_logical_name,
                       config->previous_logical_name) == 0) {
                set_error(error, error_size, "previous logical interface is duplicated");
                return false;
            }
        if (config->operation ==
            iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_DELETE)
            continue;
        if (config->operation !=
            iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_UPSERT) {
            set_error(error, error_size, "network operation is invalid");
            return false;
        }
        if (config->mode !=
                iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_DHCP &&
            config->mode !=
                iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC) {
            set_error(error, error_size, "address mode must be DHCP or static");
            return false;
        }
        if (config->bridge) {
            if (strlen(config->logical_name) > 12U || config->device[0] != '\0' ||
                config->bridge_ports_count == 0U ||
                config->bridge_ports_count > 8U) {
                set_error(error, error_size, "bridge interface or member list is invalid");
                return false;
            }
        } else if (config->device[0] == '\0' ||
                   config->bridge_ports_count != 0U) {
            set_error(error, error_size, "non-bridge interface must use one device");
            return false;
        }
        if (config->mode ==
            iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC) {
            if (!edge_network_validate_static(config->ip, config->prefix_length,
                                              config->gateway, error, error_size))
                return false;
        } else if (config->ip[0] != '\0' || config->prefix_length != 0U ||
                   config->gateway[0] != '\0') {
            set_error(error, error_size, "DHCP interface cannot contain a static address");
            return false;
        }
    }
    return validate_devices(request, protected_name, error, error_size) &&
           validate_existing_ownership(request, protected_name, error, error_size);
}

static bool run_uci(const char *operation, const char *argument) {
    const char *argv[5] = {"uci", NULL, NULL, NULL, NULL};
    size_t index = 1U;
    if (strcmp(operation, "delete") == 0)
        argv[index++] = "-q";
    argv[index++] = operation;
    if (argument != NULL)
        argv[index++] = argument;
    return edge_process_run(argv, -1, -1) == 0;
}

static bool backup_network(void) {
    const int output = open(NETWORK_BACKUP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0)
        return false;
    const char *argv[] = {"uci", "export", "network", NULL};
    const int result = edge_process_run(argv, -1, output);
    close(output);
    return result == 0;
}

static void prefix_to_mask(uint32_t prefix, char output[16]) {
    const uint32_t mask = htonl(0xFFFFFFFFU << (32U - prefix));
    (void)inet_ntop(AF_INET, &mask, output, 16U);
}

static bool set_option(const char *section, const char *option, const char *value) {
    char argument[384];
    snprintf(argument, sizeof(argument), "network.%s.%s=%s", section, option, value);
    return run_uci("set", argument);
}

static bool delete_option(const char *section, const char *option) {
    char argument[96];
    snprintf(argument, sizeof(argument), "network.%s.%s", section, option);
    (void)run_uci("delete", argument);
    return true;
}

static bool apply_interface(const iot_edge_v1_NetworkInterfaceConfig *config) {
    char section[64];
    snprintf(section, sizeof(section), "network.%s", config->logical_name);
    if (config->operation ==
        iot_edge_v1_NetworkConfigOperation_NETWORK_CONFIG_DELETE) {
        (void)run_uci("delete", section);
        return true;
    }

    if (config->previous_logical_name[0] != '\0') {
        char rename[96];
        snprintf(rename, sizeof(rename), "network.%s=%s",
                 config->previous_logical_name, config->logical_name);
        if (!run_uci("rename", rename))
            return false;
    }

    char section_type[80];
    snprintf(section_type, sizeof(section_type), "%s=interface", section);
    const char *protocol =
        config->mode == iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC
            ? "static"
            : "dhcp";
    bool applied = run_uci("set", section_type) &&
                   set_option(config->logical_name, "proto", protocol);
    if (config->bridge) {
        char members[272] = {0};
        size_t offset = 0U;
        for (pb_size_t index = 0; index < config->bridge_ports_count; ++index) {
            const int written =
                snprintf(members + offset, sizeof(members) - offset, "%s%s",
                         index == 0U ? "" : " ", config->bridge_ports[index]);
            if (written < 0 || (size_t)written >= sizeof(members) - offset)
                return false;
            offset += (size_t)written;
        }
        applied = applied && set_option(config->logical_name, "type", "bridge") &&
                  set_option(config->logical_name, "ifname", members) &&
                  delete_option(config->logical_name, "device");
    } else {
        applied = applied && delete_option(config->logical_name, "type") &&
                  set_option(config->logical_name, "ifname", config->device) &&
                  delete_option(config->logical_name, "device");
    }
    if (config->mode ==
        iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC) {
        char netmask[16];
        prefix_to_mask(config->prefix_length, netmask);
        applied = applied && set_option(config->logical_name, "ipaddr", config->ip) &&
                  set_option(config->logical_name, "netmask", netmask);
        applied = applied &&
                  (config->gateway[0] != '\0'
                       ? set_option(config->logical_name, "gateway", config->gateway)
                       : delete_option(config->logical_name, "gateway"));
    } else {
        applied = applied && delete_option(config->logical_name, "ipaddr") &&
                  delete_option(config->logical_name, "netmask") &&
                  delete_option(config->logical_name, "gateway");
    }
    return applied;
}

bool edge_network_prepare(const iot_edge_v1_NetworkConfigRequest *request,
                          const char *protected_device_name, char *error,
                          size_t error_size) {
    if (access(NETWORK_PENDING, F_OK) == 0) {
        set_error(error, error_size, "another network change is waiting for confirmation");
        return false;
    }
    if (!validate_request(request, protected_device_name, error, error_size))
        return false;
    if (!backup_network()) {
        set_error(error, error_size, "cannot back up UCI network configuration");
        return false;
    }
    bool applied = true;
    for (pb_size_t index = 0; index < request->interfaces_count && applied; ++index)
        applied = apply_interface(&request->interfaces[index]);
    applied = applied && run_uci("commit", "network");
    if (!applied) {
        restore_network();
        unlink(NETWORK_BACKUP);
        set_error(error, error_size, "UCI rejected the network configuration");
        return false;
    }
    return true;
}

static void restore_network(void) {
    const int input = open(NETWORK_BACKUP, O_RDONLY);
    if (input >= 0) {
        const char *import[] = {"uci", "import", "network", NULL};
        if (edge_process_run(import, input, -1) == 0)
            (void)run_uci("commit", "network");
        close(input);
    }
    const char *reload[] = {"/etc/init.d/network", "reload", NULL};
    (void)edge_process_run(reload, -1, -1);
}

static bool write_marker(const char *path, const uint8_t request_id[16]) {
    const int output = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0)
        return false;
    const bool written = write(output, request_id, 16U) == 16;
    close(output);
    return written;
}

bool edge_network_activate(uint32_t rollback_timeout_sec,
                           const uint8_t request_id[16]) {
    if (rollback_timeout_sec < 30U || rollback_timeout_sec > 300U ||
        request_id == NULL)
        return false;
    unlink(NETWORK_CONFIRMED);
    if (!write_marker(NETWORK_PENDING, request_id)) {
        restore_network();
        unlink(NETWORK_BACKUP);
        return false;
    }
    const int worker = edge_process_detach();
    if (worker < 0) {
        restore_network();
        unlink(NETWORK_PENDING);
        unlink(NETWORK_BACKUP);
        return false;
    }
    if (worker == 0) {
        edge_process_close_inherited_fds(-1);
        sleep(1U);
        const char *reload[] = {"/etc/init.d/network", "reload", NULL};
        (void)edge_process_run(reload, -1, -1);
        bool confirmed = false;
        for (uint32_t elapsed = 0U; elapsed < rollback_timeout_sec; ++elapsed) {
            sleep(1U);
            if (access(NETWORK_CONFIRMED, F_OK) == 0) {
                confirmed = true;
                break;
            }
        }
        if (!confirmed) {
            restore_network();
            unlink(NETWORK_PENDING);
            unlink(NETWORK_BACKUP);
            (void)write_marker(NETWORK_ROLLED_BACK, request_id);
        } else {
            unlink(NETWORK_PENDING);
            unlink(NETWORK_BACKUP);
        }
        unlink(NETWORK_CONFIRMED);
        _exit(0);
    }
    return true;
}

void edge_network_confirm(void) {
    if (access(NETWORK_PENDING, F_OK) != 0)
        return;
    const int marker =
        open(NETWORK_CONFIRMED, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (marker >= 0)
        close(marker);
}

bool edge_network_take_rollback(uint8_t request_id[16]) {
    if (request_id == NULL)
        return false;
    const int input = open(NETWORK_ROLLED_BACK, O_RDONLY);
    if (input < 0)
        return false;
    const bool read_ok = read(input, request_id, 16U) == 16;
    close(input);
    if (read_ok)
        unlink(NETWORK_ROLLED_BACK);
    return read_ok;
}
