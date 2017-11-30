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
  GCtab *strings;
  uint32_t strcount;
  GCtab *protos;
  uint32_t protocount;
} JITLogState;

#define usr2ctx(usrcontext)  ((JITLogState *)(((char *)usrcontext) - offsetof(JITLogState, user)))
#define ctx2usr(context)  (&(context)->user)

static GCtab* create_pinnedtab(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 0);
  TValue key;
  setlightudV(&key, t);
  settabV(L, lj_tab_set(L, tabV(registry(L)), &key), t);
  lj_gc_anybarriert(L, tabV(registry(L)));
  return t;
}

static GCtab* free_pinnedtab(lua_State *L, GCtab *t)
{
  TValue key;
  TValue *slot;
  setlightudV(&key, t);
  slot = lj_tab_set(L, tabV(&G(L)->registrytv), &key);
  lua_assert(tabV(slot) == t);
  setnilV(slot);
  return t;
}

static int bufwrite_strlist(SBuf *sb, const char *const *list, int limit)
{
  const char *const *pos = list;
  int count = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    lj_buf_putmem(sb, s, (MSize)strlen(s)+1);
  }
  return count;
}

static void write_enumdef(JITLogState *context, const char *name, const char *const *names, uint32_t namecount, int isbitflags)
{
  SBuf sb;
  global_State *g = context->g;
  lj_buf_init(mainthread(g), &sb);
  int count = bufwrite_strlist(&sb, names, namecount);
  lua_assert(namecount == count);
  log_enumdef(g, isbitflags, name, count, sbufB(&sb), sbuflen(&sb));
  lj_buf_free(g, &sb);
}

static int memorize_gcref(lua_State *L,  GCtab* t, TValue* key, uint32_t *count) {
  TValue *slot = lj_tab_set(L, t, key);
  
  if (tvisnil(slot) || !lj_obj_equal(key, slot+1)) {
    int id = (*count)++;
    setlightudV(slot, (void*)(uintptr_t)id);
    return 1;
  }
  return 0;
}

static int memorize_string(JITLogState *context, GCstr *s)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  setstrV(L, &key, s);

  if (s->len > 256) {
    /*TODO: don't keep around large strings */
  }

  if (memorize_gcref(L, context->strings, &key, &context->strcount)) {
    log_gcstring(context->g, s, strdata(s));
    return 1;
  }
  return 0;
}

static const uint8_t* collectvarinfo(GCproto* pt)
{
  const char *p = (const char *)proto_varinfo(pt);
  if (p) {
    BCPos lastpc = 0;
    for (;;) {
      const char *name = p;
      uint32_t vn = *(const uint8_t *)p;
      BCPos startpc, endpc;
      if (vn < VARNAME__MAX) {
        if (vn == VARNAME_END) break;  /* End of varinfo. */
      } else {
        name = p;
        do { p++; } while (*(const uint8_t *)p);  /* Skip over variable name. */
      }
      p++;
      lastpc = startpc = lastpc + lj_buf_ruleb128(&p);
      endpc = startpc + lj_buf_ruleb128(&p);
      /* TODO: save in to an easier to parse format */
      UNUSED(lastpc);
      UNUSED(endpc);

        if (vn < VARNAME__MAX) {
          #define VARNAMESTR(name, str)	str "\0"
          name = VARNAMEDEF(VARNAMESTR);
          #undef VARNAMESTR
          if (--vn) while (*name++ || --vn);
        }
        //return name;
    }
  }
  return (const uint8_t *)p;

}

static void memorize_proto(JITLogState *context, GCproto *pt)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  int i;
  memorize_string(context, strref(pt->chunkname));
  setprotoV(L, &key, pt);
  /* Only write each proto once to the jitlog */
  if (!memorize_gcref(L, context->protos, &key, &context->protocount)) {
    return;
  }

  uint8_t *lineinfo = mref(pt->lineinfo, uint8_t);
  uint32_t linesize = 0;
  if (mref(pt->lineinfo, void)) {
    if (pt->numline < 256) {
      linesize = pt->sizebc;
    } else if (pt->numline < 65536) {
      linesize = pt->sizebc * sizeof(uint16_t);
    } else {
      linesize = pt->sizebc * sizeof(uint32_t);
    }
  }

  size_t vinfosz = collectvarinfo(pt)-proto_varinfo(pt);
  lua_assert(vinfosz < 0xffffffff);

  for(i = 0; i != pt->sizekgc; i++){
    GCobj *o = proto_kgc(pt, -(i + 1));
    /* We want the string constants to be able to tell what fields are being accessed by the bytecode */
    if (o->gch.gct == ~LJ_TSTR) {
      memorize_string(context, gco2str(o));
    }
  }

  log_gcproto(context->g, pt, proto_bc(pt), proto_bc(pt), mref(pt->k, GCRef),  lineinfo, linesize, proto_varinfo(pt), (uint32_t)vinfosz);
}

#if LJ_HASJIT

static const uint32_t large_traceid = 1 << 14;
static const uint32_t large_exitnum = 1 << 9;

static void jitlog_exit(JITLogState *context, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  if (context->user.logfilter & LOGFILTER_TRACE_EXITS) {
    return;
  }
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

static void jitlog_gcstate(JITLogState *context, int newstate)
{
  global_State *g = context->g;
  if (context->user.logfilter & LOGFILTER_GC_STATE) {
    return;
  }
  log_gcstate(g, newstate, g->gc.state, g->gc.total, g->strnum);
}

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
    case VMEVENT_GC_STATECHANGE:
      jitlog_gcstate(context, (int)(uintptr_t)eventdata);
      break;
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

static const char *const gcstates[] = {
  "pause", 
  "propagate", 
  "atomic", 
  "sweepstring", 
  "sweep", 
  "finalize",
};

static const char *const flushreason[] = {
  "other",
  "user_requested",
  "maxmcode",
  "maxtrace",
  "profile_toggle",
  "set_builtinmt",
  "set_immutableuv",
};

static const char *const bc_names[] = {
  #define BCNAME(name, ma, mb, mc, mt)       #name,
  BCDEF(BCNAME)
  #undef BCNAME
};

#define write_enum(context, name, strarray) write_enumdef(context, name, strarray, (sizeof(strarray)/sizeof(strarray[0])), 0)

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

  write_enum(context, "gcstate", gcstates);
  write_enum(context, "flushreason", flushreason);
  write_enum(context, "bc", bc_names);
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
  context->strings = create_pinnedtab(L);
  context->protos = create_pinnedtab(L);

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

  free_pinnedtab(L, context->strings);
  free_pinnedtab(L, context->protos);
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
  context->strcount = 0;
  context->protocount = 0;
  lj_tab_clear(context->strings);
  lj_tab_clear(context->protos);
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
