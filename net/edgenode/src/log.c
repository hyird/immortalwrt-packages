#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "edge_protocol.h"

#define EDGE_LOG_ROOT "/tmp/edgenode"
#define EDGE_LOG_DIR EDGE_LOG_ROOT "/logs"
#define EDGE_LOG_PATH EDGE_LOG_DIR "/current.log"
#define EDGE_LOG_MAX_FILE_BYTES (256U * 1024U)
#define EDGE_LOG_MAX_FILES 16U
#define EDGE_LOG_FREE_PERCENT 20U

static int log_threshold = 1;
static char log_level_name[9] = "info";

static int64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void copy_text(char *output, size_t capacity, const char *input) {
    if (capacity == 0U)
        return;
    snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static int level_rank(const char *level) {
    if (level != NULL && strcmp(level, "debug") == 0)
        return 0;
    if (level != NULL && strcmp(level, "warn") == 0)
        return 2;
    if (level != NULL && strcmp(level, "error") == 0)
        return 3;
    return 1;
}

static bool valid_level(const char *level) {
    return level != NULL &&
           (strcmp(level, "debug") == 0 || strcmp(level, "info") == 0 ||
            strcmp(level, "warn") == 0 || strcmp(level, "error") == 0);
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return true;
    return false;
}

static bool ensure_log_dir(void) {
    return ensure_directory(EDGE_LOG_ROOT) && ensure_directory(EDGE_LOG_DIR);
}

static void log_path(unsigned index, char *path, size_t size) {
    if (index == 0U)
        snprintf(path, size, "%s", EDGE_LOG_PATH);
    else
        snprintf(path, size, "%s.%u", EDGE_LOG_PATH, index);
}

static off_t file_size(const char *path) {
    struct stat info;
    if (stat(path, &info) != 0)
        return 0;
    return info.st_size;
}

static bool tmpfs_has_reserve(void) {
    struct statvfs fs;
    if (statvfs("/tmp", &fs) != 0 || fs.f_blocks == 0U)
        return true;
    const unsigned long long available =
        (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_frsize;
    const unsigned long long total =
        (unsigned long long)fs.f_blocks * (unsigned long long)fs.f_frsize;
    return available * 100ULL >= total * EDGE_LOG_FREE_PERCENT;
}

static bool remove_oldest_log(void) {
    for (int index = (int)EDGE_LOG_MAX_FILES - 1; index >= 1; --index) {
        char path[128];
        log_path((unsigned)index, path, sizeof(path));
        if (access(path, F_OK) == 0) {
            (void)unlink(path);
            return true;
        }
    }
    if (access(EDGE_LOG_PATH, F_OK) == 0) {
        FILE *output = fopen(EDGE_LOG_PATH, "w");
        if (output != NULL) {
            fclose(output);
            return true;
        }
    }
    return false;
}

static void enforce_space_reserve(void) {
    for (unsigned attempt = 0U; attempt < EDGE_LOG_MAX_FILES + 1U; ++attempt) {
        if (tmpfs_has_reserve())
            return;
        if (!remove_oldest_log())
            return;
    }
}

static void rotate_logs(void) {
    char from[128];
    char to[128];
    log_path(EDGE_LOG_MAX_FILES - 1U, to, sizeof(to));
    (void)unlink(to);
    for (int index = (int)EDGE_LOG_MAX_FILES - 2; index >= 1; --index) {
        log_path((unsigned)index, from, sizeof(from));
        log_path((unsigned)index + 1U, to, sizeof(to));
        if (access(from, F_OK) == 0)
            (void)rename(from, to);
    }
    log_path(1U, to, sizeof(to));
    if (access(EDGE_LOG_PATH, F_OK) == 0)
        (void)rename(EDGE_LOG_PATH, to);
}

static void write_escaped(FILE *output, const char *input, size_t limit) {
    size_t written = 0U;
    const unsigned char *cursor = (const unsigned char *)(input != NULL ? input : "");
    while (*cursor != '\0' && written < limit) {
        const unsigned char value = *cursor++;
        char escaped = '\0';
        if (value == '\\')
            escaped = '\\';
        else if (value == '\t')
            escaped = 't';
        else if (value == '\n')
            escaped = 'n';
        else if (value == '\r')
            escaped = 'r';
        if (escaped != '\0') {
            if (written + 2U > limit)
                break;
            fputc('\\', output);
            fputc(escaped, output);
            written += 2U;
        } else {
            fputc(value >= 32U && value != 127U ? value : ' ', output);
            ++written;
        }
    }
}

static void copy_unescaped(char *output, size_t capacity, const char *input) {
    if (capacity == 0U)
        return;
    size_t written = 0U;
    for (size_t index = 0U; input != NULL && input[index] != '\0' &&
                           written + 1U < capacity; ++index) {
        char value = input[index];
        if (value == '\\' && input[index + 1U] != '\0') {
            const char next = input[++index];
            value = next == 't' ? '\t'
                    : next == 'n' ? '\n'
                    : next == 'r' ? '\r'
                                  : next;
        }
        output[written++] = value;
    }
    output[written] = '\0';
}

static bool split_line(char *line, char **parts, size_t count) {
    if (line == NULL || parts == NULL || count == 0U)
        return false;
    parts[0] = line;
    size_t output = 1U;
    for (char *cursor = line; *cursor != '\0' && output < count; ++cursor) {
        if (*cursor == '\t') {
            *cursor = '\0';
            parts[output++] = cursor + 1;
        }
    }
    return output == count;
}

static bool parse_time_ms(const char *text, int64_t *output) {
    if (text == NULL || output == NULL)
        return false;
    char *end = NULL;
    errno = 0;
    const long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *output = (int64_t)value;
    return true;
}

static bool matches(const iot_edge_v1_LogRequest *request, const char *level,
                    const char *source) {
    if (request == NULL)
        return false;
    if (request->level[0] != '\0' && strcmp(level, request->level) != 0)
        return false;
    if (request->source[0] != '\0' && strcmp(source, request->source) != 0)
        return false;
    return true;
}

static void append_line(char *line, const iot_edge_v1_LogRequest *request,
                        iot_edge_v1_LogResult *result, uint32_t limit) {
    char *parts[5] = {0};
    int64_t time = 0;
    if (result->lines_count >= limit || !split_line(line, parts, 5U) ||
        !parse_time_ms(parts[0], &time) || !matches(request, parts[1], parts[2]))
        return;
    iot_edge_v1_LogLine *output = &result->lines[result->lines_count++];
    output->time_ms = time;
    copy_unescaped(output->level, sizeof(output->level), parts[1]);
    copy_unescaped(output->source, sizeof(output->source), parts[2]);
    copy_unescaped(output->message, sizeof(output->message), parts[3]);
    copy_unescaped(output->detail, sizeof(output->detail), parts[4]);
}

static void read_reverse_file(const char *path, const iot_edge_v1_LogRequest *request,
                              iot_edge_v1_LogResult *result, uint32_t limit) {
    FILE *input = fopen(path, "rb");
    if (input == NULL)
        return;
    if (fseek(input, 0, SEEK_END) != 0) {
        fclose(input);
        return;
    }
    long size = ftell(input);
    if (size <= 0L) {
        fclose(input);
        return;
    }
    if ((unsigned long)size > EDGE_LOG_MAX_FILE_BYTES * 2UL)
        size = (long)(EDGE_LOG_MAX_FILE_BYTES * 2UL);
    char *buffer = malloc((size_t)size + 1U);
    if (buffer == NULL) {
        fclose(input);
        return;
    }
    if (fseek(input, -size, SEEK_END) != 0) {
        free(buffer);
        fclose(input);
        return;
    }
    const size_t read = fread(buffer, 1U, (size_t)size, input);
    fclose(input);
    buffer[read] = '\0';

    size_t end = read;
    while (end != 0U && result->lines_count < limit) {
        while (end != 0U && (buffer[end - 1U] == '\n' || buffer[end - 1U] == '\r'))
            --end;
        if (end == 0U)
            break;
        size_t start = end;
        while (start != 0U && buffer[start - 1U] != '\n')
            --start;
        buffer[end] = '\0';
        append_line(buffer + start, request, result, limit);
        end = start;
    }
    free(buffer);
}

void edge_log_init(void) {
    if (ensure_log_dir())
        enforce_space_reserve();
}

bool edge_log_set_level(const char *level) {
    if (!valid_level(level))
        return false;
    copy_text(log_level_name, sizeof(log_level_name), level);
    log_threshold = level_rank(level);
    return true;
}

const char *edge_log_level(void) {
    return log_level_name;
}

bool edge_log_enabled(const char *level) {
    return level_rank(level) >= log_threshold;
}

void edge_log_write(const char *level, const char *source, const char *message,
                    const char *detail) {
    if (!edge_log_enabled(level))
        return;
    if (!ensure_log_dir())
        return;
    if ((unsigned long)file_size(EDGE_LOG_PATH) >= EDGE_LOG_MAX_FILE_BYTES)
        rotate_logs();
    enforce_space_reserve();
    FILE *output = fopen(EDGE_LOG_PATH, "a");
    if (output == NULL)
        return;
    fprintf(output, "%" PRId64 "\t", now_ms());
    write_escaped(output, level != NULL ? level : "info", 8U);
    fputc('\t', output);
    write_escaped(output, source != NULL ? source : "node", 16U);
    fputc('\t', output);
    write_escaped(output, message, 96U);
    fputc('\t', output);
    write_escaped(output, detail, 128U);
    fputc('\n', output);
    fclose(output);
    if ((unsigned long)file_size(EDGE_LOG_PATH) >= EDGE_LOG_MAX_FILE_BYTES)
        rotate_logs();
    enforce_space_reserve();
}

void edge_log_packet(const char *source, const char *direction, const char *device,
                     const uint8_t *data, size_t size) {
    if (!edge_log_enabled("debug") || data == NULL)
        return;
    char message[96];
    snprintf(message, sizeof(message), "%s packet", direction != NULL ? direction : "io");
    char detail[160];
    const int prefix = snprintf(detail, sizeof(detail), "device=%s bytes=%zu data=",
                                device != NULL ? device : "unknown", size);
    size_t offset = prefix > 0 && (size_t)prefix < sizeof(detail) ? (size_t)prefix : 0U;
    size_t emitted = 0U;
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0U; index < size && offset + 3U < sizeof(detail); ++index) {
        if (index != 0U)
            detail[offset++] = ' ';
        detail[offset++] = digits[data[index] >> 4U];
        detail[offset++] = digits[data[index] & 0x0fU];
        ++emitted;
    }
    if (offset + 4U < sizeof(detail) && emitted < size) {
        detail[offset++] = ' ';
        detail[offset++] = '.';
        detail[offset++] = '.';
        detail[offset++] = '.';
    }
    detail[offset < sizeof(detail) ? offset : sizeof(detail) - 1U] = '\0';
    edge_log_write("debug", source, message, detail);
}

void edge_log_query(const iot_edge_v1_LogRequest *request,
                    iot_edge_v1_LogResult *result) {
    if (request == NULL || result == NULL)
        return;
    *result = (iot_edge_v1_LogResult)iot_edge_v1_LogResult_init_zero;
    if (request->request_id.size == 16U)
        edge_protocol_set_bytes(&result->request_id, sizeof(result->request_id.bytes),
                                request->request_id.bytes, 16U);
    result->success = true;
    copy_text(result->message, sizeof(result->message), "ok");
    if (!ensure_log_dir()) {
        result->success = false;
        copy_text(result->message, sizeof(result->message), "log directory unavailable");
        return;
    }

    const uint32_t limit = request->limit == 0U || request->limit > EDGE_LOG_RESULT_LIMIT
                               ? EDGE_LOG_RESULT_LIMIT
                               : request->limit;
    for (unsigned index = 0U; index < EDGE_LOG_MAX_FILES && result->lines_count < limit;
         ++index) {
        char path[128];
        log_path(index, path, sizeof(path));
        read_reverse_file(path, request, result, limit);
    }
}
