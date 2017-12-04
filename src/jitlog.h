#ifndef _LJ_JITLOG_H
#define _LJ_JITLOG_H

#include "lua.h"

typedef struct JITLogUserContext {
  void *userdata;
} JITLogUserContext;

LUA_API JITLogUserContext* jitlog_start(lua_State *L);
LUA_API void jitlog_close(JITLogUserContext *usrcontext);
LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path);
LUA_API void jitlog_reset(JITLogUserContext *usrcontext);

#endif

