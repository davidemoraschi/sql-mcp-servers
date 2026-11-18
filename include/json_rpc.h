/*
 * json_rpc.h - JSON-RPC 2.0 Lexer & Parser Header
 *
 * Line budget constraint: < 150 lines
 */

#ifndef JSON_RPC_H
#define JSON_RPC_H

#ifndef _CRT_SECURE_NO_WARNINGS
  #define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_DEPRECATE
  #define _CRT_NONSTDC_NO_DEPRECATE
#endif

#include <stdbool.h>
#include <stddef.h>

#define JSONRPC_ERR_PARSE                  -32700
#define JSONRPC_ERR_INVALID_REQUEST        -32600
#define JSONRPC_ERR_METHOD_NOT_FOUND       -32601
#define JSONRPC_ERR_INVALID_PARAMS         -32602
#define JSONRPC_ERR_INTERNAL               -32603

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

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
        struct {
            char **keys;
            JsonValue **values;
            size_t count;
        } object;
    } u;
};

typedef struct {
    char id[64];
    char method[128];
    const JsonValue *params;
} JsonRpcRequest;

JsonValue *json_parse(const char *json_str);
void json_free(JsonValue *val);
JsonValue *json_object_get(const JsonValue *obj, const char *key);
const char *json_string_value(const JsonValue *val);
double json_number_value(const JsonValue *val);

bool json_rpc_parse_request(const char *input, JsonRpcRequest *req);
void json_rpc_free_request(JsonRpcRequest *req);

void json_rpc_send_result(const char *id, const char *result_json);
void json_rpc_send_error(const char *id, int code, const char *message, const char *data_json);

#endif /* JSON_RPC_H */
