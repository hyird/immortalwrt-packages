#include "edge_firmware.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "edge_firmware_policy.h"
#include "edge_process.h"
#include "edge_sha256.h"

#define FIRMWARE_IMAGE "/tmp/edgenode/firmware.bin"
#define FIRMWARE_LOCK "/tmp/edgenode/firmware.lock"
#define OVERLAY_BINARY "/overlay/upper/usr/sbin/edgenode"
#define OVERLAY_BINARY_BACKUP "/tmp/edgenode/edgenode-overlay-backup"
#define FIRMWARE_STATUS_MAGIC 0x45444745U
#define FIRMWARE_DOWNLOAD_TIMEOUT_MS 1800000U
#define FIRMWARE_SYSUPGRADE_HANDOFF_GRACE_MS 120000U

typedef struct {
    uint32_t magic;
    uint8_t request_id[16];
    uint32_t state;
    uint64_t downloaded_bytes;
    uint64_t total_bytes;
    uint32_t progress_percent;
    char message[257];
} firmware_status;

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message != NULL ? message : "firmware error");
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0U;
    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

static void wait_for_sysupgrade_handoff(void) {
    const uint64_t deadline =
        monotonic_milliseconds() + FIRMWARE_SYSUPGRADE_HANDOFF_GRACE_MS;
    while (monotonic_milliseconds() < deadline) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    }
}

static bool valid_download_url(const char *url) {
    if (url == NULL || strncmp(url, "https://", 8U) != 0)
        return false;
    const unsigned char *host = (const unsigned char *)url + 8U;
    if (*host == '\0' || *host == '/')
        return false;
    bool in_host = true;
    size_t host_length = 0U;
    for (const unsigned char *cursor = host; *cursor != '\0'; ++cursor) {
        if (*cursor <= 0x20U || *cursor == 0x7fU || *cursor == '\\')
            return false;
        if (in_host && *cursor == '@')
            return false;
        if (*cursor == '/' || *cursor == '?' || *cursor == '#') {
            if (in_host && host_length == 0U)
                return false;
            in_host = false;
        } else if (in_host) {
            ++host_length;
        }
    }
    return host_length != 0U;
}

static void status_path(const uint8_t platform_id[16], char output[96]) {
    static const char hex[] = "0123456789abcdef";
    size_t offset = (size_t)snprintf(output, 96U, "/tmp/edgenode/firmware-");
    for (size_t index = 0; index < 16U && offset + 2U < 96U; ++index) {
        output[offset++] = hex[platform_id[index] >> 4U];
        output[offset++] = hex[platform_id[index] & 0x0FU];
    }
    snprintf(output + offset, 96U - offset, ".status");
}

static bool write_status(const uint8_t platform_id[16], const uint8_t request_id[16],
                          iot_edge_v1_FirmwareUpdateState state, const char *message,
                          uint64_t downloaded_bytes, uint64_t total_bytes) {
    char path[96];
    char temporary[104];
    status_path(platform_id, path);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    firmware_status value;
    memset(&value, 0, sizeof(value));
    value.magic = FIRMWARE_STATUS_MAGIC;
    memcpy(value.request_id, request_id, sizeof(value.request_id));
    value.state = (uint32_t)state;
    value.downloaded_bytes = downloaded_bytes;
    value.total_bytes = total_bytes;
    value.progress_percent =
        total_bytes == 0U
            ? 0U
            : (uint32_t)(downloaded_bytes >= total_bytes
                             ? 100U
                             : (downloaded_bytes * 100U) / total_bytes);
    snprintf(value.message, sizeof(value.message), "%s", message != NULL ? message : "");
    const int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0)
        return false;
    const ssize_t written = write(output, &value, sizeof(value));
    const bool ok = written == (ssize_t)sizeof(value) && fsync(output) == 0;
    close(output);
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

static bool sha256_file(const char *path, uint8_t output[32]) {
    FILE *input = fopen(path, "rb");
    if (input == NULL)
        return false;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = edge_sha256_starts(&context, 0) == 0;
    uint8_t buffer[4096];
    while (ok) {
        const size_t size = fread(buffer, 1U, sizeof(buffer), input);
        if (size != 0U && edge_sha256_update(&context, buffer, size) != 0)
            ok = false;
        if (size < sizeof(buffer)) {
            if (ferror(input) != 0)
                ok = false;
            break;
        }
    }
    if (ok)
        ok = edge_sha256_finish(&context, output) == 0;
    mbedtls_sha256_free(&context);
    fclose(input);
    return ok;
}

static bool copy_file(const char *source, const char *destination, mode_t mode) {
    const int input = open(source, O_RDONLY);
    if (input < 0)
        return errno == ENOENT;
    const int output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (output < 0) {
        close(input);
        return false;
    }
    bool ok = true;
    uint8_t buffer[4096];
    while (ok) {
        const ssize_t size = read(input, buffer, sizeof(buffer));
        if (size == 0)
            break;
        if (size < 0) {
            ok = errno == EINTR;
            continue;
        }
        ssize_t offset = 0;
        while (offset < size) {
            const ssize_t written =
                write(output, buffer + offset, (size_t)(size - offset));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                ok = false;
                break;
            }
            offset += written;
        }
    }
    if (ok)
        ok = fsync(output) == 0;
    close(output);
    close(input);
    if (!ok)
        unlink(destination);
    return ok;
}

static bool hide_overlay_binary(void) {
    unlink(OVERLAY_BINARY_BACKUP);
    if (access(OVERLAY_BINARY, F_OK) != 0)
        return errno == ENOENT;
    if (!copy_file(OVERLAY_BINARY, OVERLAY_BINARY_BACKUP, 0700))
        return false;
    if (unlink(OVERLAY_BINARY) == 0)
        return true;
    unlink(OVERLAY_BINARY_BACKUP);
    return false;
}

static void restore_overlay_binary(void) {
    if (access(OVERLAY_BINARY_BACKUP, F_OK) != 0)
        return;
    if (copy_file(OVERLAY_BINARY_BACKUP, OVERLAY_BINARY, 0755))
        unlink(OVERLAY_BINARY_BACKUP);
}

static void firmware_child(const uint8_t platform_id[16],
                           const iot_edge_v1_FirmwareUpdateRequest *request,
                           int lock_fd) {
    const uint8_t *request_id = request->request_id.bytes;
    unlink(FIRMWARE_IMAGE);
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_DOWNLOADING,
                       "downloading firmware", 0U, request->size_bytes);
    const pid_t downloader = fork();
    if (downloader == 0) {
        (void)setpgid(0, 0);
        edge_process_close_inherited_fds(-1);
        execlp("uclient-fetch", "uclient-fetch", "-O", FIRMWARE_IMAGE,
               request->download_url, (char *)NULL);
        _exit(127);
    }
    if (downloader > 0)
        (void)setpgid(downloader, downloader);
    int download_status = 0;
    uint64_t downloaded_bytes = 0U;
    pid_t waited = downloader < 0 ? -1 : 0;
    const uint64_t download_deadline =
        monotonic_milliseconds() + FIRMWARE_DOWNLOAD_TIMEOUT_MS;
    while (waited == 0 && monotonic_milliseconds() < download_deadline) {
        struct stat progress;
        if (stat(FIRMWARE_IMAGE, &progress) == 0 && progress.st_size >= 0) {
            const uint64_t current = (uint64_t)progress.st_size;
            if (current != downloaded_bytes) {
                downloaded_bytes = current;
                (void)write_status(
                    platform_id, request_id,
                    iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_DOWNLOADING,
                    "downloading firmware", downloaded_bytes, request->size_bytes);
            }
        }
        usleep(500000U);
        do {
            waited = waitpid(downloader, &download_status, WNOHANG);
        } while (waited < 0 && errno == EINTR);
    }
    const bool download_timed_out = waited == 0;
    if (download_timed_out) {
        (void)kill(-downloader, SIGKILL);
        (void)kill(downloader, SIGKILL);
        do {
            waited = waitpid(downloader, &download_status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    if (downloader < 0 || waited != downloader || !WIFEXITED(download_status) ||
        WEXITSTATUS(download_status) != 0) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           download_timed_out ? "firmware download timed out"
                                              : "firmware download failed",
                           downloaded_bytes, request->size_bytes);
        _exit(1);
    }
    struct stat info;
    if (stat(FIRMWARE_IMAGE, &info) != 0 || info.st_size < 0 ||
        (uint64_t)info.st_size != request->size_bytes) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "firmware size mismatch", downloaded_bytes,
                           request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        _exit(1);
    }
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_VERIFYING,
                       "verifying firmware sha256", request->size_bytes,
                       request->size_bytes);
    uint8_t actual[32];
    if (!sha256_file(FIRMWARE_IMAGE, actual) ||
        memcmp(actual, request->sha256.bytes, sizeof(actual)) != 0) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "firmware sha256 mismatch", request->size_bytes,
                           request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        _exit(1);
    }
    const char *validate[] = {"sysupgrade", "-T", FIRMWARE_IMAGE, NULL};
    if (edge_process_run(validate, -1, -1) != 0) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "sysupgrade rejected firmware during validation",
                           request->size_bytes, request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        _exit(1);
    }
    if (request->keep_settings && !hide_overlay_binary()) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "cannot prepare overlay binary replacement",
                           request->size_bytes, request->size_bytes);
        unlink(FIRMWARE_IMAGE);
        _exit(1);
    }
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FLASHING,
                       "firmware verified; sysupgrade is starting", request->size_bytes,
                       request->size_bytes);
    sleep(2U);
    const char *upgrade_keep[] = {"sysupgrade", "-v", FIRMWARE_IMAGE, NULL};
    const char *upgrade_clean[] = {"sysupgrade", "-v", "-n", FIRMWARE_IMAGE, NULL};
    const int upgrade_status = edge_process_run_timeout(
        request->keep_settings ? upgrade_keep : upgrade_clean, -1, -1, 300000U);
    if (edge_firmware_sysupgrade_may_have_handed_off(upgrade_status)) {
        /*
         * A successful OpenWrt handoff replaces procd and disconnects the ubus
         * client that sysupgrade is waiting on. The sysupgrade wrapper may
         * therefore return or be killed by stage2 before stage2 opens the image.
         * Keep both the image and the hidden overlay binary in place until stage2
         * kills this old process during the normal reboot path. Only exec failure
         * proves that no handoff could have started; otherwise the grace period
         * expires before the ordinary failure cleanup below.
         */
        wait_for_sysupgrade_handoff();
    }
    if (request->keep_settings)
        restore_overlay_binary();
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                       upgrade_status == 127
                           ? "sysupgrade command not found"
                           : "sysupgrade handoff did not reboot the device",
                       request->size_bytes, request->size_bytes);
    unlink(FIRMWARE_IMAGE);
    close(lock_fd);
    _exit(1);
}

bool edge_firmware_start(const uint8_t platform_id[16],
                         const iot_edge_v1_FirmwareUpdateRequest *request,
                         char *error, size_t error_size) {
    if (platform_id == NULL || request == NULL || request->request_id.size != 16U ||
        request->sha256.size != 32U || request->size_bytes == 0U ||
        request->size_bytes > 128U * 1024U * 1024U ||
        !valid_download_url(request->download_url)) {
        set_error(error, error_size, "firmware request is invalid");
        return false;
    }
    const int lock = open(FIRMWARE_LOCK, O_WRONLY | O_CREAT, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0)
            close(lock);
        set_error(error, error_size, "another firmware update is active");
        return false;
    }
    const int worker = edge_process_detach();
    if (worker < 0) {
        close(lock);
        unlink(FIRMWARE_LOCK);
        set_error(error, error_size, "cannot start firmware worker");
        return false;
    }
    if (worker == 0) {
        edge_process_close_inherited_fds(lock);
        firmware_child(platform_id, request, lock);
    }
    close(lock);
    return true;
}

bool edge_firmware_read_status(const uint8_t platform_id[16],
                               iot_edge_v1_FirmwareUpdateResult *result) {
    if (platform_id == NULL || result == NULL)
        return false;
    char path[96];
    status_path(platform_id, path);
    const int input = open(path, O_RDONLY);
    if (input < 0)
        return false;
    firmware_status value;
    const ssize_t size = read(input, &value, sizeof(value));
    close(input);
    if (size != (ssize_t)sizeof(value) || value.magic != FIRMWARE_STATUS_MAGIC)
        return false;
    unlink(path);
    memset(result, 0, sizeof(*result));
    result->request_id.size = 16U;
    memcpy(result->request_id.bytes, value.request_id, 16U);
    result->state = (iot_edge_v1_FirmwareUpdateState)value.state;
    result->downloaded_bytes = value.downloaded_bytes;
    result->total_bytes = value.total_bytes;
    result->progress_percent = value.progress_percent;
    snprintf(result->message, sizeof(result->message), "%s", value.message);
    return true;
}

bool edge_firmware_active(void) {
    const int lock = open(FIRMWARE_LOCK, O_WRONLY | O_CREAT, 0600);
    if (lock < 0)
        return true;
    const bool active = flock(lock, LOCK_EX | LOCK_NB) != 0;
    if (!active)
        (void)flock(lock, LOCK_UN);
    close(lock);
    return active;
}

bool edge_firmware_has_status(const uint8_t platform_id[16]) {
    if (platform_id == NULL)
        return false;
    char path[96];
    status_path(platform_id, path);
    return access(path, F_OK) == 0;
}
