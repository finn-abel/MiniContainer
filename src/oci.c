#include "oci.h"

#include "config.h"
#include "util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Cap on the config file size we are willing to read into memory. config.json
 * files are small; this bounds allocation for malformed or hostile inputs.
 */
#define OCI_MAX_FILE_SIZE (1024 * 1024)

/* Recursion guard so deeply nested input cannot overflow the stack. */
#define OCI_MAX_DEPTH 64

/*
 * A tiny JSON document model. The parser below is intentionally minimal: it
 * supports objects, arrays, strings (with common escapes and \uXXXX in the BMP),
 * numbers, booleans, and null. It is sufficient for reading an OCI config.json,
 * not a general-purpose validator.
 */
typedef enum JsonType {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct JsonMember {
    char *key;
    JsonValue *value;
    struct JsonMember *next;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
        } array;
        JsonMember *members;
    } u;
};

typedef struct JsonParser {
    const char *p;
    const char *end;
    int depth;
} JsonParser;

static JsonValue *parse_value(JsonParser *parser);

static void json_free(JsonValue *value)
{
    if (value == NULL) {
        return;
    }

    switch (value->type) {
        case JSON_STRING:
            free(value->u.string);
            break;
        case JSON_ARRAY: {
            size_t i;

            for (i = 0; i < value->u.array.count; i++) {
                json_free(value->u.array.items[i]);
            }
            free(value->u.array.items);
            break;
        }
        case JSON_OBJECT: {
            JsonMember *member = value->u.members;

            while (member != NULL) {
                JsonMember *next = member->next;

                free(member->key);
                json_free(member->value);
                free(member);
                member = next;
            }
            break;
        }
        default:
            break;
    }

    free(value);
}

static void skip_whitespace(JsonParser *parser)
{
    while (parser->p < parser->end) {
        char c = *parser->p;

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            parser->p++;
        } else {
            break;
        }
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int encode_utf8(unsigned int codepoint, char *out, size_t *out_len)
{
    /* Encode a BMP codepoint as UTF-8; surrogate halves are passed through. */
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        *out_len = 1;
    } else if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        *out_len = 2;
    } else {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        *out_len = 3;
    }
    return 0;
}

static char *parse_string_raw(JsonParser *parser)
{
    char *buffer;
    size_t capacity;
    size_t length = 0;

    if (parser->p >= parser->end || *parser->p != '"') {
        return NULL;
    }
    parser->p++;

    /* The decoded string is never longer than the source span. */
    capacity = (size_t)(parser->end - parser->p) + 1;
    buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }

    while (parser->p < parser->end) {
        char c = *parser->p++;

        if (c == '"') {
            buffer[length] = '\0';
            return buffer;
        }

        if (c != '\\') {
            buffer[length++] = c;
            continue;
        }

        if (parser->p >= parser->end) {
            break;
        }

        char escape = *parser->p++;
        switch (escape) {
            case '"': buffer[length++] = '"'; break;
            case '\\': buffer[length++] = '\\'; break;
            case '/': buffer[length++] = '/'; break;
            case 'b': buffer[length++] = '\b'; break;
            case 'f': buffer[length++] = '\f'; break;
            case 'n': buffer[length++] = '\n'; break;
            case 'r': buffer[length++] = '\r'; break;
            case 't': buffer[length++] = '\t'; break;
            case 'u': {
                unsigned int codepoint = 0;
                char encoded[4];
                size_t encoded_len = 0;
                int k;

                if (parser->end - parser->p < 4) {
                    free(buffer);
                    return NULL;
                }
                for (k = 0; k < 4; k++) {
                    int digit = hex_digit(*parser->p++);

                    if (digit < 0) {
                        free(buffer);
                        return NULL;
                    }
                    codepoint = (codepoint << 4) | (unsigned int)digit;
                }
                encode_utf8(codepoint, encoded, &encoded_len);
                memcpy(buffer + length, encoded, encoded_len);
                length += encoded_len;
                break;
            }
            default:
                free(buffer);
                return NULL;
        }
    }

    free(buffer);
    return NULL;
}

static JsonValue *new_value(JsonType type)
{
    JsonValue *value = calloc(1, sizeof(*value));

    if (value != NULL) {
        value->type = type;
    }
    return value;
}

static JsonValue *parse_string_value(JsonParser *parser)
{
    JsonValue *value;
    char *raw = parse_string_raw(parser);

    if (raw == NULL) {
        return NULL;
    }

    value = new_value(JSON_STRING);
    if (value == NULL) {
        free(raw);
        return NULL;
    }
    value->u.string = raw;
    return value;
}

static JsonValue *parse_number(JsonParser *parser)
{
    const char *start = parser->p;
    char buffer[64];
    size_t length;
    char *parse_end = NULL;
    JsonValue *value;
    double number;

    while (parser->p < parser->end) {
        char c = *parser->p;

        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            parser->p++;
        } else {
            break;
        }
    }

    length = (size_t)(parser->p - start);
    if (length == 0 || length >= sizeof(buffer)) {
        return NULL;
    }
    memcpy(buffer, start, length);
    buffer[length] = '\0';

    errno = 0;
    number = strtod(buffer, &parse_end);
    if (errno != 0 || parse_end != buffer + length) {
        return NULL;
    }

    value = new_value(JSON_NUMBER);
    if (value == NULL) {
        return NULL;
    }
    value->u.number = number;
    return value;
}

static JsonValue *parse_literal(JsonParser *parser)
{
    size_t remaining = (size_t)(parser->end - parser->p);

    if (remaining >= 4 && strncmp(parser->p, "true", 4) == 0) {
        JsonValue *value = new_value(JSON_BOOL);

        if (value != NULL) {
            value->u.boolean = true;
            parser->p += 4;
        }
        return value;
    }
    if (remaining >= 5 && strncmp(parser->p, "false", 5) == 0) {
        JsonValue *value = new_value(JSON_BOOL);

        if (value != NULL) {
            value->u.boolean = false;
            parser->p += 5;
        }
        return value;
    }
    if (remaining >= 4 && strncmp(parser->p, "null", 4) == 0) {
        JsonValue *value = new_value(JSON_NULL);

        if (value != NULL) {
            parser->p += 4;
        }
        return value;
    }
    return NULL;
}

static JsonValue *parse_array(JsonParser *parser)
{
    JsonValue *value = new_value(JSON_ARRAY);

    if (value == NULL) {
        return NULL;
    }
    parser->p++; /* consume '[' */

    skip_whitespace(parser);
    if (parser->p < parser->end && *parser->p == ']') {
        parser->p++;
        return value;
    }

    for (;;) {
        JsonValue *element = parse_value(parser);
        JsonValue **grown;

        if (element == NULL) {
            json_free(value);
            return NULL;
        }

        grown = realloc(value->u.array.items, sizeof(*grown) * (value->u.array.count + 1));
        if (grown == NULL) {
            json_free(element);
            json_free(value);
            return NULL;
        }
        value->u.array.items = grown;
        value->u.array.items[value->u.array.count++] = element;

        skip_whitespace(parser);
        if (parser->p >= parser->end) {
            json_free(value);
            return NULL;
        }
        if (*parser->p == ',') {
            parser->p++;
            continue;
        }
        if (*parser->p == ']') {
            parser->p++;
            return value;
        }
        json_free(value);
        return NULL;
    }
}

static JsonValue *parse_object(JsonParser *parser)
{
    JsonValue *value = new_value(JSON_OBJECT);

    if (value == NULL) {
        return NULL;
    }
    parser->p++; /* consume '{' */

    skip_whitespace(parser);
    if (parser->p < parser->end && *parser->p == '}') {
        parser->p++;
        return value;
    }

    for (;;) {
        char *key;
        JsonValue *member_value;
        JsonMember *member;

        skip_whitespace(parser);
        key = parse_string_raw(parser);
        if (key == NULL) {
            json_free(value);
            return NULL;
        }

        skip_whitespace(parser);
        if (parser->p >= parser->end || *parser->p != ':') {
            free(key);
            json_free(value);
            return NULL;
        }
        parser->p++;

        member_value = parse_value(parser);
        if (member_value == NULL) {
            free(key);
            json_free(value);
            return NULL;
        }

        member = calloc(1, sizeof(*member));
        if (member == NULL) {
            free(key);
            json_free(member_value);
            json_free(value);
            return NULL;
        }
        member->key = key;
        member->value = member_value;
        member->next = value->u.members;
        value->u.members = member;

        skip_whitespace(parser);
        if (parser->p >= parser->end) {
            json_free(value);
            return NULL;
        }
        if (*parser->p == ',') {
            parser->p++;
            continue;
        }
        if (*parser->p == '}') {
            parser->p++;
            return value;
        }
        json_free(value);
        return NULL;
    }
}

static JsonValue *parse_value(JsonParser *parser)
{
    JsonValue *value;
    char c;

    if (parser->depth >= OCI_MAX_DEPTH) {
        return NULL;
    }
    parser->depth++;

    skip_whitespace(parser);
    if (parser->p >= parser->end) {
        parser->depth--;
        return NULL;
    }

    c = *parser->p;
    if (c == '{') {
        value = parse_object(parser);
    } else if (c == '[') {
        value = parse_array(parser);
    } else if (c == '"') {
        value = parse_string_value(parser);
    } else if (c == 't' || c == 'f' || c == 'n') {
        value = parse_literal(parser);
    } else {
        value = parse_number(parser);
    }

    parser->depth--;
    return value;
}

static JsonValue *json_parse(const char *text, size_t length)
{
    JsonParser parser = {text, text + length, 0};
    JsonValue *root = parse_value(&parser);

    if (root == NULL) {
        return NULL;
    }

    /* Reject trailing junk so malformed documents fail rather than parse partly. */
    skip_whitespace(&parser);
    if (parser.p != parser.end) {
        json_free(root);
        return NULL;
    }
    return root;
}

static const JsonValue *object_get(const JsonValue *object, const char *key)
{
    const JsonMember *member;

    if (object == NULL || object->type != JSON_OBJECT) {
        return NULL;
    }
    for (member = object->u.members; member != NULL; member = member->next) {
        if (strcmp(member->key, key) == 0) {
            return member->value;
        }
    }
    return NULL;
}

static const char *object_get_string(const JsonValue *object, const char *key)
{
    const JsonValue *value = object_get(object, key);

    return (value != NULL && value->type == JSON_STRING) ? value->u.string : NULL;
}

static bool object_get_number(const JsonValue *object, const char *key, double *out)
{
    const JsonValue *value = object_get(object, key);

    if (value != NULL && value->type == JSON_NUMBER) {
        *out = value->u.number;
        return true;
    }
    return false;
}

static char **string_array_dup(const JsonValue *array, size_t *count_out)
{
    char **result;
    size_t count;
    size_t i;

    if (array == NULL || array->type != JSON_ARRAY || array->u.array.count == 0) {
        return NULL;
    }

    count = array->u.array.count;
    /* NULL-terminate so the vector is directly usable as argv/envp. */
    result = calloc(count + 1, sizeof(*result));
    if (result == NULL) {
        return NULL;
    }

    for (i = 0; i < count; i++) {
        const JsonValue *item = array->u.array.items[i];
        char *copy;

        if (item->type != JSON_STRING) {
            goto fail;
        }
        copy = strdup(item->u.string);
        if (copy == NULL) {
            goto fail;
        }
        result[i] = copy;
    }

    *count_out = count;
    return result;

fail:
    for (i = 0; i < count; i++) {
        free(result[i]);
    }
    free(result);
    return NULL;
}

static int resolve_root_path(const char *config_path, const char *root_path, char **out)
{
    char dir_buffer[MINICTL_MAX_PATH_SIZE];
    char joined[MINICTL_MAX_PATH_SIZE];
    const char *slash;

    /* Absolute root paths are taken as-is; relative ones join the bundle dir. */
    if (root_path[0] == '/') {
        *out = strdup(root_path);
        return *out != NULL ? 0 : -1;
    }

    slash = strrchr(config_path, '/');
    if (slash == NULL) {
        *out = strdup(root_path);
        return *out != NULL ? 0 : -1;
    }

    {
        size_t dir_len = (size_t)(slash - config_path);

        if (dir_len == 0) {
            dir_len = 1; /* config in root: keep the leading slash */
        }
        if (dir_len >= sizeof(dir_buffer)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(dir_buffer, config_path, dir_len);
        dir_buffer[dir_len] = '\0';
    }

    if (minictl_path_join(joined, sizeof(joined), dir_buffer, root_path) != 0) {
        return -1;
    }

    *out = strdup(joined);
    return *out != NULL ? 0 : -1;
}

static int map_config(const JsonValue *root, const char *config_path, OciConfig *out)
{
    const JsonValue *root_obj;
    const JsonValue *process;
    const JsonValue *linux_obj;
    const JsonValue *resources;
    const char *hostname;
    const char *root_path;

    if (root == NULL || root->type != JSON_OBJECT) {
        errno = EINVAL;
        return -1;
    }

    root_obj = object_get(root, "root");
    root_path = object_get_string(root_obj, "path");
    if (root_path != NULL && resolve_root_path(config_path, root_path, &out->rootfs) != 0) {
        return -1;
    }

    hostname = object_get_string(root, "hostname");
    if (hostname != NULL) {
        out->hostname = strdup(hostname);
        if (out->hostname == NULL) {
            return -1;
        }
    }

    process = object_get(root, "process");
    if (process != NULL) {
        out->args = string_array_dup(object_get(process, "args"), &out->args_count);
        out->env = string_array_dup(object_get(process, "env"), &out->env_count);
    }

    linux_obj = object_get(root, "linux");
    resources = object_get(linux_obj, "resources");
    if (resources != NULL) {
        const JsonValue *memory = object_get(resources, "memory");
        const JsonValue *pids = object_get(resources, "pids");
        const JsonValue *cpu = object_get(resources, "cpu");
        double limit;
        double quota;
        double period;

        if (object_get_number(memory, "limit", &limit) && limit >= 0) {
            out->limits.memory_max = (unsigned long long)limit;
            out->limits.memory_max_set = true;
        }
        if (object_get_number(pids, "limit", &limit) && limit > 0) {
            out->limits.pids_max = (long)limit;
            out->limits.pids_max_set = true;
        }
        if (object_get_number(cpu, "quota", &quota) && object_get_number(cpu, "period", &period) &&
            quota > 0 && period > 0) {
            out->limits.cpu_quota = (long)quota;
            out->limits.cpu_period = (long)period;
            out->limits.cpu_max_set = true;
        }
    }

    return 0;
}

static char *read_file(const char *path, size_t *length_out)
{
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;
    size_t read_bytes;

    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > OCI_MAX_FILE_SIZE) {
        fclose(file);
        errno = size < 0 ? errno : EFBIG;
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    read_bytes = fread(buffer, 1, (size_t)size, file);
    if (read_bytes != (size_t)size && ferror(file)) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);

    buffer[read_bytes] = '\0';
    *length_out = read_bytes;
    return buffer;
}

int oci_load_config(const char *path, OciConfig *out)
{
    char *text;
    size_t length;
    JsonValue *root;

    if (path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, sizeof(*out));

    text = read_file(path, &length);
    if (text == NULL) {
        return -1;
    }

    root = json_parse(text, length);
    free(text);
    if (root == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (map_config(root, path, out) != 0) {
        int saved_errno = errno;

        json_free(root);
        oci_config_free(out);
        errno = saved_errno;
        return -1;
    }

    json_free(root);
    return 0;
}

void oci_config_free(OciConfig *config)
{
    size_t i;

    if (config == NULL) {
        return;
    }

    free(config->rootfs);
    free(config->hostname);
    if (config->args != NULL) {
        for (i = 0; i < config->args_count; i++) {
            free(config->args[i]);
        }
        free(config->args);
    }
    if (config->env != NULL) {
        for (i = 0; i < config->env_count; i++) {
            free(config->env[i]);
        }
        free(config->env);
    }

    memset(config, 0, sizeof(*config));
}
