#define LUA_CORE

#include "lj_jit.h"
#include "lj_vm.h"
#include "lj_lib.h"
#include "lj_trace.h"
#include "lj_tab.h"
#include "lj_gc.h"
#include "lj_buf.h"
#include "lj_vmevent.h"
#include "lj_debug.h"
#include "luajit.h"
#include "lauxlib.h"

#include "lj_jitlog_def.h"
#include "lj_jitlog_writers.h"
#include "jitlog.h"


typedef struct JITLogState {
  SBuf eventbuf; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
} JITLogState;

#define usr2ctx(usrcontext)  ((JITLogState *)(((char *)usrcontext) - offsetof(JITLogState, user)))
#define ctx2usr(context)  (&(context)->user)

#if LJ_HASJIT

static const uint32_t large_traceid = 1 << 14;
static const uint32_t large_exitnum = 1 << 9;

static void jitlog_exit(JITLogState *context, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  /* Use a more the compact message if the trace Id is smaller than 16k and the exit smaller than 
  ** 512 which will fit in the spare 24 bits of a message header.
  */
  if (J->parent < large_traceid && J->exitno < large_exitnum) {
    log_traceexit_small(context->g, exitState->gcexit, J->parent, J->exitno);
  } else {
    log_traceexit(context->g, exitState->gcexit, J->parent, J->exitno);
  }
}

static void jitlog_traceflush(JITLogState *context, FlushReason reason)
{
  jit_State *J = G2J(context->g);
  log_alltraceflush(context->g, reason, J->param[JIT_P_maxtrace], J->param[JIT_P_maxmcode] << 10);
}

#endif
static void free_context(JITLogState *context);

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  JITLogState *context = contextptr;

  switch (event) {
#if LJ_HASJIT
    case VMEVENT_TRACE_EXIT:
      jitlog_exit(context, (VMEventData_TExit*)eventdata);
      break;
    case VMEVENT_TRACE_FLUSH:
      jitlog_traceflush(context, (FlushReason)(uintptr_t)eventdata);
      break;
#endif
    case VMEVENT_DETACH:
      free_context(context);
      break;
    case VMEVENT_STATE_CLOSING:
      if (G(L)->vmevent_cb == jitlog_callback) {
        luaJIT_vmevent_sethook(L, NULL, NULL);
      }
      free_context(context);
      break;
    default:
      break;
  }
}

#if LJ_TARGET_X86ORX64

static int getcpumodel(char *model)
{
  lj_vm_cpuid(0x80000002u, (uint32_t*)(model));
  lj_vm_cpuid(0x80000003u, (uint32_t*)(model + 16));
  lj_vm_cpuid(0x80000004u, (uint32_t*)(model + 32));
  return (int)strnlen((char*)model, 12 * 4);
}

#else

static int getcpumodel(char *model)
{
  strcpy(model, "unknown");
  return (int)strlen("unknown");
}

#endif

static int bufwrite_strlist(SBuf *sb, const char** list, int limit)
{
  const char** pos = list;
  int count = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    lj_buf_putmem(sb, s, (MSize)strlen(s)+1);
  }
  return count;
}

static void write_header(JITLogState *context)
{
  lua_State *L = mainthread(context->g);
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  SBuf sb;
  lj_buf_init(L, &sb);
  bufwrite_strlist(&sb, msgnames, MSGTYPE_MAX);
  log_header(context->g, 1, 0, sizeof(MSG_header), msgsizes, MSGTYPE_MAX, sbufB(&sb), sbuflen(&sb), cpumodel, model_length, LJ_OS_NAME, (uintptr_t)G2GG(context->g));
  lj_buf_free(context->g, &sb);
}

static int jitlog_isrunning(lua_State *L)
{
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  return cb == jitlog_callback;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

LUA_API JITLogUserContext* jitlog_start(lua_State *L)
{
  JITLogState *context ;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(JITLogState));
  memset(context, 0 , sizeof(JITLogState));
  context->g = G(L);

  MSize total = G(L)->gc.total;
  SBuf *sb = &context->eventbuf;
  lj_buf_init(L, sb);
  lj_buf_more(sb, 1024*1024*100);
  /* 
  ** Our Buffer size counts towards the gc heap size. This is an ugly hack to try and 
  ** keep the GC running at the same rate with the jitlog running by excluding our buffer
  ** from the gcheap size.
  */
  G(L)->gc.total = total;
  luaJIT_vmevent_sethook(L, jitlog_callback, context);
  write_header(context);

  lj_lib_prereg(L, "jitlog", luaopen_jitlog, tabref(L->env));
  return &context->user;
}

static void free_context(JITLogState *context)
{
  global_State *g = context->g;
  const char *path = getenv("JITLOG_PATH");
  if (path != NULL) {
    jitlog_save(ctx2usr(context), path);
  }

  g->gc.total += sbufsz(&context->eventbuf);
  lj_buf_free(g, &context->eventbuf);
  free(context);
}

static void jitlog_shutdown(JITLogState *context)
{
  lua_State *L = mainthread(context->g);
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  if (cb == jitlog_callback) {
    lua_assert(current_context == context);
    luaJIT_vmevent_sethook(L, NULL, NULL);
  }

  free_context(context);
}

LUA_API void jitlog_close(JITLogUserContext *usrcontext)
{
  JITLogState *context = usr2ctx(usrcontext);
  jitlog_shutdown(context);
}

LUA_API void jitlog_reset(JITLogUserContext *usrcontext)
{
  JITLogState *context = usr2ctx(usrcontext);
  lj_buf_reset(&context->eventbuf);
  write_header(context);
}

LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path)
{
  JITLogState *context = usr2ctx(usrcontext);
  SBuf *sb = &context->eventbuf;
  int result = 0;
  lua_assert(path && path[0]);

  FILE* dumpfile = fopen(path, "wb");
  if (dumpfile == NULL) {
    return -errno;
  }

  size_t written = fwrite(sbufB(sb), 1, sbuflen(sb), dumpfile);
  if (written != sbuflen(sb) && ferror(dumpfile)) {
    result = -errno;
  } else {
    int status = fflush(dumpfile);
    if (status != 0 && ferror(dumpfile)) {
      result = -errno;
    }
  }
  fclose(dumpfile);
  return result;
}

/* -- Lua module to control the JITLog ------------------------------------ */

static JITLogState* jlib_getstate(lua_State *L)
{
  JITLogState *context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&context);
  if (cb != jitlog_callback) {
    luaL_error(L, "The JITLog is not currently running");
  }
  return context;
}

static int jlib_start(lua_State *L)
{
  if (jitlog_isrunning(L)) {
    return 0;
  }
  jitlog_start(L);
  return 0;
}

static int jlib_shutdown(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  jitlog_shutdown(context);
  return 0;
}

static int jlib_addmarker(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  size_t size = 0;
  const char *label = luaL_checklstring(L, 1, &size);
  int flags = luaL_optint(L, 2, 0);
  log_stringmarker(context->g, flags, label);
  return 0;
}

static int jlib_reset(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  jitlog_reset(ctx2usr(context));
  return 0;
}

static int jlib_save(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  const char *path = luaL_checkstring(L, 1);
  int result = jitlog_save(ctx2usr(context), path);
  if (result != 0) {
    luaL_error(L, "Failed to save JITLog. last error %d", result);
  }
  return 0;
}

static int jlib_savetostring(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  lua_pushlstring(L, sbufB(&context->eventbuf), sbuflen(&context->eventbuf));
  return 1;
}

static int jlib_getsize(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  SBuf *sb = &context->eventbuf;
  lua_pushnumber(L, sbuflen(sb));
  return 1;
}

static const luaL_Reg jitlog_lib[] = {
  {"start", jlib_start},
  {"shutdown", jlib_shutdown},
  {"reset", jlib_reset},
  {"save", jlib_save},
  {"savetostring", jlib_savetostring},
  {"getsize", jlib_getsize},
  {"addmarker", jlib_addmarker},
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
