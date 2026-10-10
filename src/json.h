#ifndef NEONPX_JSON_H
#define NEONPX_JSON_H

#include <stdbool.h>
#include <stddef.h>

/* Minimal DOM JSON parser: just enough for the Docker Engine API. */

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } json_type;

typedef struct json json;
struct json {
    json_type type;
    bool boolean;
    double num;
    char *str;   /* JSON_STR: NUL-terminated value */
    size_t len;  /* string length, or item count for arrays/objects */
    json *items; /* array elements / object values */
    char **keys; /* object keys, parallel to items */
};

/* Parses n bytes of src. Returns NULL on malformed input or nesting deeper than 128. */
json *json_parse(const char *src, size_t n);
void json_free(json *v);

const json *json_get(const json *obj, const char *key);
const json *json_at(const json *arr, size_t i);
/* Dotted lookup through nested objects: "memory_stats.stats.inactive_file". */
const json *json_path(const json *v, const char *dotted);
size_t json_len(const json *v);
const char *json_string(const json *v, const char *fallback);
double json_number(const json *v, double fallback);

#endif
