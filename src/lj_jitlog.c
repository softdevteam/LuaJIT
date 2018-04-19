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
#include "lj_ircall.h"
#include "luajit.h"
#include "lauxlib.h"
#include "lj_err.h"
#include "jitlog.h"
#include "lj_jitlog_def.h"

static SBuf *jitlog_buf_putmem(SBuf *sb, const void *q, MSize len);
static char *jitlog_buf_more(SBuf* sb, MSize sz);
#include "lj_jitlog_writers.h"


typedef struct JITLogState {
  SBuf eventbuf; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
  GCtab *strings;
  uint32_t strcount;
  GCtab *protos;
  uint32_t protocount;
  GCtab *funcs;
  uint32_t funccount;
  GCfunc *startfunc;
  GCproto *lastpt;
  BCPos lastpc;
  GCfunc *lastlua;
  GCfunc *lastfunc;
  uint16_t lasthotcounts[HOTCOUNT_SIZE];
  char safestarted;
} JITLogState;

static void buf_grow(SBuf *sb, MSize sz)
{
  global_State *g = ((JITLogState *)sb)->g;
  MSize osz = sbufsz(sb), len = sbuflen(sb), nsz = osz;
  char *b;
  if (nsz < LJ_MIN_SBUF) nsz = LJ_MIN_SBUF;
  while (nsz < sz) nsz += nsz;
  b = g->allocf(g->allocd, sbufB(sb), osz, nsz);
  if (!b) {
    lj_err_mem(sbufL(sb));
  }
  setmref(sb->b, b);
  setmref(sb->p, b + len);
  setmref(sb->e, b + nsz);
}

static LJ_NOINLINE char *buf_more2(SBuf *sb, MSize sz)
{
  MSize len = sbuflen(sb);
  lua_assert(sz > sbufleft(sb));
  if (LJ_UNLIKELY(sz > LJ_MAX_BUF || len + sz > LJ_MAX_BUF))
    lj_err_mem(sbufL(sb));
  buf_grow(sb, len + sz);
  return sbufP(sb);
}

static char *jitlog_buf_more(SBuf* sb, MSize sz)
{
  if (LJ_UNLIKELY(sz > sbufleft(sb))) {
    return buf_more2(sb, sz);
  }
  return sbufP(sb);
}

static SBuf *jitlog_buf_putmem(SBuf *sb, const void *q, MSize len)
{
  char *p = jitlog_buf_more(sb, len);
  p = lj_buf_wmem(p, q, len);
  setsbufP(sb, p);
  return sb;
}

LJ_STATIC_ASSERT(offsetof(JITLogState, eventbuf) == 0);
LJ_STATIC_ASSERT(offsetof(SBuf, p) == 0);

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

static char* strlist_concat(const char *const *list, int limit, MSize *retsize)
{
  const char *const *pos = list;
  int count = 0;
  char *buff;
  MSize total = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    total += (MSize)strlen(s)+1;
  }
  buff = (char *)malloc(total);
  *retsize = total;

  pos = list;
  count = 0;
  total = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    size_t size = strlen(s)+1;
    memcpy(buff + total, s, size);
    total += size;
  }
  return buff;
}

static void write_enumdef(JITLogState *context, const char *name, const char *const *names, uint32_t namecount, int isbitflags)
{
  MSize size = 0;
  char *namesblob = strlist_concat(names, namecount, &size);
  log_enumdef(context->g, isbitflags, name, namecount, namesblob, size);
  free(namesblob);
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

static void memorize_func(JITLogState *context, GCfunc *fn)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  setfuncV(L, &key, fn);

  if (!memorize_gcref(L, context->funcs, &key, &context->funccount)) {
    return;
  }

  if (isluafunc(fn)) {
    memorize_proto(context, funcproto(fn));

    int i;
    TValue *upvalues = lj_mem_newvec(L, fn->l.nupvalues, TValue);
    for(i = 0; i != fn->l.nupvalues; i++) {
      upvalues[i] = *uvval(&gcref(fn->l.uvptr[i])->uv);
    }
    log_gcfunc(context->g, fn, funcproto(fn), fn->l.ffid, upvalues, fn->l.nupvalues);
  } else {
    log_gcfunc(context->g, fn, fn->c.f, fn->l.ffid, fn->c.upvalue, fn->c.nupvalues);
  }
}

#if LJ_HASJIT

static GCproto* getcurlualoc(JITLogState *context, uint32_t *pc)
{
  jit_State *J = G2J(context->g);
  GCproto *pt = NULL;

  if (J->pt) {
    pt = J->pt;
    *pc = proto_bcpos(pt, J->pc);
  } else if (context->lastlua) {
    pt = funcproto(context->lastlua);
    lua_assert(context->lastpc < pt->sizebc);
    *pc = context->lastpc;
  }

  return pt;
}

static void jitlog_tracestart(JITLogState *context, GCtrace *T)
{ 
  jit_State *J = G2J(context->g);
  context->startfunc = J->fn;
  context->lastfunc = context->lastlua = J->fn;
  context->lastpc = proto_bcpos(J->pt, J->pc);
}

static int isstitched(JITLogState *context, GCtrace *T)
{
  jit_State *J = G2J(context->g);
  if (J->parent == 0) {
    BCOp op = bc_op(T->startins);
    /* The parent trace rewrites the stack so this trace is started after the untraceable call */
    return op == BC_CALLM || op == BC_CALL || op == BC_ITERC;
  }
  return 0;
}

static void jitlog_writetrace(JITLogState *context, GCtrace *T, int abort)
{
  jit_State *J = G2J(context->g);
  GCproto *startpt = &gcref(T->startpt)->pt;
  BCPos startpc = proto_bcpos(startpt, mref(T->startpc, const BCIns));
  BCPos stoppc;
  GCproto *stoppt = getcurlualoc(context, &stoppc);
  memorize_proto(context, startpt);
  memorize_proto(context, stoppt);
  memorize_func(context, context->lastfunc);

  int abortreason = (abort && tvisnumber(J->L->top-1)) ? numberVint(J->L->top-1) : -1;
  MSize mcodesize;
  if (context->user.logfilter & LOGFILTER_TRACE_MCODE) {
    mcodesize = 0;
  } else {
    mcodesize = T->szmcode;
  }
  int irsize;
  if (context->user.logfilter & LOGFILTER_TRACE_IR) {
    irsize = 0;
  } else {
    irsize = (T->nins - T->nk) + 1;
  }

  log_trace(context->g, T, abort, isstitched(context, T), J->parent, stoppt, stoppc, context->lastfunc, (uint16_t)abortreason, startpc, mcodesize, T->ir + T->nk, irsize);
}

static void jitlog_tracestop(JITLogState *context, GCtrace *T)
{
  if (context->user.logfilter & LOGFILTER_TRACE_COMPLETED) {
    return;
  }
  jitlog_writetrace(context, T, 0);
}

static void jitlog_traceabort(JITLogState *context, GCtrace *T)
{
  if (context->user.logfilter & LOGFILTER_TRACE_ABORTS) {
    return;
  }
  jitlog_writetrace(context, T, 1);
}

static void jitlog_tracebc(JITLogState *context)
{
  jit_State *J = G2J(context->g);
  if (context->lastfunc != J->fn) {
    context->lastfunc = J->fn;
  }

  if (J->pt) {
    lua_assert(isluafunc(J->fn));
    context->lastlua = J->fn;
    context->lastpc = proto_bcpos(J->pt, J->pc);
  }
}

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

static void jitlog_protobl(JITLogState *context, VMEventData_ProtoBL *data)
{
  memorize_proto(context, data->pt);
  log_protobl(context->g, data->pt, data->pc);
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

static void jitlog_protoloaded(JITLogState *context, GCproto *pt)
{
  if (context->user.logfilter & LOGFILTER_PROTO_LOADED) {
    return;
  }
  memorize_proto(context, pt);
  log_protoloaded(context->g, pt);
}

static void free_context(JITLogState *context);

static void gcalloc_cb(JITLogState *context, GCobj *o, uint32_t info, size_t size)
{
  int free = (info & 0x80) != 0;
  int tid = info & 0x7f;
  uint32_t type = 0;
  uint32_t extra = 0;

  if (tid == (1 + ~LJ_TUDATA)) {
    GCtab *t = (GCtab *)o;
    uint32_t ahsize = t->asize << 8;
    ahsize |= t->hmask > 0 ? lj_fls(t->hmask+1) : 0;
    log_gctab_resize(context->g, (info >> 8) & 0xff, info >> 16, o, ahsize);
    return;
  }

  if (!free && 0) {
    if (tid == ~LJ_TSTR) {
      memorize_string(context, (GCstr *)o);
      return;
    } else if(tid == ~LJ_TFUNC) {
      memorize_func(context, (GCfunc *)o);
      return;
    }
  }
  
  switch (tid) {
    case ~LJ_TSTR:
      type = 0;
      break;
    case ~LJ_TUPVAL:
      type = 1;
      break;
    case ~LJ_TTHREAD:
      type = 2;
      break;
    case ~LJ_TPROTO:
      type = 3;
      break;
    case ~LJ_TFUNC:
      type = isluafunc(&o->fn) ? 4 : 5;
      break;
    case ~LJ_TTRACE:
      type = 6;
      break;
    case ~LJ_TCDATA:
      type = 7;
      extra = ((GCcdata *)o)->ctypeid;
      break;
    case ~LJ_TTAB:
      type = 8;
      break;
    case ~LJ_TUDATA:
      type = 9;
      break;
    default:
      lua_assert(0);
      break;
  }

  if (size < (1 << 19) && extra == 0) {
    log_gcobj(context->g, free, type, (uint32_t)size, o);
  } else {
    log_gcobj_large(context->g, free, type, extra, o, (uint32_t)size);
  }
}

static void jitlog_loadstage2(lua_State *L, JITLogState *context);

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  JITLogState *context = contextptr;

  if (context->safestarted && event != VMEVENT_DETACH && event != VMEVENT_STATE_CLOSING) {
    jitlog_loadstage2(L, context);
    context->safestarted = 0;
  }

  switch (event) {
#if LJ_HASJIT
    case VMEVENT_TRACE_START:
      jitlog_tracestart(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_RECORD:
      jitlog_tracebc(context);
      break;
    case VMEVENT_TRACE_STOP:
      jitlog_tracestop(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_ABORT:
      jitlog_traceabort(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_EXIT:
      jitlog_exit(context, (VMEventData_TExit*)eventdata);
      break;
    case VMEVENT_PROTO_BLACKLISTED:
      jitlog_protobl(context, (VMEventData_ProtoBL*)eventdata);
      break;
    case VMEVENT_TRACE_FLUSH:
      jitlog_traceflush(context, (FlushReason)(uintptr_t)eventdata);
      break;
#endif
    case VMEVENT_BC:
      jitlog_protoloaded(context, (GCproto*)eventdata);
      break;
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

static const char *const fastfunc_names[] = {
  "Lua",
  "C",
  #define FFDEF(name)   #name,
  #include "lj_ffdef.h"
  #undef FFDEF
};

static const char *const terror[] = {
  #define TREDEF(name, msg)	#name,
  #include "lj_traceerr.h"
  #undef TREDEF
};

static const char *const trace_errors[] = {
  #define TREDEF(name, msg)	msg,
  #include "lj_traceerr.h"
  #undef TREDEF
};

static const char *const ir_names[] = {
  #define IRNAME(name, m, m1, m2)	#name,
  IRDEF(IRNAME)
  #undef IRNAME
};

static const char *const irt_names[] = {
  #define IRTNAME(name, size)	#name,
  IRTDEF(IRTNAME)
  #undef IRTNAME
};

static const char *const ircall_names[] = {
  #define IRCALLNAME(cond, name, nargs, kind, type, flags)	#name,
  IRCALLDEF(IRCALLNAME)
  #undef IRCALLNAME
};

static const char * irfield_names[] = {
  #define FLNAME(name, ofs)	#name,
  IRFLDEF(FLNAME)
  #undef FLNAME
};

#define write_enum(context, name, strarray) write_enumdef(context, name, strarray, (sizeof(strarray)/sizeof(strarray[0])), 0)

static void write_header(JITLogState *context)
{
  lua_State *L = mainthread(context->g);
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  MSize msgnamessz = 0;
  char *msgnamelist = strlist_concat(msgnames, MSGTYPE_MAX, &msgnamessz);
  log_header(context->g, 1, 0, sizeof(MSG_header), msgsizes, MSGTYPE_MAX, msgnamelist, msgnamessz, cpumodel, model_length, LJ_OS_NAME, (uintptr_t)G2GG(context->g));
  free(msgnamelist);

  write_enum(context, "gcstate", gcstates);
  write_enum(context, "flushreason", flushreason);
  write_enum(context, "bc", bc_names);
  write_enum(context, "fastfuncs", fastfunc_names);
  write_enum(context, "terror", terror);
  write_enum(context, "trace_errors", trace_errors);
  write_enum(context, "ir", ir_names);
  write_enum(context, "irtypes", irt_names);
  write_enum(context, "ircalls", ircall_names);
  write_enum(context, "irfields", irfield_names);
}

static int jitlog_isrunning(lua_State *L)
{
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  return cb == jitlog_callback;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

static JITLogState *jitlog_start_safe(lua_State *L)
{
  JITLogState *context;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(JITLogState));
  memset(context, 0, sizeof(JITLogState));
  context->g = G(L);
  context->safestarted = 1;

  SBuf *sb = &context->eventbuf;
  lj_buf_init(L, sb);
  jitlog_buf_more(sb, 1024*1024*500);
  
  luaJIT_vmevent_sethook(L, jitlog_callback, context);
  /* FIXME this currently has to be after luaJIT_vmevent_sethook because the log writer 
     functions access G->vmevent_data.
  */
  write_header(context);

  G(L)->objalloc_cb = (lua_ObjAlloc_cb)&gcalloc_cb;
  G(L)->objallocd = context;
  return context;
}

static void jitlog_loadstage2(lua_State *L, JITLogState *context)
{
  lua_assert(!context->strings && !context->protos && !context->funcs);
  context->strings = create_pinnedtab(L);
  context->protos = create_pinnedtab(L);
  context->funcs = create_pinnedtab(L);
  lj_lib_prereg(L, "jitlog", luaopen_jitlog, tabref(L->env));
  context->safestarted = 0;
}

LUA_API JITLogUserContext* jitlog_start(lua_State *L)
{
  JITLogState *context;
  lua_assert(!jitlog_isrunning(L));
  context = jitlog_start_safe(L);
  if (0) {
    jitlog_loadstage2(L, context);
  }
  return &context->user;
}

static void free_context(JITLogState *context)
{
  global_State *g = context->g;
  SBuf *sb = &context->eventbuf;
  const char *path = getenv("JITLOG_PATH");
  if (path != NULL) {
    jitlog_save(ctx2usr(context), path);
  }
  if (g->objalloc_cb == (lua_ObjAlloc_cb)&gcalloc_cb) {
    g->objalloc_cb = NULL;
    g->objallocd = NULL;
  }

  g->allocf(g->allocd, sbufB(sb), sbufsz(sb), 0);
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

  if (G(L)->objalloc_cb == (lua_ObjAlloc_cb)&gcalloc_cb) {
    G(L)->objalloc_cb = NULL;
    G(L)->objallocd = NULL;
  }

  free_pinnedtab(L, context->strings);
  free_pinnedtab(L, context->protos);
  free_pinnedtab(L, context->funcs);
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
  context->funccount = 0;
  lj_tab_clear(context->strings);
  lj_tab_clear(context->protos);
  lj_tab_clear(context->funcs);
  lj_buf_reset(&context->eventbuf);
  memset(context->lasthotcounts, 0, HOTCOUNT_SIZE * sizeof(short));
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

LUA_API void jitlog_savehotcounts(JITLogUserContext *usrcontext)
{
  JITLogState *context = usr2ctx(usrcontext);
  GG_State *gg = G2GG(context->g);
#if LJ_HASJIT
  if (memcmp(context->lasthotcounts, gg->hotcount, sizeof(gg->hotcount)) == 0) {
    return;
  }
  memcpy(context->lasthotcounts, gg->hotcount, sizeof(gg->hotcount));
  log_hotcounts(context->g, G2GG(context->g)->hotcount, HOTCOUNT_SIZE);
#endif
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

#if LJ_HASJIT

static int jlib_cmp_hotcounts(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  GG_State *gg = G2GG(context->g);
  int i, changed = 0;
  for (i = 0; i != HOTCOUNT_SIZE; i++) {
    if (context->lasthotcounts[i] != gg->hotcount[i]) changed++;
  }
  lua_pushnumber(L, changed);
  return 1;
}

static int jlib_snap_hotcounts(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  GG_State *gg = G2GG(context->g);
  memcpy(context->lasthotcounts, gg->hotcount, sizeof(gg->hotcount));
  return 0;
}

/*
** Write the Lua function hot counters to the JITLog if they've changed since the last call to
** snap_hotcounts or write_hotcounts.
*/
static int jlib_write_hotcounts(lua_State *L)
{
  JITLogState *context = jlib_getstate(L);
  jitlog_savehotcounts(ctx2usr(context));
  return 0;
}

#endif

static const luaL_Reg jitlog_lib[] = {
  {"start", jlib_start},
  {"shutdown", jlib_shutdown},
  {"reset", jlib_reset},
  {"save", jlib_save},
  {"savetostring", jlib_savetostring},
  {"getsize", jlib_getsize},
  {"addmarker", jlib_addmarker},
#if LJ_HASJIT
  {"snap_hotcounts", jlib_snap_hotcounts},
  {"cmp_hotcounts", jlib_cmp_hotcounts},
  {"write_hotcounts", jlib_write_hotcounts},
#endif
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
