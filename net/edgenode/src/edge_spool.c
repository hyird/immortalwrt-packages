#include "edge_spool.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "edge.pb.h"
#include "edge_config.h"
#include "edge_protocol.h"
#include "edge_sha256.h"

#define EDGE_TMPFS_MIN_FREE_PERCENT 15U

static bool path_join(char *output, size_t capacity, const char *left, const char *right);

static bool find_oldest_outbox(char *output, size_t capacity) {
    DIR *root = opendir(EDGE_SPOOL_ROOT);
    if (root == NULL)
        return false;
    bool found = false;
    time_t oldest = 0;
    struct dirent *platform;
    while ((platform = readdir(root)) != NULL) {
        if (platform->d_name[0] == '.')
            continue;
        char platform_path[192];
        char outbox_path[224];
        if (!path_join(platform_path, sizeof(platform_path), EDGE_SPOOL_ROOT,
                       platform->d_name) ||
            !path_join(outbox_path, sizeof(outbox_path), platform_path, "outbox"))
            continue;
        DIR *outbox = opendir(outbox_path);
        if (outbox == NULL)
            continue;
        struct dirent *entry;
        while ((entry = readdir(outbox)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            char candidate[256];
            struct stat info;
            if (!path_join(candidate, sizeof(candidate), outbox_path, entry->d_name) ||
                lstat(candidate, &info) != 0 || !S_ISREG(info.st_mode))
                continue;
            if (!found || info.st_mtime < oldest) {
                const size_t length = strlen(candidate);
                if (length >= capacity)
                    continue;
                memcpy(output, candidate, length + 1U);
                oldest = info.st_mtime;
                found = true;
            }
        }
        closedir(outbox);
    }
    closedir(root);
    return found;
}

static bool ensure_tmpfs_reserve(size_t incoming_bytes) {
    for (;;) {
        struct statvfs usage;
        if (statvfs(EDGE_SPOOL_ROOT, &usage) != 0 || usage.f_blocks == 0U)
            return false;
        const uint64_t block_size = usage.f_frsize != 0U ? usage.f_frsize : usage.f_bsize;
        const uint64_t total = (uint64_t)usage.f_blocks * block_size;
        const uint64_t available = (uint64_t)usage.f_bavail * block_size;
        const uint64_t reserve = (total * EDGE_TMPFS_MIN_FREE_PERCENT + 99U) / 100U;
        if (available >= reserve && available - reserve >= incoming_bytes)
            return true;
        char oldest[256];
        if (!find_oldest_outbox(oldest, sizeof(oldest)) || unlink(oldest) != 0) {
            syslog(LOG_ERR, "tmpfs has less than 15%% reserve and no outbox record can be removed");
            return false;
        }
        syslog(LOG_WARNING, "tmpfs reserve below 15%%; removed oldest outbox message: %s",
               oldest);
    }
}

static bool path_join(char *output, size_t capacity, const char *left, const char *right) {
    const int result = snprintf(output, capacity, "%s/%s", left, right);
    return result > 0 && (size_t)result < capacity;
}

static bool make_directory(const char *path) {
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static bool sync_directory(const char *path) {
    const int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

static bool write_atomic(const char *directory, const char *name,
                         const uint8_t *data, size_t size) {
    char path[256];
    char temporary[288];
    if (data == NULL || size == 0U || !ensure_tmpfs_reserve(size) ||
        !path_join(path, sizeof(path), directory, name))
        return false;
    const int temporary_size = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                                        (long)getpid());
    if (temporary_size <= 0 ||
        (size_t)temporary_size >= sizeof(temporary))
        return false;
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    size_t written = 0U;
    while (written < size) {
        const ssize_t count = write(fd, data + written, size - written);
        if (count <= 0) {
            close(fd);
            unlink(temporary);
            return false;
        }
        written += (size_t)count;
    }
    const bool file_synced = fsync(fd) == 0;
    const bool file_closed = close(fd) == 0;
    const bool ok = file_synced && file_closed && rename(temporary, path) == 0 &&
                    sync_directory(directory);
    if (!ok)
        unlink(temporary);
    return ok;
}

static uint8_t *read_file(const char *path, size_t maximum, size_t *size) {
    *size = 0U;
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        (uint64_t)info.st_size > maximum)
        return NULL;
    uint8_t *data = malloc((size_t)info.st_size);
    if (data == NULL)
        return NULL;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        free(data);
        return NULL;
    }
    size_t offset = 0U;
    while (offset < (size_t)info.st_size) {
        const ssize_t count = read(fd, data + offset, (size_t)info.st_size - offset);
        if (count <= 0) {
            close(fd);
            free(data);
            return NULL;
        }
        offset += (size_t)count;
    }
    close(fd);
    *size = offset;
    return data;
}

static bool remove_tree(const char *path) {
    DIR *directory = opendir(path);
    if (directory == NULL)
        return errno == ENOENT;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[256];
        struct stat info;
        if (!path_join(child, sizeof(child), path, entry->d_name) || lstat(child, &info) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(info.st_mode))
            ok = remove_tree(child) && ok;
        else
            ok = unlink(child) == 0 && ok;
    }
    closedir(directory);
    return rmdir(path) == 0 && ok;
}

static bool encode_message(const pb_msgdesc_t *fields, const void *message,
                           uint8_t *output, size_t capacity, size_t *size) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, fields, message))
        return false;
    *size = stream.bytes_written;
    return true;
}

static bool decode_message(const pb_msgdesc_t *fields, const uint8_t *data,
                           size_t size, void *message) {
    pb_istream_t stream = pb_istream_from_buffer(data, size);
    return pb_decode(&stream, fields, message);
}

static void id_name(const uint8_t id[16], char output[36]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < 16U; ++index) {
        output[index * 2U] = digits[id[index] >> 4U];
        output[index * 2U + 1U] = digits[id[index] & 0x0fU];
    }
    memcpy(output + 32U, ".pb", 4U);
}

static bool parse_id_name(const char *name, uint8_t id[16]) {
    if (strlen(name) != 35U || strcmp(name + 32U, ".pb") != 0)
        return false;
    char uuid[37];
    snprintf(uuid, sizeof(uuid), "%.8s-%.4s-%.4s-%.4s-%.12s",
             name, name + 8U, name + 12U, name + 16U, name + 20U);
    return edge_config_parse_uuid(uuid, id);
}

static bool verify_item_blob(const uint8_t *payload, size_t payload_size,
                             iot_edge_v1_ConfigItem *item) {
    *item = (iot_edge_v1_ConfigItem)iot_edge_v1_ConfigItem_init_zero;
    if (!decode_message(iot_edge_v1_ConfigItem_fields, payload, payload_size, item) ||
        item->sha256.size != 32U)
        return false;
    uint8_t expected[32];
    memcpy(expected, item->sha256.bytes, 32U);
    item->sha256.size = 0U;
    uint8_t canonical[iot_edge_v1_ConfigItem_size];
    size_t canonical_size = 0U;
    if (!encode_message(iot_edge_v1_ConfigItem_fields, item, canonical,
                        sizeof(canonical), &canonical_size))
        return false;
    uint8_t actual[32];
    const bool ok = edge_sha256(canonical, canonical_size, actual, 0) == 0 &&
                    memcmp(actual, expected, 32U) == 0;
    memcpy(item->sha256.bytes, expected, 32U);
    item->sha256.size = 32U;
    return ok;
}

static bool load_active(edge_spool *spool) {
    char directory[192];
    char path[224];
    if (!path_join(directory, sizeof(directory), spool->directory, "active") ||
        !path_join(path, sizeof(path), directory, "begin.pb"))
        return false;
    size_t size = 0U;
    uint8_t *data = read_file(path, iot_edge_v1_ConfigBegin_size, &size);
    if (data == NULL)
        return access(directory, F_OK) != 0;
    iot_edge_v1_ConfigBegin begin = iot_edge_v1_ConfigBegin_init_zero;
    const bool begin_ok = decode_message(iot_edge_v1_ConfigBegin_fields, data, size, &begin) &&
                          begin.sha256.size == 32U &&
                          edge_memory_config_begin(&spool->staging_config, begin.revision,
                                                   begin.item_count, begin.sha256.bytes);
    free(data);
    if (!begin_ok)
        return false;
    for (uint32_t index = 0; index < begin.item_count; ++index) {
        char name[32];
        snprintf(name, sizeof(name), "%08lu.pb", (unsigned long)index);
        if (!path_join(path, sizeof(path), directory, name))
            return false;
        data = read_file(path, iot_edge_v1_ConfigItem_size, &size);
        iot_edge_v1_ConfigItem item;
        if (data == NULL || !verify_item_blob(data, size, &item) ||
            item.revision != begin.revision || item.index != index ||
            !edge_memory_config_put(&spool->staging_config, item.revision, item.index,
                                    item.sha256.bytes, data, size)) {
            free(data);
            return false;
        }
        free(data);
    }
    if (!path_join(path, sizeof(path), directory, "commit.pb"))
        return false;
    data = read_file(path, iot_edge_v1_ConfigCommit_size, &size);
    iot_edge_v1_ConfigCommit commit = iot_edge_v1_ConfigCommit_init_zero;
    const bool committed = data != NULL &&
                           decode_message(iot_edge_v1_ConfigCommit_fields, data, size, &commit) &&
                           commit.sha256.size == 32U &&
                           edge_memory_config_commit(&spool->active_config,
                                                     &spool->staging_config,
                                                     commit.revision, commit.sha256.bytes);
    free(data);
    return committed;
}

static bool load_outbox(edge_spool *spool) {
    char directory[192];
    if (!path_join(directory, sizeof(directory), spool->directory, "outbox"))
        return false;
    DIR *input = opendir(directory);
    if (input == NULL)
        return errno == ENOENT;
    struct dirent *entry;
    while ((entry = readdir(input)) != NULL) {
        uint8_t message_id[16];
        if (!parse_id_name(entry->d_name, message_id))
            continue;
        char path[232];
        size_t size = 0U;
        if (!path_join(path, sizeof(path), directory, entry->d_name)) {
            continue;
        }
        uint8_t *data = read_file(path, EDGENODE_MAX_WS_MESSAGE, &size);
        iot_edge_v1_Envelope envelope;
        const char *error = NULL;
        bool entry_ok = true;
        if (data == NULL || !edge_protocol_decode(data, size, &envelope, &error) ||
            envelope.platform_id.size != 16U ||
            memcmp(envelope.platform_id.bytes, spool->platform_id, 16U) != 0)
            entry_ok = false;
        else {
            const uint8_t *ack_id = envelope.message_id.bytes;
            size_t ack_id_size = envelope.message_id.size;
            if (envelope.which_payload == iot_edge_v1_Envelope_telemetry_batch_tag &&
                envelope.payload.telemetry_batch.records_count == 1U) {
                ack_id = envelope.payload.telemetry_batch.records[0].record_id.bytes;
                ack_id_size = envelope.payload.telemetry_batch.records[0].record_id.size;
            } else if (envelope.which_payload == iot_edge_v1_Envelope_raw_packet_tag) {
                ack_id = envelope.payload.raw_packet.packet_id.bytes;
                ack_id_size = envelope.payload.raw_packet.packet_id.size;
            } else if (envelope.which_payload ==
                       iot_edge_v1_Envelope_command_result_tag) {
                ack_id = envelope.payload.command_result.command_id.bytes;
                ack_id_size = envelope.payload.command_result.command_id.size;
            }
            const bool priority = envelope.which_payload ==
                                  iot_edge_v1_Envelope_command_result_tag;
            if (ack_id_size != 16U || memcmp(ack_id, message_id, 16U) != 0 ||
                !edge_memory_outbox_put_priority(&spool->outbox, message_id, data,
                                                 size, priority))
                entry_ok = false;
        }
        if (!entry_ok) {
            unlink(path);
            syslog(LOG_WARNING, "removed invalid or over-limit tmpfs outbox message: %s", path);
        }
        free(data);
    }
    closedir(input);
    return true;
}

bool edge_spool_init(edge_spool *spool, const uint8_t platform_id[16],
                     size_t outbox_maximum_bytes) {
    if (spool == NULL || platform_id == NULL || outbox_maximum_bytes == 0U)
        return false;
    memset(spool, 0, sizeof(*spool));
    memcpy(spool->platform_id, platform_id, 16U);
    edge_memory_outbox_init(&spool->outbox, outbox_maximum_bytes);
    char uuid[37];
    edge_config_format_uuid(platform_id, uuid);
    if (!make_directory(EDGE_SPOOL_ROOT) ||
        !path_join(spool->directory, sizeof(spool->directory), EDGE_SPOOL_ROOT, uuid) ||
        !make_directory(spool->directory))
        return false;
    char outbox[192];
    if (!path_join(outbox, sizeof(outbox), spool->directory, "outbox") ||
        !make_directory(outbox))
        return false;
    char active[192];
    char previous[192];
    if (!path_join(active, sizeof(active), spool->directory, "active") ||
        !path_join(previous, sizeof(previous), spool->directory, "active.previous"))
        return false;
    if (access(active, F_OK) != 0 && access(previous, F_OK) == 0)
        rename(previous, active);
    else if (access(active, F_OK) == 0)
        remove_tree(previous);
    if (!load_active(spool)) {
        syslog(LOG_WARNING, "discarded incomplete tmpfs active configuration for platform %s",
               uuid);
        edge_memory_config_free(&spool->staging_config);
        edge_memory_config_free(&spool->active_config);
        remove_tree(active);
    }
    return load_outbox(spool);
}

void edge_spool_free(edge_spool *spool) {
    if (spool == NULL)
        return;
    edge_memory_config_free(&spool->staging_config);
    edge_memory_config_free(&spool->active_config);
    edge_memory_outbox_free(&spool->outbox);
    memset(spool, 0, sizeof(*spool));
}

bool edge_spool_config_begin(edge_spool *spool, uint64_t revision,
                             uint32_t item_count, const uint8_t digest[32]) {
    if (spool == NULL || !edge_memory_config_begin(&spool->staging_config, revision,
                                                   item_count, digest))
        return false;
    char staging[192];
    if (!path_join(staging, sizeof(staging), spool->directory, "staging")) {
        edge_memory_config_free(&spool->staging_config);
        return false;
    }
    remove_tree(staging);
    if (!make_directory(staging)) {
        edge_memory_config_free(&spool->staging_config);
        return false;
    }
    iot_edge_v1_ConfigBegin begin = iot_edge_v1_ConfigBegin_init_zero;
    begin.revision = revision;
    begin.item_count = item_count;
    begin.sha256.size = 32U;
    memcpy(begin.sha256.bytes, digest, 32U);
    uint8_t encoded[iot_edge_v1_ConfigBegin_size];
    size_t encoded_size = 0U;
    const bool ok = encode_message(iot_edge_v1_ConfigBegin_fields, &begin, encoded,
                                   sizeof(encoded), &encoded_size) &&
                    write_atomic(staging, "begin.pb", encoded, encoded_size);
    if (!ok) {
        edge_memory_config_free(&spool->staging_config);
        remove_tree(staging);
    }
    return ok;
}

bool edge_spool_config_put(edge_spool *spool, uint64_t revision, uint32_t index,
                           const uint8_t digest[32], const uint8_t *payload,
                           size_t payload_size) {
    if (spool == NULL || index >= EDGE_MAX_CONFIG_ITEMS)
        return false;
    char staging[192];
    char name[32];
    if (!path_join(staging, sizeof(staging), spool->directory, "staging") ||
        snprintf(name, sizeof(name), "%08lu.pb", (unsigned long)index) <= 0 ||
        !write_atomic(staging, name, payload, payload_size))
        return false;
    if (!edge_memory_config_put(&spool->staging_config, revision, index, digest,
                                payload, payload_size)) {
        char path[224];
        if (path_join(path, sizeof(path), staging, name))
            unlink(path);
        return false;
    }
    return true;
}

bool edge_spool_config_commit(edge_spool *spool, uint64_t revision,
                              const uint8_t digest[32]) {
    if (spool == NULL || digest == NULL || spool->staging_config.revision != revision ||
        spool->staging_config.received_count != spool->staging_config.item_count)
        return false;
    /* Validate on a shallow digest pass before changing filesystem state. */
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool valid = edge_sha256_starts(&sha, 0) == 0;
    for (uint32_t index = 0; valid && index < spool->staging_config.item_count; ++index)
        valid = spool->staging_config.items[index].present &&
                edge_sha256_update(&sha, spool->staging_config.items[index].digest, 32U) == 0;
    uint8_t actual[32];
    valid = valid && edge_sha256_finish(&sha, actual) == 0 &&
            memcmp(actual, digest, 32U) == 0 &&
            memcmp(spool->staging_config.digest, digest, 32U) == 0;
    mbedtls_sha256_free(&sha);
    if (!valid)
        return false;

    char staging[192];
    char active[192];
    char previous[192];
    if (!path_join(staging, sizeof(staging), spool->directory, "staging") ||
        !path_join(active, sizeof(active), spool->directory, "active") ||
        !path_join(previous, sizeof(previous), spool->directory, "active.previous"))
        return false;
    iot_edge_v1_ConfigCommit commit = iot_edge_v1_ConfigCommit_init_zero;
    commit.revision = revision;
    commit.sha256.size = 32U;
    memcpy(commit.sha256.bytes, digest, 32U);
    uint8_t encoded[iot_edge_v1_ConfigCommit_size];
    size_t encoded_size = 0U;
    if (!encode_message(iot_edge_v1_ConfigCommit_fields, &commit, encoded,
                        sizeof(encoded), &encoded_size) ||
        !write_atomic(staging, "commit.pb", encoded, encoded_size))
        return false;
    remove_tree(previous);
    const bool had_active = rename(active, previous) == 0;
    if (rename(staging, active) != 0) {
        if (had_active)
            rename(previous, active);
        return false;
    }
    sync_directory(spool->directory);
    remove_tree(previous);
    return edge_memory_config_commit(&spool->active_config, &spool->staging_config,
                                     revision, digest);
}

bool edge_spool_outbox_put(edge_spool *spool, const uint8_t message_id[16],
                           const uint8_t *envelope, size_t envelope_size) {
    return edge_spool_outbox_put_priority(spool, message_id, envelope,
                                          envelope_size, false);
}

bool edge_spool_outbox_put_priority(edge_spool *spool,
                                    const uint8_t message_id[16],
                                    const uint8_t *envelope, size_t envelope_size,
                                    bool priority) {
    if (spool == NULL || message_id == NULL || envelope == NULL ||
        envelope_size == 0U || envelope_size > EDGENODE_MAX_WS_MESSAGE ||
        envelope_size > spool->outbox.maximum_bytes)
        return false;
    (void)edge_spool_outbox_first(spool);
    while (spool->outbox.bytes > spool->outbox.maximum_bytes - envelope_size) {
        const edge_memory_message *oldest =
            edge_memory_outbox_oldest_regular(&spool->outbox);
        if (oldest == NULL && priority)
            oldest = edge_spool_outbox_first(spool);
        if (oldest == NULL)
            return false;
        uint8_t oldest_id[16];
        memcpy(oldest_id, oldest->message_id, sizeof(oldest_id));
        if (!edge_spool_outbox_ack(spool, oldest_id))
            return false;
        syslog(LOG_WARNING, "platform outbox limit reached; removed oldest nanopb message");
    }
    char outbox[192];
    char name[36];
    if (!path_join(outbox, sizeof(outbox), spool->directory, "outbox"))
        return false;
    id_name(message_id, name);
    if (!write_atomic(outbox, name, envelope, envelope_size))
        return false;
    (void)edge_spool_outbox_first(spool);
    if (!edge_memory_outbox_put_priority(&spool->outbox, message_id, envelope,
                                         envelope_size, priority)) {
        char path[232];
        if (path_join(path, sizeof(path), outbox, name))
            unlink(path);
        return false;
    }
    return true;
}

const edge_memory_message *edge_spool_outbox_first(edge_spool *spool) {
    if (spool == NULL)
        return NULL;
    for (;;) {
        const edge_memory_message *message = edge_memory_outbox_first(&spool->outbox);
        if (message == NULL)
            return NULL;
        char outbox[192];
        char name[36];
        char path[232];
        id_name(message->message_id, name);
        if (!path_join(outbox, sizeof(outbox), spool->directory, "outbox") ||
            !path_join(path, sizeof(path), outbox, name))
            return NULL;
        if (access(path, F_OK) == 0)
            return message;
        /* A global 15% cleanup may have removed another session's oldest file. */
        edge_memory_outbox_ack(&spool->outbox, message->message_id);
    }
}

const edge_memory_message *edge_spool_outbox_next(edge_spool *spool,
                                                  uint64_t now_ms) {
    if (spool == NULL)
        return NULL;
    for (;;) {
        const edge_memory_message *message =
            edge_memory_outbox_next(&spool->outbox, now_ms);
        if (message == NULL)
            return NULL;
        char outbox[192];
        char name[36];
        char path[232];
        id_name(message->message_id, name);
        if (!path_join(outbox, sizeof(outbox), spool->directory, "outbox") ||
            !path_join(path, sizeof(path), outbox, name)) {
            (void)edge_memory_outbox_retry(&spool->outbox, message->message_id);
            return NULL;
        }
        if (access(path, F_OK) == 0)
            return message;
        edge_memory_outbox_ack(&spool->outbox, message->message_id);
    }
}

bool edge_spool_outbox_timed_out(const edge_spool *spool,
                                 uint64_t now_ms, uint32_t timeout_ms) {
    return spool != NULL &&
           edge_memory_outbox_timed_out(&spool->outbox, now_ms, timeout_ms);
}

bool edge_spool_outbox_retry(edge_spool *spool, const uint8_t message_id[16]) {
    return spool != NULL && edge_memory_outbox_retry(&spool->outbox, message_id);
}

void edge_spool_outbox_reset(edge_spool *spool) {
    if (spool != NULL)
        edge_memory_outbox_reset(&spool->outbox);
}

bool edge_spool_outbox_ack(edge_spool *spool, const uint8_t message_id[16]) {
    if (spool == NULL || message_id == NULL)
        return false;
    char outbox[192];
    char name[36];
    char path[232];
    id_name(message_id, name);
    if (!path_join(outbox, sizeof(outbox), spool->directory, "outbox") ||
        !path_join(path, sizeof(path), outbox, name))
        return false;
    if (unlink(path) != 0 && errno != ENOENT)
        return false;
    sync_directory(outbox);
    return edge_memory_outbox_ack(&spool->outbox, message_id);
}

bool edge_spool_maintain(edge_spool *spool) {
    if (spool == NULL || !ensure_tmpfs_reserve(0U))
        return false;
    (void)edge_spool_outbox_first(spool);
    return true;
}
