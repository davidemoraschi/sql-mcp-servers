/*
 * json_rpc.c - JSON-RPC 2.0 Lexer, Parser, and Serializer
 *
 * Line budget constraint: < 450 lines
 */

#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_DEPRECATE
  #define _CRT_NONSTDC_NO_DEPRECATE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "json_rpc.h"

static const char *skip_whitespace(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Parse 4 hex digits; returns false if any is not a hex digit. */
static bool parse_hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

/* Encode a code point as UTF-8; returns the number of bytes written (<= 4). */
static size_t utf8_encode(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static char *parse_string_token(const char **src) {
    const char *p = *src;
    if (*p != '"') return NULL;
    p++;
    const char *start = p;
    size_t len = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) p++;
        p++;
        len++;
    }
    if (*p != '"') return NULL;
    
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    
    p = start;
    size_t idx = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == 'n') out[idx++] = '\n';
            else if (*p == 'r') out[idx++] = '\r';
            else if (*p == 't') out[idx++] = '\t';
            else if (*p == 'b') out[idx++] = '\b';
            else if (*p == 'f') out[idx++] = '\f';
            else if (*p == 'u') {
                unsigned cp;
                if (parse_hex4(p + 1, &cp)) {
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                        unsigned lo;
                        if (parse_hex4(p + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* lone surrogate */
                    if (cp != 0) idx += utf8_encode(cp, out + idx);
                } else {
                    out[idx++] = *p;
                }
            }
            else out[idx++] = *p;
        } else {
            out[idx++] = *p;
        }
        p++;
    }
    out[idx] = '\0';
    *src = p + 1;
    return out;
}

JsonValue *json_parse_value(const char **src) {
    const char *p = skip_whitespace(*src);
    if (!*p) return NULL;

    JsonValue *val = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (!val) return NULL;

    if (*p == '"') {
        val->type = JSON_STRING;
        val->u.string = parse_string_token(&p);
        *src = p;
        return val;
    } else if (*p == '{') {
        val->type = JSON_OBJECT;
        p++;
        p = skip_whitespace(p);
        while (*p && *p != '}') {
            const char *before = p;
            char *key = parse_string_token(&p);
            if (!key || p == before) break;  /* truncated: stop */
            p = skip_whitespace(p);
            if (*p == ':') p++;
            JsonValue *v = json_parse_value(&p);

            val->u.object.keys = realloc(val->u.object.keys, sizeof(char *) * (val->u.object.count + 1));
            val->u.object.values = realloc(val->u.object.values, sizeof(JsonValue *) * (val->u.object.count + 1));
            val->u.object.keys[val->u.object.count] = key;
            val->u.object.values[val->u.object.count] = v;
            val->u.object.count++;

            p = skip_whitespace(p);
            if (*p == ',') p++;
            p = skip_whitespace(p);
        }
        if (*p == '}') p++;
        *src = p;
        return val;
    } else if (*p == '[') {
        val->type = JSON_ARRAY;
        p++;
        p = skip_whitespace(p);
        while (*p && *p != ']') {
            const char *before = p;
            JsonValue *v = json_parse_value(&p);
            if (!v || p == before) break;  /* truncated / unparseable: stop, don't loop forever */
            val->u.array.items = realloc(val->u.array.items, sizeof(JsonValue *) * (val->u.array.count + 1));
            val->u.array.items[val->u.array.count++] = v;
            p = skip_whitespace(p);
            if (*p == ',') p++;
            p = skip_whitespace(p);
        }
        if (*p == ']') p++;
        *src = p;
        return val;
    } else if (isdigit((unsigned char)*p) || *p == '-') {
        val->type = JSON_NUMBER;
        val->u.number = strtod(p, (char **)&p);
        *src = p;
        return val;
    } else if (strncmp(p, "true", 4) == 0) {
        val->type = JSON_BOOL;
        val->u.boolean = true;
        *src = p + 4;
        return val;
    } else if (strncmp(p, "false", 5) == 0) {
        val->type = JSON_BOOL;
        val->u.boolean = false;
        *src = p + 5;
        return val;
    } else if (strncmp(p, "null", 4) == 0) {
        val->type = JSON_NULL;
        *src = p + 4;
        return val;
    }

    free(val);
    return NULL;
}

JsonValue *json_parse(const char *json_str) {
    const char *p = json_str;
    return json_parse_value(&p);
}

void json_free(JsonValue *val) {
    if (!val) return;
    if (val->type == JSON_STRING) {
        free(val->u.string);
    } else if (val->type == JSON_ARRAY) {
        for (size_t i = 0; i < val->u.array.count; i++) {
            json_free(val->u.array.items[i]);
        }
        free(val->u.array.items);
    } else if (val->type == JSON_OBJECT) {
        for (size_t i = 0; i < val->u.object.count; i++) {
            free(val->u.object.keys[i]);
            json_free(val->u.object.values[i]);
        }
        free(val->u.object.keys);
        free(val->u.object.values);
    }
    free(val);
}

JsonValue *json_object_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.keys[i], key) == 0) {
            return obj->u.object.values[i];
        }
    }
    return NULL;
}

const char *json_string_value(const JsonValue *val) {
    return (val && val->type == JSON_STRING) ? val->u.string : NULL;
}

double json_number_value(const JsonValue *val) {
    return (val && val->type == JSON_NUMBER) ? val->u.number : 0.0;
}

bool json_rpc_parse_request(const char *input, JsonRpcRequest *req) {
    if (!input || !req) return false;
    memset(req, 0, sizeof(JsonRpcRequest));

    JsonValue *root = json_parse(input);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        return false;
    }

    JsonValue *v_id = json_object_get(root, "id");
    JsonValue *v_method = json_object_get(root, "method");
    JsonValue *v_params = json_object_get(root, "params");

    if (v_id) {
        if (v_id->type == JSON_STRING) {
            snprintf(req->id, sizeof(req->id), "%s", v_id->u.string);
        } else if (v_id->type == JSON_NUMBER) {
            snprintf(req->id, sizeof(req->id), "%.0f", v_id->u.number);
        }
    } else {
        snprintf(req->id, sizeof(req->id), "null");
    }

    if (v_method && v_method->type == JSON_STRING) {
        snprintf(req->method, sizeof(req->method), "%s", v_method->u.string);
    } else {
        json_free(root);
        return false;
    }

    if (v_params) {
        for (size_t i = 0; i < root->u.object.count; i++) {
            if (root->u.object.values[i] == v_params) {
                root->u.object.values[i] = NULL;
                break;
            }
        }
    }
    req->params = v_params;
    json_free(root);
    return true;
}

void json_rpc_free_request(JsonRpcRequest *req) {
    if (req && req->params) {
        json_free((JsonValue *)req->params);
        req->params = NULL;
    }
}

void json_rpc_send_result(const char *id, const char *result_json) {
    fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%s%s%s,\"result\":%s}\n",
            (id && id[0] && strcmp(id, "null") != 0 && !isdigit((unsigned char)id[0])) ? "\"" : "",
            id ? id : "null",
            (id && id[0] && strcmp(id, "null") != 0 && !isdigit((unsigned char)id[0])) ? "\"" : "",
            result_json ? result_json : "{}");
    fflush(stdout);
}

void json_rpc_send_error(const char *id, int code, const char *message, const char *data_json) {
    fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%s%s%s,\"error\":{\"code\":%d,\"message\":\"%s\"%s%s}}\n",
            (id && id[0] && strcmp(id, "null") != 0 && !isdigit((unsigned char)id[0])) ? "\"" : "",
            id ? id : "null",
            (id && id[0] && strcmp(id, "null") != 0 && !isdigit((unsigned char)id[0])) ? "\"" : "",
            code, message ? message : "Error",
            data_json ? ",\"data\":" : "",
            data_json ? data_json : "");
    fflush(stdout);
}
