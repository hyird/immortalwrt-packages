#include "edge_capability.h"

#include "edge_interface.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <uci.h>

static void safe_copy(char *output, size_t capacity, const char *input) {
    if (capacity != 0U)
        snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static bool protected_device(const char *name, const char *protected_name) {
    if (name == NULL || protected_name == NULL || protected_name[0] == '\0')
        return false;
    const size_t length = strlen(protected_name);
    return strcmp(name, protected_name) == 0 ||
           (strncmp(name, protected_name, length) == 0 &&
            (name[length] == '.' || name[length] == ':'));
}

static uint32_t prefix_length(struct in_addr mask) {
    uint32_t value = ntohl(mask.s_addr);
    uint32_t prefix = 0U;
    while ((value & 0x80000000U) != 0U) {
        ++prefix;
        value <<= 1U;
    }
    return value == 0U ? prefix : 0U;
}

static void read_default_gateway(const char *name, char output[16]) {
    FILE *input = fopen("/proc/net/route", "r");
    if (input == NULL)
        return;
    char line[256];
    while (fgets(line, sizeof(line), input) != NULL) {
        char device[33] = {0};
        unsigned long destination = 0UL;
        unsigned long gateway = 0UL;
        unsigned long flags = 0UL;
        if (sscanf(line, "%32s %lx %lx %lx", device, &destination, &gateway, &flags) != 4 ||
            strcmp(device, name) != 0 || destination != 0UL || (flags & 0x2UL) == 0UL)
            continue;
        struct in_addr address;
        address.s_addr = (in_addr_t)gateway;
        (void)inet_ntop(AF_INET, &address, output, 16U);
        break;
    }
    fclose(input);
}

static void read_bridge_ports(const char *name, iot_edge_v1_InterfaceCapability *output) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/net/%s/brif", name);
    DIR *directory = opendir(path);
    if (directory == NULL)
        return;
    output->bridge = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           output->bridge_ports_count <
               sizeof(output->bridge_ports) / sizeof(output->bridge_ports[0])) {
        if (entry->d_name[0] == '.')
            continue;
        safe_copy(output->bridge_ports[output->bridge_ports_count],
                  sizeof(output->bridge_ports[output->bridge_ports_count]), entry->d_name);
        ++output->bridge_ports_count;
    }
    closedir(directory);
}

static void read_interface(const char *name, iot_edge_v1_InterfaceCapability *output) {
    safe_copy(output->name, sizeof(output->name), name);
    safe_copy(output->display_name, sizeof(output->display_name), name);
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd >= 0) {
        struct ifreq request;
        memset(&request, 0, sizeof(request));
        safe_copy(request.ifr_name, sizeof(request.ifr_name), name);
        if (ioctl(socket_fd, SIOCGIFFLAGS, &request) == 0)
            output->up = (request.ifr_flags & IFF_UP) != 0;
#ifdef SIOCGIFHWADDR
        safe_copy(request.ifr_name, sizeof(request.ifr_name), name);
        if (ioctl(socket_fd, SIOCGIFHWADDR, &request) == 0) {
            output->mac.size = 6U;
            memcpy(output->mac.bytes, request.ifr_hwaddr.sa_data, 6U);
        }
#endif
        safe_copy(request.ifr_name, sizeof(request.ifr_name), name);
        if (ioctl(socket_fd, SIOCGIFADDR, &request) == 0) {
            const struct sockaddr_in *address =
                (const struct sockaddr_in *)&request.ifr_addr;
            (void)inet_ntop(AF_INET, &address->sin_addr, output->ipv4,
                            sizeof(output->ipv4));
        }
        safe_copy(request.ifr_name, sizeof(request.ifr_name), name);
        if (ioctl(socket_fd, SIOCGIFNETMASK, &request) == 0) {
            const struct sockaddr_in *mask =
                (const struct sockaddr_in *)&request.ifr_netmask;
            output->prefix_length = prefix_length(mask->sin_addr);
        }
        close(socket_fd);
    }
    read_default_gateway(name, output->gateway);
    read_bridge_ports(name, output);
}

static iot_edge_v1_InterfaceCapability *
find_interface(iot_edge_v1_CapabilityReport *report, const char *name) {
    for (pb_size_t index = 0; index < report->interfaces_count; ++index)
        if (strcmp(report->interfaces[index].name, name) == 0)
            return &report->interfaces[index];
    return NULL;
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

static iot_edge_v1_NetworkAddressMode address_mode(const char *value) {
    if (value != NULL && strcmp(value, "dhcp") == 0)
        return iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_DHCP;
    if (value != NULL && strcmp(value, "static") == 0)
        return iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC;
    return iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_NONE;
}

static void copy_runtime(iot_edge_v1_NetworkCapability *network,
                         const iot_edge_v1_InterfaceCapability *runtime) {
    if (runtime == NULL)
        return;
    network->up = runtime->up;
    safe_copy(network->ipv4, sizeof(network->ipv4), runtime->ipv4);
    network->prefix_length = runtime->prefix_length;
    safe_copy(network->gateway, sizeof(network->gateway), runtime->gateway);
}

static void collect_logical_networks(iot_edge_v1_CapabilityReport *report,
                                     const char *excluded) {
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "network", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        return;
    }
    struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        if (report->networks_count >=
                sizeof(report->networks) / sizeof(report->networks[0]) ||
            strcmp(element->name, "loopback") == 0)
            continue;
        struct uci_section *section = uci_to_section(element);
        if (strcmp(section->type, "interface") != 0)
            continue;
        const char *protocol = uci_lookup_option_string(context, section, "proto");
        const iot_edge_v1_NetworkAddressMode mode = address_mode(protocol);
        if (mode == iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_NONE)
            continue;
        const char *type = uci_lookup_option_string(context, section, "type");
        const bool bridge = type != NULL && strcmp(type, "bridge") == 0;
        char devices[8][33] = {{0}};
        size_t device_count = option_devices(context, section, "ifname", devices, 8U, 0U);
        if (device_count == 0U)
            device_count = option_devices(context, section, "device", devices, 8U, 0U);
        bool skip = device_count == 0U;
        for (size_t index = 0; index < device_count; ++index)
            if (protected_device(devices[index], excluded))
                skip = true;
        if (skip)
            continue;

        iot_edge_v1_NetworkCapability *network =
            &report->networks[report->networks_count++];
        safe_copy(network->name, sizeof(network->name), element->name);
        network->mode = mode;
        network->bridge = bridge;
        char runtime_name[33] = {0};
        if (bridge) {
            snprintf(runtime_name, sizeof(runtime_name), "br-%s", element->name);
            safe_copy(network->device, sizeof(network->device), runtime_name);
            for (size_t index = 0; index < device_count; ++index) {
                safe_copy(network->bridge_ports[network->bridge_ports_count],
                          sizeof(network->bridge_ports[network->bridge_ports_count]),
                          devices[index]);
                ++network->bridge_ports_count;
            }
        } else {
            safe_copy(runtime_name, sizeof(runtime_name), devices[0]);
            safe_copy(network->device, sizeof(network->device), devices[0]);
        }
        copy_runtime(network, find_interface(report, runtime_name));
        if (network->ipv4[0] == '\0' && mode == iot_edge_v1_NetworkAddressMode_NETWORK_ADDRESS_STATIC) {
            const char *ip = uci_lookup_option_string(context, section, "ipaddr");
            const char *netmask = uci_lookup_option_string(context, section, "netmask");
            const char *gateway = uci_lookup_option_string(context, section, "gateway");
            safe_copy(network->ipv4, sizeof(network->ipv4), ip);
            safe_copy(network->gateway, sizeof(network->gateway), gateway);
            struct in_addr mask;
            if (netmask != NULL && inet_pton(AF_INET, netmask, &mask) == 1)
                network->prefix_length = prefix_length(mask);
        }
    }
    uci_unload(context, package);
    uci_free_context(context);
}

bool edge_capability_has_ttyd(void) {
    static const char *paths[] = {"/usr/bin/ttyd", "/usr/sbin/ttyd", "/bin/ttyd"};
    for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index)
        if (access(paths[index], X_OK) == 0)
            return true;
    return false;
}

bool edge_capability_collect_network(iot_edge_v1_CapabilityReport *report,
                                     const char *protected_name) {
    if (report == NULL)
        return false;
    DIR *directory = opendir("/sys/class/net");
    if (directory == NULL)
        return false;
    char interface_names[sizeof(report->interfaces) / sizeof(report->interfaces[0])]
                        [EDGE_INTERFACE_NAME_CAPACITY] = {{0}};
    size_t interface_count = 0U;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           interface_count < sizeof(interface_names) / sizeof(interface_names[0])) {
        if (entry->d_name[0] == '.' || strcmp(entry->d_name, "lo") == 0 ||
            strlen(entry->d_name) > 32U ||
            protected_device(entry->d_name, protected_name))
            continue;
        safe_copy(interface_names[interface_count], sizeof(interface_names[interface_count]),
                  entry->d_name);
        ++interface_count;
    }
    closedir(directory);
    for (size_t index = 0; index < interface_count; ++index) {
        if (edge_interface_has_subinterface(interface_names[index], interface_names,
                                            interface_count))
            continue;
        read_interface(interface_names[index], &report->interfaces[report->interfaces_count]);
        ++report->interfaces_count;
    }
    collect_logical_networks(report, protected_name);
    return true;
}
