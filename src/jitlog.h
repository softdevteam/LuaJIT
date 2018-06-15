#ifndef _LJ_JITLOG_H
#define _LJ_JITLOG_H

#include "lua.h"
#include "lj_usrbuf.h"

typedef enum JITLogFilter {
  LOGFILTER_TRACE_COMPLETED = 0x1,
  LOGFILTER_TRACE_ABORTS    = 0x2,
  LOGFILTER_TRACE_IR        = 0x4, /* Exclude IR from trace messages */
  LOGFILTER_TRACE_MCODE     = 0x8, /* Exclude machine code from trace messages */
  /* Exclude ALL extra data from trace messages */ 
  LOGFILTER_TRACE_DATA      = LOGFILTER_TRACE_IR | LOGFILTER_TRACE_MCODE,

  LOGFILTER_TRACE_EXITS     = 0x10,
  LOGFILTER_GC_STATE        = 0x20,
  LOGFILTER_PROTO_LOADED    = 0x40,
  LOGFILTER_PROTO_LOADONLY  = 0x80, /* Don't try to memorize\log GCproto's except when there first loaded */
} JITLogFilter;

typedef struct JITLogUserContext {
  void *userdata;
  JITLogFilter logfilter;
} JITLogUserContext;

LUA_API JITLogUserContext* jitlog_start(lua_State *L);
LUA_API void jitlog_close(JITLogUserContext *usrcontext);
LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path);
LUA_API void jitlog_reset(JITLogUserContext *usrcontext);
LUA_API void jitlog_savehotcounts(JITLogUserContext *usrcontext);

#endif

