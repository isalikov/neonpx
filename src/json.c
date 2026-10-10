#include "json.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

#define JSON_MAX_DEPTH 128

typedef struct {
    const char *s;
    size_t n, i;
    int depth;
} parser;

static bool parse_value(parser *p, json *v);

static void skip_ws(parser *p) {
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->i++;
    }
}

static bool literal(parser *p, const char *word) {
    size_t l = strlen(word);
    if (p->n - p->i < l || memcmp(p->s + p->i, word, l) != 0) return false;
    p->i += l;
    return true;
}

static bool hex4(parser *p, uint32_t *out) {
    if (p->n - p->i < 4) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[p->i + (size_t)k];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
        else
            return false;
    }
    p->i += 4;
    *out = v;
    return true;
}

static bool parse_string(parser *p, char **out, size_t *outlen) {
    p->i++; /* opening quote */
    size_t cap = 16, len = 0;
    char *buf = xmalloc(cap);
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') {
            buf[len] = '\0';
            *out = buf;
            if (outlen) *outlen = len;
            return true;
        }
        if (c < 0x20) break;
        char tmp[4];
        int tl = 1;
        if (c != '\\') {
            tmp[0] = (char)c;
        } else {
            if (p->i >= p->n) break;
            char e = p->s[p->i++];
            switch (e) {
            case '"': tmp[0] = '"'; break;
            case '\\': tmp[0] = '\\'; break;
            case '/': tmp[0] = '/'; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'n': tmp[0] = '\n'; break;
            case 'r': tmp[0] = '\r'; break;
            case 't': tmp[0] = '\t'; break;
            case 'u': {
                uint32_t cp, lo;
                if (!hex4(p, &cp)) goto fail;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p->n - p->i >= 6 && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                        p->i += 2;
                        if (!hex4(p, &lo)) goto fail;
                        cp = (lo >= 0xDC00 && lo <= 0xDFFF) ? 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00) : 0xFFFD;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                tl = utf8_encode(cp, tmp);
                break;
            }
            default: goto fail;
            }
        }
        if (len + (size_t)tl + 1 > cap) {
            while (len + (size_t)tl + 1 > cap)
                cap *= 2;
            buf = xrealloc(buf, cap);
        }
        memcpy(buf + len, tmp, (size_t)tl);
        len += (size_t)tl;
    }
fail:
    free(buf);
    return false;
}

static bool parse_number(parser *p, json *v) {
    size_t start = p->i;
    while (p->i < p->n && p->s[p->i] && strchr("0123456789.eE+-", p->s[p->i]))
        p->i++;
    size_t l = p->i - start;
    if (l == 0 || l > 63) return false;
    char tmp[64];
    memcpy(tmp, p->s + start, l);
    tmp[l] = '\0';
    char *end;
    v->num = strtod(tmp, &end);
    if (*end) return false;
    v->type = JSON_NUM;
    return true;
}

static bool parse_array(parser *p, json *v) {
    v->type = JSON_ARR;
    p->i++;
    if (++p->depth > JSON_MAX_DEPTH) return false;
    size_t cap = 0;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') {
        p->i++;
        p->depth--;
        return true;
    }
    for (;;) {
        if (v->len == cap) {
            cap = cap ? cap * 2 : 4;
            v->items = xrealloc(v->items, cap * sizeof(json));
        }
        json *item = &v->items[v->len++];
        memset(item, 0, sizeof *item);
        if (!parse_value(p, item)) return false;
        skip_ws(p);
        if (p->i >= p->n) return false;
        char c = p->s[p->i++];
        if (c == ']') break;
        if (c != ',') return false;
    }
    p->depth--;
    return true;
}

static bool parse_object(parser *p, json *v) {
    v->type = JSON_OBJ;
    p->i++;
    if (++p->depth > JSON_MAX_DEPTH) return false;
    size_t cap = 0;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') {
        p->i++;
        p->depth--;
        return true;
    }
    for (;;) {
        skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != '"') return false;
        char *key;
        if (!parse_string(p, &key, NULL)) return false;
        if (v->len == cap) {
            cap = cap ? cap * 2 : 4;
            v->items = xrealloc(v->items, cap * sizeof(json));
            v->keys = xrealloc(v->keys, cap * sizeof(char *));
        }
        v->keys[v->len] = key;
        json *item = &v->items[v->len++];
        memset(item, 0, sizeof *item);
        skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != ':') return false;
        p->i++;
        if (!parse_value(p, item)) return false;
        skip_ws(p);
        if (p->i >= p->n) return false;
        char c = p->s[p->i++];
        if (c == '}') break;
        if (c != ',') return false;
    }
    p->depth--;
    return true;
}

static bool parse_value(parser *p, json *v) {
    memset(v, 0, sizeof *v);
    skip_ws(p);
    if (p->i >= p->n) return false;
    switch (p->s[p->i]) {
    case '{': return parse_object(p, v);
    case '[': return parse_array(p, v);
    case '"': v->type = JSON_STR; return parse_string(p, &v->str, &v->len);
    case 't':
        v->type = JSON_BOOL;
        v->boolean = true;
        return literal(p, "true");
    case 'f': v->type = JSON_BOOL; return literal(p, "false");
    case 'n': v->type = JSON_NULL; return literal(p, "null");
    default: return parse_number(p, v);
    }
}

static void json_clear(json *v) {
    if (v->type == JSON_STR) free(v->str);
    if (v->type == JSON_ARR || v->type == JSON_OBJ) {
        for (size_t i = 0; i < v->len; i++) {
            json_clear(&v->items[i]);
            if (v->keys) free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
    }
}

json *json_parse(const char *src, size_t n) {
    parser p = {src, n, 0, 0};
    json *root = xmalloc(sizeof *root);
    bool ok = parse_value(&p, root);
    if (ok) {
        skip_ws(&p);
        ok = p.i == p.n;
    }
    if (!ok) {
        json_clear(root);
        free(root);
        return NULL;
    }
    return root;
}

void json_free(json *v) {
    if (!v) return;
    json_clear(v);
    free(v);
}

const json *json_get(const json *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJ) return NULL;
    for (size_t i = 0; i < obj->len; i++)
        if (strcmp(obj->keys[i], key) == 0) return &obj->items[i];
    return NULL;
}

const json *json_at(const json *arr, size_t i) {
    if (!arr || arr->type != JSON_ARR || i >= arr->len) return NULL;
    return &arr->items[i];
}

const json *json_path(const json *v, const char *dotted) {
    char seg[128];
    while (v && *dotted) {
        const char *dot = strchr(dotted, '.');
        size_t l = dot ? (size_t)(dot - dotted) : strlen(dotted);
        str_ncopy(seg, sizeof seg, dotted, l);
        v = json_get(v, seg);
        dotted += l + (dot ? 1 : 0);
    }
    return v;
}

size_t json_len(const json *v) { return v && (v->type == JSON_ARR || v->type == JSON_OBJ) ? v->len : 0; }

const char *json_string(const json *v, const char *fallback) { return v && v->type == JSON_STR ? v->str : fallback; }

double json_number(const json *v, double fallback) { return v && v->type == JSON_NUM ? v->num : fallback; }
