#ifndef RDBMS_TOOLS_H
#define RDBMS_TOOLS_H

#include "json_rpc.h"

void tools_handle_initialize(const char* id);
void tools_handle_discover(const char *id);
void tools_handle_list(const char *id);
void tools_handle_call(const char *id, const JsonValue *params);

#endif /* RDBMS_TOOLS_H */
