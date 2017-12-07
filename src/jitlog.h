#ifndef _LJ_JITLOG_H
#define _LJ_JITLOG_H

#include "lua.h"

typedef enum JITLogFilter {
  LOGFILTER_TRACE_COMPLETED = 0x1,
  LOGFILTER_TRACE_ABORTS    = 0x2,
  LOGFILTER_TRACE_EXITS     = 0x4,
  LOGFILTER_TRACE_DATA      = 0x8,
  LOGFILTER_PROTO_LOADED    = 0x10,
  LOGFILTER_GC_STATE        = 0x40,
} JITLogFilter;

typedef struct JITLogUserContext {
  void *userdata;
  JITLogFilter logfilter;
} JITLogUserContext;

LUA_API JITLogUserContext* jitlog_start(lua_State *L);
LUA_API void jitlog_close(JITLogUserContext *usrcontext);
LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path);
LUA_API void jitlog_reset(JITLogUserContext *usrcontext);

#endif

