#include "edge_modem.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <uci.h>

#include "edge_protocol.h"

#define EDGE_MODEM_RESPONSE_CAPACITY 4096U
#define EDGE_MODEM_TIMEOUT_MS 5000
#define EDGE_MODEM_LOCK "/tmp/edgenode/modem.lock"

static volatile sig_atomic_t monitor_stop;

static void initialize_info(edge_modem_info *info) {
    memset(info, 0, sizeof(*info));
    info->registration_status = -1;
    info->csq = 99;
    info->rssi_dbm = -1;
    info->sim_state = iot_edge_v1_ModemSimState_MODEM_SIM_UNKNOWN;
}

static int64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static bool write_all(int fd, const char *data, size_t size) {
    while (size != 0U) {
        const ssize_t written = write(fd, data, size);
        if (written > 0) {
            data += (size_t)written;
            size -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool configure_port(int fd, struct termios *original) {
    if (tcgetattr(fd, original) != 0)
        return false;
    struct termios value = *original;
    value.c_iflag = IGNPAR;
    value.c_oflag = 0;
    value.c_lflag = 0;
    value.c_cflag &= (tcflag_t)~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    value.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
    value.c_cflag |= CS8 | CREAD | CLOCAL;
    value.c_cc[VMIN] = 0;
    value.c_cc[VTIME] = 0;
    if (cfsetispeed(&value, B115200) != 0 || cfsetospeed(&value, B115200) != 0)
        return false;
    if (tcsetattr(fd, TCSANOW, &value) != 0)
        return false;
    tcflush(fd, TCIOFLUSH);
    return true;
}

static int lock_modem(void) {
    if (mkdir("/tmp/edgenode", 0700) != 0 && errno != EEXIST)
        return -1;
    const int lock = open(EDGE_MODEM_LOCK, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX) != 0) {
        if (lock >= 0)
            close(lock);
        return -1;
    }
    return lock;
}

static bool copy_digit_token(const char *start, size_t minimum, size_t maximum,
                             char *output, size_t capacity) {
    while (*start == ' ' || *start == '\t')
        ++start;
    size_t length = 0U;
    while (start[length] >= '0' && start[length] <= '9')
        ++length;
    if (length < minimum || length > maximum || length + 1U > capacity)
        return false;
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool parse_imei(const char *response, char output[16]) {
    for (const char *cursor = response; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9' ||
            (cursor != response && cursor[-1] >= '0' && cursor[-1] <= '9'))
            continue;
        size_t length = 0U;
        while (cursor[length] >= '0' && cursor[length] <= '9')
            ++length;
        if (length == 15U && !(cursor[length] >= '0' && cursor[length] <= '9')) {
            memcpy(output, cursor, 15U);
            output[15] = '\0';
            if (edge_protocol_validate_imei(output))
                return true;
            output[0] = '\0';
        }
        cursor += length != 0U ? length - 1U : 0U;
    }
    return false;
}

static void parse_response(const char *response, edge_modem_info *info) {
    parse_imei(response, info->imei);

    const char *iccid = strstr(response, "+QCCID:");
    if (iccid != NULL)
        copy_digit_token(iccid + strlen("+QCCID:"), 18U, 22U,
                         info->iccid, sizeof(info->iccid));
    if (info->iccid[0] == '\0') {
        iccid = strstr(response, "+ICCID:");
        if (iccid != NULL)
            copy_digit_token(iccid + strlen("+ICCID:"), 18U, 22U,
                             info->iccid, sizeof(info->iccid));
    }

    const char *sim = strstr(response, "+CPIN:");
    if (sim != NULL) {
        if (strstr(sim, "NOT INSERTED") != NULL || strstr(sim, "NOT READY") != NULL)
            info->sim_state = iot_edge_v1_ModemSimState_MODEM_SIM_NOT_INSERTED;
        else if (strstr(sim, "SIM PIN") != NULL)
            info->sim_state = iot_edge_v1_ModemSimState_MODEM_SIM_PIN_REQUIRED;
        else if (strstr(sim, "SIM PUK") != NULL)
            info->sim_state = iot_edge_v1_ModemSimState_MODEM_SIM_PUK_REQUIRED;
        else if (strstr(sim, "BLOCKED") != NULL)
            info->sim_state = iot_edge_v1_ModemSimState_MODEM_SIM_BLOCKED;
        else if (strstr(sim, "READY") != NULL)
            info->sim_state = iot_edge_v1_ModemSimState_MODEM_SIM_READY;
    }

    const char *registration = strstr(response, "+CEREG:");
    int mode = 0;
    int status = -1;
    if (registration != NULL && sscanf(registration, "+CEREG: %d,%d", &mode, &status) == 2) {
        (void)mode;
        info->registration_status = status;
        info->registered = status == 1 || status == 5;
    }

    const char *signal = strstr(response, "+CSQ:");
    int csq = 99;
    int error_rate = 99;
    if (signal != NULL && sscanf(signal, "+CSQ: %d,%d", &csq, &error_rate) == 2) {
        (void)error_rate;
        if ((csq >= 0 && csq <= 31) || csq == 99) {
            info->csq = csq;
            if (csq <= 31) {
                info->rssi_dbm = -113 + 2 * csq;
                info->signal_percent = (unsigned)(csq * 100 + 15) / 31U;
            }
        }
    }

    const char *context = strstr(response, "+CGDCONT:");
    if (context != NULL)
        (void)sscanf(context, "+CGDCONT: %*d,\"%*[^\"]\",\"%63[^\"]\"", info->apn);

    const char *operator_response = strstr(response, "+COPS:");
    if (operator_response != NULL)
        (void)sscanf(operator_response, "+COPS: %*d,%*d,\"%64[^\"]\"",
                     info->mobile_operator);
}

static void read_network_state(const char *wan_interface, edge_modem_info *info) {
    if (wan_interface == NULL || *wan_interface == '\0')
        return;
    char carrier_path[128];
    bool carrier = true;
    if (snprintf(carrier_path, sizeof(carrier_path), "/sys/class/net/%s/carrier",
                 wan_interface) < (int)sizeof(carrier_path)) {
        FILE *file = fopen(carrier_path, "r");
        if (file != NULL) {
            int value = 0;
            if (fscanf(file, "%d", &value) == 1)
                carrier = value == 1;
            fclose(file);
        }
    }

    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0)
        return;
    for (const struct ifaddrs *item = interfaces; item != NULL; item = item->ifa_next) {
        if (item->ifa_addr == NULL || item->ifa_addr->sa_family != AF_INET ||
            strcmp(item->ifa_name, wan_interface) != 0)
            continue;
        const struct sockaddr_in *address = (const struct sockaddr_in *)item->ifa_addr;
        if (inet_ntop(AF_INET, &address->sin_addr, info->mobile_ipv4,
                      sizeof(info->mobile_ipv4)) != NULL)
            info->connected = carrier;
        break;
    }
    freeifaddrs(interfaces);
}

bool edge_modem_probe(const char *port, const char *wan_interface, edge_modem_info *info) {
    if (port == NULL || wan_interface == NULL || info == NULL)
        return false;
    initialize_info(info);

    const int lock = lock_modem();
    if (lock < 0)
        return false;
    const int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        close(lock);
        return false;
    }
    struct termios original;
    if (!configure_port(fd, &original)) {
        close(fd);
        close(lock);
        return false;
    }

    static const char commands[] =
        "ATE0\rAT+CPIN?\rAT+CEREG?\rAT+GSN\rAT+QCCID\rAT+CSQ\rAT+CGDCONT?\rAT+COPS?\r";
    if (!write_all(fd, commands, sizeof(commands) - 1U)) {
        tcsetattr(fd, TCSANOW, &original);
        close(fd);
        close(lock);
        return false;
    }

    char response[EDGE_MODEM_RESPONSE_CAPACITY];
    size_t used = 0U;
    const int64_t deadline = monotonic_milliseconds() + EDGE_MODEM_TIMEOUT_MS;
    while (used + 1U < sizeof(response)) {
        const int64_t remaining = deadline - monotonic_milliseconds();
        if (remaining <= 0)
            break;
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        const int ready = poll(&poll_fd, 1, (int)remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        const ssize_t count = read(fd, response + used, sizeof(response) - used - 1U);
        if (count > 0) {
            used += (size_t)count;
            response[used] = '\0';
            if (strstr(response, "+CEREG:") != NULL && strstr(response, "+QCCID:") != NULL &&
                strstr(response, "+CSQ:") != NULL && parse_imei(response, info->imei))
                break;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        break;
    }
    response[used] = '\0';
    parse_response(response, info);
    read_network_state(wan_interface, info);
    tcsetattr(fd, TCSANOW, &original);
    close(fd);
    close(lock);
    return info->imei[0] != '\0' || info->iccid[0] != '\0' || info->csq != 99 ||
           info->registration_status >= 0 ||
           info->sim_state != iot_edge_v1_ModemSimState_MODEM_SIM_UNKNOWN;
}

static struct uci_section *lookup_section(struct uci_context *context,
                                          struct uci_package *package,
                                          const char *name) {
    struct uci_element *element;
    uci_foreach_element(&package->sections, element) {
        struct uci_section *section = uci_to_section(element);
        if (strcmp(section->e.name, name) == 0)
            return section;
    }
    (void)context;
    return NULL;
}

static bool set_option_if_changed(struct uci_context *context, struct uci_package *package,
                                  struct uci_section *section, const char *name,
                                  const char *value, bool *changed) {
    const char *current = uci_lookup_option_string(context, section, name);
    if (current != NULL && strcmp(current, value) == 0)
        return true;
    struct uci_ptr pointer = {
        .p = package,
        .s = section,
        .option = name,
        .value = value,
    };
    if (uci_set(context, &pointer) != UCI_OK)
        return false;
    *changed = true;
    return true;
}

bool edge_modem_save_identity(const edge_modem_info *info) {
    if (info == NULL || !edge_protocol_validate_imei(info->imei))
        return false;
    struct uci_context *context = uci_alloc_context();
    struct uci_package *package = NULL;
    if (context == NULL || uci_load(context, "edgenode", &package) != UCI_OK) {
        if (context != NULL)
            uci_free_context(context);
        return false;
    }
    struct uci_section *node = lookup_section(context, package, "node");
    struct uci_section *modem = lookup_section(context, package, "modem");
    bool changed = false;
    bool success = node != NULL && modem != NULL &&
                   set_option_if_changed(context, package, node, "imei", info->imei, &changed) &&
                   set_option_if_changed(context, package, modem, "imei", info->imei, &changed);
    if (success && info->iccid[0] != '\0')
        success = set_option_if_changed(context, package, modem, "iccid", info->iccid, &changed);
    if (success && changed)
        success = uci_save(context, package) == UCI_OK &&
                  uci_commit(context, &package, false) == UCI_OK;
    if (package != NULL)
        uci_unload(context, package);
    uci_free_context(context);
    return success;
}

bool edge_modem_write_status(const char *path, const edge_modem_info *info, bool available) {
    if (path == NULL || info == NULL)
        return false;
    if (mkdir("/tmp/edgenode", 0700) != 0 && errno != EEXIST)
        return false;
    char temporary[256];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >=
        (int)sizeof(temporary))
        return false;
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
        close(fd);
        unlink(temporary);
        return false;
    }
    const int result = fprintf(file,
                               "available=%u\nregistered=%u\nregistration_status=%d\n"
                               "imei=%s\niccid=%s\ncsq=%d\nrssi_dbm=%d\n"
                               "signal_percent=%u\nsim_state=%d\napn=%s\n"
                               "mobile_operator=%s\nconnected=%u\nmobile_ipv4=%s\n"
                               "updated_at=%lld\n",
                               available ? 1U : 0U, info->registered ? 1U : 0U,
                               info->registration_status, info->imei, info->iccid, info->csq,
                               info->rssi_dbm, info->signal_percent, (int)info->sim_state,
                               info->apn, info->mobile_operator, info->connected ? 1U : 0U,
                               info->mobile_ipv4, (long long)time(NULL));
    const bool success = result > 0 && fclose(file) == 0 && rename(temporary, path) == 0;
    if (!success)
        unlink(temporary);
    return success;
}

bool edge_modem_read_status(const char *path, edge_modem_info *info, bool *available) {
    if (path == NULL || info == NULL || available == NULL)
        return false;
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;
    initialize_info(info);
    *available = false;
    char line[128];
    while (fgets(line, sizeof(line), file) != NULL) {
        char *newline = strpbrk(line, "\r\n");
        if (newline != NULL)
            *newline = '\0';
        char *separator = strchr(line, '=');
        if (separator == NULL)
            continue;
        *separator++ = '\0';
        if (strcmp(line, "available") == 0)
            *available = strcmp(separator, "1") == 0;
        else if (strcmp(line, "registered") == 0)
            info->registered = strcmp(separator, "1") == 0;
        else if (strcmp(line, "registration_status") == 0)
            info->registration_status = atoi(separator);
        else if (strcmp(line, "imei") == 0 && strlen(separator) < sizeof(info->imei))
            memcpy(info->imei, separator, strlen(separator) + 1U);
        else if (strcmp(line, "iccid") == 0 && strlen(separator) < sizeof(info->iccid))
            memcpy(info->iccid, separator, strlen(separator) + 1U);
        else if (strcmp(line, "csq") == 0)
            info->csq = atoi(separator);
        else if (strcmp(line, "rssi_dbm") == 0)
            info->rssi_dbm = atoi(separator);
        else if (strcmp(line, "signal_percent") == 0)
            info->signal_percent = (unsigned)strtoul(separator, NULL, 10);
        else if (strcmp(line, "sim_state") == 0)
            info->sim_state = (iot_edge_v1_ModemSimState)atoi(separator);
        else if (strcmp(line, "apn") == 0 && strlen(separator) < sizeof(info->apn))
            memcpy(info->apn, separator, strlen(separator) + 1U);
        else if (strcmp(line, "mobile_operator") == 0 &&
                 strlen(separator) < sizeof(info->mobile_operator))
            memcpy(info->mobile_operator, separator, strlen(separator) + 1U);
        else if (strcmp(line, "connected") == 0)
            info->connected = strcmp(separator, "1") == 0;
        else if (strcmp(line, "mobile_ipv4") == 0 &&
                 strlen(separator) < sizeof(info->mobile_ipv4))
            memcpy(info->mobile_ipv4, separator, strlen(separator) + 1U);
    }
    const bool success = !ferror(file);
    fclose(file);
    return success;
}

static bool valid_apn(const char *apn, bool allow_empty) {
    if (apn == NULL || strlen(apn) > 100U)
        return false;
    if (*apn == '\0')
        return allow_empty;
    size_t label_length = 0U;
    for (const unsigned char *cursor = (const unsigned char *)apn;
         *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            if (label_length == 0U || label_length > 63U || cursor[-1] == '-')
                return false;
            label_length = 0U;
            continue;
        }
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '-'))
            return false;
        if (label_length == 0U && *cursor == '-')
            return false;
        ++label_length;
    }
    return label_length != 0U && label_length <= 63U && apn[strlen(apn) - 1U] != '-';
}

static bool valid_pin(const char *pin) {
    if (pin == NULL || *pin == '\0')
        return true;
    const size_t size = strlen(pin);
    if (size < 4U || size > 8U)
        return false;
    for (const char *cursor = pin; *cursor != '\0'; ++cursor)
        if (*cursor < '0' || *cursor > '9')
            return false;
    return true;
}

static bool valid_credential(const char *value) {
    if (value == NULL)
        return false;
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor)
        if (*cursor < 0x20U || *cursor == 0x7fU || *cursor == '"')
            return false;
    return true;
}

static bool run_at_command(int fd, const char *command, bool require_ok) {
    (void)tcflush(fd, TCIFLUSH);
    if (!write_all(fd, command, strlen(command)))
        return false;
    char response[512] = {0};
    size_t used = 0U;
    const int64_t deadline = monotonic_milliseconds() + EDGE_MODEM_TIMEOUT_MS;
    while (used + 1U < sizeof(response)) {
        const int64_t remaining = deadline - monotonic_milliseconds();
        if (remaining <= 0)
            break;
        struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
        const int ready = poll(&poll_fd, 1, (int)remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;
        const ssize_t count = read(fd, response + used, sizeof(response) - used - 1U);
        if (count > 0) {
            used += (size_t)count;
            response[used] = '\0';
            if (strstr(response, "ERROR") != NULL ||
                strstr(response, "+CME ERROR") != NULL)
                return false;
            if (strstr(response, "\r\nOK\r\n") != NULL)
                return true;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        break;
    }
    return !require_ok;
}

bool edge_modem_validate_control(const iot_edge_v1_ModemControlRequest *request,
                                 char *message, size_t message_size) {
    if (message != NULL && message_size != 0U)
        message[0] = '\0';
    const iot_edge_v1_ModemControlAction action =
        request != NULL ? request->action
                        : iot_edge_v1_ModemControlAction_MODEM_CONTROL_UNSPECIFIED;
    const bool apply =
        action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE;
    const bool redial = action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_REDIAL;
    const bool auth_none = request != NULL &&
        request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_NONE;
    const bool auth_valid = request != NULL &&
        (auth_none ||
         request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP ||
         request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_CHAP ||
         request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP_OR_CHAP);
    const bool pdp_valid = request != NULL &&
        (request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4 ||
         request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV6 ||
         request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4V6);
    if (request == NULL || (!apply && !redial) ||
        (apply &&
         (!pdp_valid || !auth_valid ||
          !valid_apn(request->apn, request->automatic_apn) ||
          (request->automatic_apn && request->apn[0] != '\0') ||
          (!request->automatic_apn && request->apn[0] == '\0') ||
          !valid_pin(request->pin_code) || !valid_credential(request->username) ||
          !valid_credential(request->password) ||
          (auth_none && (request->username[0] != '\0' || request->password[0] != '\0')) ||
          (!auth_none && (request->username[0] == '\0' || request->password[0] == '\0'))))) {
        if (message != NULL && message_size != 0U)
            snprintf(message, message_size, "invalid modem control request");
        return false;
    }
    return true;
}

bool edge_modem_control(const char *port,
                        const iot_edge_v1_ModemControlRequest *request,
                        char *message, size_t message_size) {
    if (message != NULL && message_size != 0U)
        message[0] = '\0';
    if (port == NULL || *port == '\0' ||
        !edge_modem_validate_control(request, message, message_size)) {
        if (message != NULL && message_size != 0U && message[0] == '\0')
            snprintf(message, message_size, "modem AT port is unavailable");
        return false;
    }
    const iot_edge_v1_ModemControlAction action = request->action;
    const bool apply =
        action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_APPLY_PROFILE;
    const bool redial = action == iot_edge_v1_ModemControlAction_MODEM_CONTROL_REDIAL;

    const int lock = lock_modem();
    if (lock < 0) {
        if (message != NULL && message_size != 0U)
            snprintf(message, message_size, "modem is busy");
        return false;
    }
    const int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        close(lock);
        if (message != NULL && message_size != 0U)
            snprintf(message, message_size, "modem AT port is unavailable");
        return false;
    }
    struct termios original;
    if (!configure_port(fd, &original)) {
        close(fd);
        close(lock);
        if (message != NULL && message_size != 0U)
            snprintf(message, message_size, "cannot configure modem AT port");
        return false;
    }

    bool success = true;
    char command[384];
    if (apply) {
        if (request->pin_code[0] != '\0') {
            snprintf(command, sizeof(command), "AT+CPIN=\"%s\"\r", request->pin_code);
            success = run_at_command(fd, command, true);
        }
        const char *pdp = request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV6
                              ? "IPV6"
                          : request->pdp_type == iot_edge_v1_ModemPdpType_MODEM_PDP_IPV4V6
                              ? "IPV4V6"
                              : "IP";
        if (success) {
            snprintf(command, sizeof(command), "AT+CGDCONT=1,\"%s\",\"%s\"\r",
                     pdp, request->apn);
            success = run_at_command(fd, command, true);
        }
        if (success) {
            const unsigned auth =
                request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP ? 1U
                : request->auth_type == iot_edge_v1_ModemAuthType_MODEM_AUTH_CHAP ? 2U
                : request->auth_type ==
                      iot_edge_v1_ModemAuthType_MODEM_AUTH_PAP_OR_CHAP ? 3U
                                                                      : 0U;
            if (auth == 0U)
                snprintf(command, sizeof(command), "AT+CGAUTH=1,0\r");
            else
                snprintf(command, sizeof(command), "AT+CGAUTH=1,%u,\"%s\",\"%s\"\r",
                         auth, request->username, request->password);
            success = run_at_command(fd, command, true);
        }
    }
    if (success && (redial || request->redial_after_apply))
        success = run_at_command(fd, "AT+CFUN=1,1\r", false);
    tcsetattr(fd, TCSANOW, &original);
    close(fd);
    close(lock);
    if (message != NULL && message_size != 0U) {
        const char *result = !success
                                 ? "modem rejected control command"
                             : redial
                                 ? "modem reconnecting"
                             : request->redial_after_apply
                                 ? "mobile profile applied; modem reconnecting"
                                 : "mobile profile applied";
        snprintf(message, message_size, "%s", result);
    }
    return success;
}

int edge_modem_initialize(const char *port, const char *status_path,
                          const char *wan_interface) {
    edge_modem_info info;
    const bool available = edge_modem_probe(port, wan_interface, &info);
    edge_modem_write_status(status_path, &info, available);
    if (!available || !edge_protocol_validate_imei(info.imei))
        return EXIT_FAILURE;
    return edge_modem_save_identity(&info) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void stop_monitor(int signal_number) {
    (void)signal_number;
    monitor_stop = 1;
}

int edge_modem_monitor(const char *port, const char *status_path,
                       const char *wan_interface, unsigned interval_sec) {
    monitor_stop = 0;
    signal(SIGTERM, stop_monitor);
    signal(SIGINT, stop_monitor);
    signal(SIGHUP, stop_monitor);
    if (interval_sec == 0U)
        interval_sec = 30U;

    while (!monitor_stop) {
        edge_modem_info info;
        const bool available = edge_modem_probe(port, wan_interface, &info);
        if (available && edge_protocol_validate_imei(info.imei))
            edge_modem_save_identity(&info);
        edge_modem_write_status(status_path, &info, available);
        for (unsigned elapsed = 0U; elapsed < interval_sec && !monitor_stop; ++elapsed)
            sleep(1U);
    }
    return EXIT_SUCCESS;
}
