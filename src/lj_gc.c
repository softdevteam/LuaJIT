/*
** Garbage collector.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_vm.h"
#include "lj_vmevent.h"
#include "lj_dispatch.h"
#include "lj_alloc.h"
#include "lj_vmperf.h"
#include "gcdebug.h"
#include "stdio.h"

#include <mmintrin.h>
#include <xmmintrin.h>
#include "lj_vmperf.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

#if DEBUG
void TraceGC(global_State *g, int newstate);
#else
#define TraceGC(g, newstate)
#endif

static void gc_setstate(global_State *g, int newstate)
{
  TraceGC(g, newstate);
  lj_vmevent_callback(mainthread(g), VMEVENT_GC_STATECHANGE, (void*)(uintptr_t)newstate);
  g->gc.state = newstate;
}

#if DEBUG
#define GCDEBUG(fmt, ...)  printf(fmt, __VA_ARGS__)
extern void VERIFYGC(global_State *g);
extern void VERIFYGC_SKIPOBJ(global_State *g, void *o);
#else
#define GCDEBUG(fmt, ...)
#define VERIFYGC(g)
#define VERIFYGC_SKIPOBJ(g)
#endif


/* Macros to set GCobj colors and flags. */
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tvisgcv(tv) && (gc_ishugeblock(gcV(tv)) || arenaobj_iswhite(gcV(tv)))) gc_mark(g, gcV(tv), ~itype(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (gc_ishugeblock(o) || arenaobj_iswhite(obj2gco(o))) gc_mark(g, obj2gco(o), obj2gco(o)->gch.gct); }

#define gc_markgct(g, o, gct) \
  { if (gc_ishugeblock(o) || arenaobj_iswhite(obj2gco(o))) gc_mark(g, obj2gco(o), gct); }

#define gc_markthread(g, o)  gc_markgct(g, o, ~LJ_TTHREAD)

#define gc_mark_tab(g, o) \
  { if (arenaobj_iswhite(obj2gco(o))) gc_mark(g, obj2gco(o), ~LJ_TTAB); }

/* Mark a string object. */
#define gc_mark_str(g, s)	\
  { if (gc_ishugeblock(s) || arenaobj_iswhite(obj2gco(s))) gc_mark(g, obj2gco(s), ~LJ_TSTR); }

void gc_mark_gcvec(global_State *g, void *v, MSize size);

/* Mark a white GCobj. */
void gc_mark(global_State *g, GCobj *o, int gct)
{
  lua_assert(!isdead(g, o));
  lua_assert(gc_ishugeblock(o) || iswhite(g, o));
  lua_assert(gct == o->gch.gct);

  /* Huge objects are always unconditionally sent to us to make white checks simple */
  if (LJ_UNLIKELY(gc_ishugeblock(o))) {
    hugeblock_mark(g, o);

    /* No further processing */
    if (gct != ~LJ_TUDATA) {
      return;
    }
  }

  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    arenaobj_markcdstr(o);  /* Userdata are never gray. */
    if (mt) gc_mark_tab(g, mt);
    gc_mark_tab(g, tabref(gco2ud(o)->env));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    arenaobj_toblack(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gc_markobj(g, o);  /* Closed upvalues are never gray. */
  } else {
    arenaobj_markgct(g, o, gct);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  gc_setstate(g, GCSpropagate);
  if (g->gc.isminor) {
    arenaobj_towhite(obj2gco(&G2GG(g)->L));
    arenaobj_towhite(obj2gco(mainthread(g)));
    setgcrefnull(g->gc.grayagain);
  } else {
    g->gc.ssbsize = 0;
    setgcrefnull(g->gc.grayagain);
  }

  arena_markcell(ptr2arena(G2GG(g)), MinCellId);
  gc_markthread(g, &G2GG(g)->L);
  gc_mark_str(g, &g->strempty);
  gc_markthread(g, mainthread(g));
  gc_mark_tab(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    if (lj_gc_arenaflags(g, i) & ArenaFlag_FixedList) {
      arena_markfixed(g, arena);
    } else {
      lua_assert(!mref(arena_extrainfo(arena)->fixedcells, GCCellID1));
    }
  }
  lua_assert(arenaobj_isblack(&g->strempty));
//  print_deadobjs(g);
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    lua_assert(!arenaobj_isdead(uv));
    if (arenaobj_isblack(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

static void gc_sweep_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    if (arenaobj_iswhite(obj2gco(uv))) {
      setgcrefr(uvnext(uv)->prev, uv->prev);
      setgcrefr(uvprev(uv)->next, uv->next);
    }
  }
}

static int check_arenafinalizers(global_State *g, GCArena *arena)
{
  lua_State *L = mainthread(g);
  CellIdChunk *chunk = arena_finalizers(arena);
  MRef *prev = &arena_extrainfo(arena)->finalizers;
  int hasudata = lj_gc_arenaflags(g, arena->extra.id) & ArenaFlag_TravObjs;
  int numfinal = 0;

  for (; chunk != NULL;) {
    MSize count = idlist_count(chunk);
    lua_assert(count <= (sizeof(chunk->cells)/2));

    for (size_t i = 0; i < count; i++) {
      GCCellID cell = chunk->cells[i];
      lua_assert(!cell || arena_cellisallocated(arena, cell));
      if (cell == 0) {
        chunk->cells[i] = chunk->cells[--count];
        i--;
        continue;
      } else if (arena_cellismarked(arena, cell)) {
        continue;
      }

      GCobj *o = arena_cellobj(arena, cell);
      if (hasudata && o->gch.gct == ~LJ_TUDATA) {
        if (lj_meta_fastg(g, tabref(o->gch.metatable), MM_gc)) {
          /* We have to make sure the env\meta tables of the userdata are kept
          ** alive past the sweep phase, in case there needed in the finalizer
          ** phase that happens after the sweep.
          */
          gc_mark(g, o, ~LJ_TUDATA);
          idlist_markcell(chunk, i);
          numfinal++;
        } else {
          chunk->cells[i] = chunk->cells[--count];
          i--;
          continue;
        }
      } else {
        lua_assert(o->gch.gct == ~LJ_TCDATA);
        arena_markcell(arena, cell);
        idlist_markcell(chunk, i);
        numfinal++;
      }
      /* Mark that this value can be swept from weak tables */
      o->gch.marked |= LJ_GCFLAG_FINALIZED;
    }

    if (count == 0) {
      CellIdChunk *next = idlist_next(chunk);
      setmref(*prev, next);
      idlist_freechunk(g, chunk);
      chunk = next;
      continue;
    } else {
      idlist_setcount(chunk, count);
      prev = &chunk->next;
      lua_assert(idlist_count(chunk) <= idlist_maxcells);
      chunk = idlist_next(chunk);
    }
  }

  return numfinal;
}

/* Flag white cells to be finalized */
size_t lj_gc_scan_finalizers(global_State *g, int all)
{
  lua_State *L = mainthread(g);
  size_t count = 0;
  TIMER_START(gc_separateudata);
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);

    if (arena_finalizers(arena)) {
      MSize objnum = check_arenafinalizers(g, arena);
      count += objnum;
      if (objnum) {
        GCDEBUG("arena(%d) has %d finalizations\n", i, objnum);
        lj_gc_setarenaflag(G(L), i, ArenaFlag_Finalizers);
      }
    }
  }
  TIMER_END(gc_separateudata);
  count += hugeblock_checkfinalizers(g);
  if (count) {
    g->gc.stateflags |= GCSFLAG_HASFINALIZERS;
  }
  return count;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_mark_tab(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GCFLAG_WEAKKEY;
      else if (c == 'v') weak |= LJ_GCFLAG_WEAKVAL;
    }
    if (weak > 0) {  /* Weak tables are cleared in the atomic phase. */
      t->marked = (uint8_t)((t->marked & ~LJ_GCFLAG_WEAK) | weak);
      GCRef *weaklist = mref(g->gc.weak, GCRef);
      if (LJ_UNLIKELY(g->gc.weaknum == g->gc.weakcapacity)) {
        lj_mem_growvec(gco2th(gcref(g->cur_L)), weaklist, g->gc.weakcapacity,
                       LJ_MAX_MEM32, GCRef);
        setmref(g->gc.weak, weaklist);
      }
      setgcref(weaklist[g->gc.weaknum++], obj2gco(t));
    }
  }
  if (t->asize && !hascolo_array(t))
    gc_mark_gcvec(g, arrayslot(t, 0), t->asize * sizeof(TValue));
  if (t->hmask && !hascolo_hash(t))
    gc_mark_gcvec(g, mref(t->node, void), t->hmask * sizeof(Node));
  if (weak == LJ_GCFLAG_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;

  if (!(weak & LJ_GCFLAG_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++) {
      TValue *tv = arrayslot(t, i);
      //gc_marktv(g, arrayslot(t, i));

      if (tvisgcv(tv) && (gc_ishugeblock(gcV(tv)) || arenaobj_iswhite(gcV(tv)))) {
        if (!gc_ishugeblock(gcV(tv)) && (itype(tv) == LJ_TSTR || itype(tv) == LJ_TCDATA ||
                                         itype(tv) == LJ_TFUNC || itype(tv) == LJ_TTAB)) {
          arenaobj_markgct(g, gcV(tv), ~itype(tv));
        } else {
          gc_mark(g, gcV(tv), ~itype(tv));
        }
      }
    }
  }
  if (weak == 0) {
    cleargray(t);
  }

  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
        lua_assert(!tvisnil(&n->key));
        if (!(weak & LJ_GCFLAG_WEAKKEY)) gc_marktv(g, &n->key);
        if (!(weak & LJ_GCFLAG_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  cleargray((GCobj *)fn);
  gc_mark_tab(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lua_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);
    gc_markgct(g, funcproto(fn), ~LJ_TPROTO);
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lua_assert(traceno != G2J(g)->cur.traceno);
  gc_markgct(g, o, ~LJ_TTRACE);
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) {
    return;
  }
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markgct(g, ir_kgc(ir), ~irt_toitype(ir->t));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markgct(g, gcref(T->startpt), ~LJ_TPROTO);
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(g, proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

GCSize gc_traverse(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0) {
      //black2gray(o);  /* Keep weak tables gray. */
    }
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    gc_marktv(g, &(gco2uv(o)->tv));
    cleargray(o);
    return sizeof(GCupval);
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lua_assert(0);
    return 0;
#endif
  }
}

static void pqueue_init(lua_State *L, PQueue* q)
{
  q->size = 16;
  q->count = 0;
  q->array = lj_mem_newvec(L, q->size, GCArena*);
}

#define child_left(idx) (idx * 2 + 1)
#define child_right(idx) (idx * 2 + 2)
#define parentidx(idx) ((idx - 1) / 2)

static void pqueue_pushup(PQueue* q, MSize idx)
{
  GCArena *arena = q->array[idx];
  MSize greylen = arena_greysize(arena);
  MSize i = idx;

  while (i != 0) {
    GCArena *parent = q->array[parentidx(i)];
    MSize parentlen = arena_greysize(parent);
    /* If parents queue is smaller swap it down */
    if (parentlen < greylen) {
      q->array[parentidx(i)] = arena;
      q->array[i] = parent;
    }
    i = parentidx(i);
    arena = parent;
    greylen = parentlen;
  }
}

static void pqueue_insert(lua_State *L, PQueue* q, GCArena *arena)
{
  /* TODO: cache queue size in the lower bits of the pointer with some refresh mechanism */
  if ((q->count+1) >= q->size) {
    lj_mem_growvec(L, q->array, q->size, LJ_MAX_MEM32, GCArena*);
  }
  q->array[q->count] = arena;
  pqueue_pushup(q, q->count);
  q->count++;
}

static void pqueue_pushdown(PQueue* q, MSize idx)
{
  MSize i = idx;

  while (1) {
    GCArena *arena = q->array[i];
    if (child_right(i) >= q->count) {
      break;
    }

    MSize lsize = arena_greysize(q->array[child_left(i)]);
    MSize rsize = arena_greysize(q->array[child_right(i)]);
    MSize cidx = lsize > rsize ? child_left(i) : child_right(i);
    GCArena *child = lsize > rsize ? q->array[child_left(i)] : q->array[child_right(i)];
    MSize csize = lsize > rsize ? lsize : rsize;

    if (csize > arena_greysize(arena)) {
      q->array[i] = q->array[cidx];
      q->array[cidx] = arena;
      i = cidx;
    } else {
      return;
    }
  }
}

/* Rotate tree now that max was emptied */
static void pqueue_rotatemax(PQueue* q)
{
  GCArena *maxarena = q->array[0];

  if (q->count <= 1) {
    return;
  }

  q->array[0] = q->array[q->count-1];
  q->array[q->count-1] = maxarena;
  pqueue_pushdown(q, 0);
}

static GCArena *pqueue_peekmax(PQueue* q)
{
  if (arena_greysize(q->array[0]) == 0) {
    if (q->count == 1) {
      return NULL;
    } else {
      pqueue_rotatemax(q);
      /* FIXME: dirty hack for queues lengths changing from zero after being inserted */
      if (arena_greysize(q->array[0]) == 0) {
        for (MSize i = 0; i < q->count; i++) {
          if (arena_greysize(q->array[i]) != 0) {
            return q->array[i];
          }
        }
        return NULL;
      }
      return q->array[0];
    }
  }

  return q->array[0];
}

static GCArena* largestgray(global_State *g)
{
  MSize maxqueue = 0;
  int arenai = -1;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    if (lj_gc_arenaflags(g, i) & ArenaFlag_Empty) continue;
    if (arena_greysize(arena)) {
      //printf("arena(%d) greyqueue = %d\n", i, arena_greysize(arena));
    }
    if (arena_greysize(arena) > maxqueue) {
      maxqueue = arena_greysize(arena);
      arenai = i;
    }
  }

  return arenai != -1 ? lj_gc_arenaref(g, arenai) : NULL;
}

static GCSize propagate_arenagrays(global_State *g, GCArena *arena, int limit, MSize *travcount)
{
  ArenaFreeList *freelist = arena_freelist(arena);
  GCSize total = 0;
  MSize count = 0;

  if (mref(arena->greytop, GCCellID1) == NULL) {
    return 0;
  }

  for (; *mref(arena->greytop, GCCellID1) != 0;) {
    GCCellID1 *top = mref(arena->greytop, GCCellID1);
    GCCellID cellid = *top;
    MSize gct = arena_cell(arena, cellid)->gct;
    lua_assert(cellid >= MinCellId && cellid < MaxCellId);
    lua_assert(arena_cellstate(arena, cellid) == CellState_Black);

    setmref(arena->greytop, top+1);
    _mm_prefetch((char *)(arena->cells + *(top)), _MM_HINT_T0);
    total += gc_traverse(g, arena_cellobj(arena, cellid));

    count++;
    if (limit != -1 && count >(MSize)limit) {
      break;
    }
  }
  if (travcount)*travcount = count;
  /* Check we didn't stop from some corrupted cell id that looked like the stack top sentinel */
  lua_assert(arena_greysize(arena) == 0 || *mref(arena->greytop, GCCellID1) != 0);

  return total;
}

/* Propagate all gray objects. */
static GCSize gc_propagate_gray(global_State *g)
{
  lua_State *L = mainthread(g);
  GCSize total = 0;
  GCArena *maxarena = NULL;
  PQueue *greyqueu = &g->gc.greypq;

  if (greyqueu->size == 0) {
    pqueue_init(mainthread(g), greyqueu);
  } else {
    greyqueu->count = 0;
  }

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags flags = lj_gc_arenaflags(g, i);
    /* Skip empty and non traversable arenas */
    if ((flags & (ArenaFlag_Empty|ArenaFlag_TravObjs)) != ArenaFlag_TravObjs) {
      lua_assert(arena_greysize(arena) == 0);
      lua_assert(((flags & ArenaFlag_Empty) && arena_totalobjmem(arena) == 0) ||
                 (!(flags & ArenaFlag_Empty) && arena_totalobjmem(arena) != 0));
      continue;
    }
    pqueue_insert(L, greyqueu, arena);
  }

  while (1) {
    maxarena = pqueue_peekmax(greyqueu);
    //maxarena = largestgray(g);
    /* Stop once all arena queues are empty */
    if (maxarena == NULL) {
      break;
    }

    MSize count = 0;
    GCSize omem = propagate_arenagrays(g, maxarena, -1, &count);
    /* Swap the arena to the back of the queue now its grey queue is empty */
    pqueue_rotatemax(greyqueu);
    total += omem;

    // printf("propagated %d objects in arena(%d), with a size of %d\n", count, arena_extrainfo(maxarena)->id, omem);
  }
  assert_greyempty(g);

  return total;
}

/* -- Sweep phase --------------------------------------------------------- */

static int gc_sweepstring(global_State *g)
{
  GCRef str = g->strhash[g->gc.sweepstr]; /* Sweep one chain. */
  GCstr *prev = NULL;

  while (gcref(str)) {
    GCstr *s = strref(str);
    str = s->nextgc;
    if (iswhite(g, s)) {
      g->strnum--;
      *(prev ? &prev->nextgc : &g->strhash[g->gc.sweepstr]) = s->nextgc;
    } else {
      lua_assert(!gc_ishugeblock(s) ? !arenaobj_isdead(s) : !hugeblock_isdead(g, obj2gco(s)));
      prev = s;
    }
  }

  return ++g->gc.sweepstr <= g->strmask;
}

static void atomic_enqueue_finalizers(lua_State *L)
{
#if LJ_HASFFI
  global_State *g = G(L);
  CTState *cts = mref(g->ctype_state, CTState);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *n = noderef(t->node);
    MSize i = t->hmask + 1;
    for (; i; --i, ++n) {
      if (!tvisnil(&n->val) && tvisgcv(&n->key)) {
        GCobj *o = gcV(&n->key);
        if (iswhite(g, (void*)o)) {
          lj_gc_setfinalizable(L, o, NULL);
        }
      }
    }
  }
#endif
}

void lj_gc_setfinalizable(lua_State *L, GCobj *o, GCtab *mt)
{
  lua_assert(o->gch.gct == ~LJ_TCDATA || o->gch.gct == ~LJ_TUDATA);
  if (!gc_ishugeblock(o)) {
    GCArena *arena = ptr2arena(o);
    arena_addfinalizer(L, arena, o);
  } else {
    hugeblock_setfinalizable(G(L), o);
  }
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
  cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  GCSize oldt = g->gc.threshold;
  int errcode;
  TValue *top;
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  top = L->top;
  copyTV(L, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(L, top, o, ~o->gch.gct);
  L->top = top+1;
  errcode = lj_vm_pcall(L, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

/* Finalize a userdata or cdata object */
static void gc_finalize(lua_State *L, GCobj *o)
{
  global_State *g = G(L);
  cTValue *mo;
  lua_assert(tvref(g->jit_base) == NULL);  /* Must not be called on trace. */

#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */

    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      g->gc.nocdatafin = 0;
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

static int gc_finalize_step(lua_State *L)
{
  global_State *g = G(L);
  CellIdChunk *chunk;
  MRef *prev = (MRef *)g->gc.sweep;

  for (; g->gc.curarena < g->gc.arenastop; g->gc.curarena++) {
    GCArena *arena = lj_gc_curarena(g);
    if (!(lj_gc_arenaflags(g, g->gc.curarena) & ArenaFlag_Finalizers)) {
      prev = NULL;
      g->gc.sweepi = 0;
      continue;
    }
    /* Continuing on from previous */
    if (prev) {
      chunk = mref(*(MRef *)prev, CellIdChunk);
    } else {
      prev = &arena_extrainfo(arena)->finalizers;
      chunk = arena_finalizers(arena);
    }
    lua_assert(!chunk || chunk->count != 0);

    while (chunk) {
      MSize i;
      for (i = g->gc.sweepi; i < idlist_count(chunk); i++) {
        if (idlist_getmark(chunk, i)) {
          GCCellID id = chunk->cells[i];
          GCDEBUG("Finalizing cell %d\n", id);
          if (idlist_remove(chunk, i, 1)) {
            *((MRef *)prev) = chunk->next;
          }
          /* Note idlist_remove will have swapped the cellid at the end of the
          ** list in to the current index.
          */
          g->gc.sweepi = i;
          g->gc.sweep = prev;
          gc_finalize(L, (GCobj *)arena_cell(arena, id));
          return 1;
        }
      }
      prev = &chunk->next;
      chunk = idlist_next(chunk);
    }
    prev = NULL;
    lj_gc_cleararenaflags(g, g->gc.curarena, ArenaFlag_Finalizers);
  }

  return 0;
}

/* Finalize all objects in an arenas. */
void lj_gc_finalize_all(lua_State *L)
{
  global_State *g = G(L);
  CellIdChunk *chunk;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    chunk = arena_finalizers(arena);

    while (chunk) {
      CellIdChunk *next;
      for (MSize j = 0; j < chunk->count; j++) {
        gc_finalize(L, arena_cellobj(arena, chunk->cells[j]));
      }

      next = idlist_next(chunk);
      idlist_freechunk(g, chunk);
      chunk = next;
    }
    setmref(arena_extrainfo(arena)->finalizers, NULL);
  }
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *node = noderef(t->node);
    ptrdiff_t i;
    setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
    for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
      if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
        GCobj *o = gcV(&node[i].key);
        TValue tmp;
        makewhite(g, o);
        o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
        copyTV(L, &tmp, &node[i].val);
        setnilV(&node[i].val);
        gc_call_finalizer(g, L, &tmp, o);
      }
  }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i;
  setgcref(g->gc.root, obj2gco(mainthread(g)));
  g->strnum = 0;
  g->gc.total -= arena_totalobjmem(lj_gc_arenaref(g, 0));
  /* Skip GG arena */
  for (i = 1; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    g->gc.total -= arena_totalobjmem(arena);
    arena_destroy(g, arena);
  }
  hugeblock_freeall(g);

  lj_mem_freevec(g, g->gc.arenas, g->gc.arenassz, GCArena*);
  lj_mem_freevec(g, g->gc.freelists, g->gc.arenassz, ArenaFreeList);
  lj_mem_freevec(g, mref(g->gc.weak, GCRef), g->gc.weakcapacity, GCRef);
}

int lj_gc_getarenaid(global_State *g, void* arena)
{
  for (MSize i = 0; i < g->gc.arenastop; i++)
    if (lj_gc_arenaref(g, i) == arena) return i;

  return -1;
}

static int register_arena(lua_State *L, GCArena *arena, uint32_t flags)
{
  global_State *g = G(L);
  ArenaFreeList *freelist;

  if (LJ_UNLIKELY((g->gc.arenastop+1) >= g->gc.arenassz)) {
    MSize size = g->gc.arenassz;
    lj_mem_growvec(L, g->gc.arenas, g->gc.arenassz, LJ_MAX_MEM32, GCArena*);
    lj_mem_growvec(L, g->gc.freelists, size, LJ_MAX_MEM32, ArenaFreeList);
    freelist = g->gc.freelists;
    for (MSize i = 0; i < g->gc.arenastop; i++) {
      freelist[i].owner = lj_gc_arenaref(g, i);
      setmref(lj_gc_arenaref(g, i)->freelist, freelist+i);
    }
  }

  /* Don't allow the GC to sweep freshly created arenas */
  if (g->gc.state == GCSsweep || g->gc.state == GCSsweepstring) {
    flags |= ArenaFlag_Swept;
  }

  g->gc.arenas[g->gc.arenastop] = (GCArena *)(((intptr_t)arena) | flags);

  freelist = G(L)->gc.freelists + g->gc.arenastop;
  memset(freelist, 0, sizeof(ArenaFreeList));
  freelist->owner = arena;
  setmref(arena->freelist, freelist);
  arena->extra.id = g->gc.arenastop;
  arena->extra.flags = flags;
  return g->gc.arenastop++;
}

GCArena* lj_gc_newarena(lua_State *L, uint32_t flags)
{
  GCArena *arena = arena_create(L, flags);
  MSize id = register_arena(L, arena, flags);
  global_State *g = G(L);
  /* If the GC is propagating make sure to add the arena to the grey queue since
  ** objects created in it could ref objects in other arenas
  */
  if ((flags & ArenaFlag_TravObjs) && (g->gc.state & 1)) {
    pqueue_insert(L, &g->gc.greypq, arena);
  }
#ifdef LJ_ENABLESTATS
  if (g->vmevent_data) {
    /*  log_arenacreated((UserBuf *)g->vmevent_data, id, arena, g->gc.total, flags); */
  }
#endif
  GCDEBUG("Arena %d created\n", id);
  return arena;
}

static GCArena *getarena_forflags(global_State *g, int requiredflags)
{
  if (requiredflags & ArenaFlag_LongLived) {
    return g->llivedarena;
  } else {
    return (requiredflags & ArenaFlag_TravObjs) ? g->travarena : g->arena;
  }
}

/* Find or create new arena to replace the currently active full one */
GCArena *lj_gc_findnewarena(lua_State *L, int flags)
{
  global_State *g = G(L);
  MSize fallback = g->gc.arenastop, id = g->gc.arenastop;
  GCArena *arena = NULL, *curarena = getarena_forflags(g, flags);
  uint32_t wantedflags = flags & (ArenaFlag_TravObjs|ArenaFlag_LongLived);
  int travobj = flags & ArenaFlag_TravObjs;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    ArenaFlags arenaflags = lj_gc_arenaflags(g, i);

    if ((arenaflags & (ArenaFlag_NoBump|ArenaFlag_Explicit|ArenaFlag_Empty)) ==
        ArenaFlag_Empty) {
      if ((arenaflags & wantedflags) == wantedflags) {
        id = i;
        break;
      } else if(fallback == g->gc.arenastop) {
        /* have to switch the arena mode to/from traversable objects */
        fallback = i;
      }
    }
  }

  if (id != g->gc.arenastop) {
    arena = lj_gc_arenaref(g, id);
    fallback = 0;
  }else if (fallback != g->gc.arenastop) {
    arena = lj_gc_arenaref(g, fallback);
    id = fallback;
  } else {
    arena = NULL;
  }

  if (!arena) {
    arena = lj_gc_newarena(L, wantedflags);
  } else {
    uint32_t clearflags = ArenaFlag_Empty;
    /* Switching a non traversable arena to traversable objects */
    if (fallback) {
      if (travobj) {
        lj_gc_setarenaflag(g, id, ArenaFlag_TravObjs);
      } else {
        clearflags |= ArenaFlag_TravObjs;
      }
      arena_setobjmode(L, arena, travobj);
    }
    lj_gc_cleararenaflags(g, id, clearflags);
  }
  lj_gc_setactive_arena(L, arena, wantedflags);
  return arena;
}

void lj_gc_freearena(global_State *g, GCArena *arena)
{
  int i = lj_gc_getarenaid(g, arena);
  lua_assert(i != -1 && i != 0);/* GG arena must never be destroyed here */
  lua_assert(arena_firstallocated(arena) == 0);

  arena_destroy(g, arena);
  if (i != (g->gc.arenastop-1)) {
    MSize swapi = g->gc.arenastop - 1;
    GCArena *arena2 = lj_gc_arenaref(g, swapi);
    arena2->extra.id = i;
    /* Swap the arena at the end of the list to the position of the one we removed */
    g->gc.arenas[i] = g->gc.arenas[swapi];
    memcpy(g->gc.freelists + i, g->gc.freelists + swapi, sizeof(ArenaFreeList));
    setmref(arena2->freelist, g->gc.freelists + i);
    g->gc.freelists[i].owner = arena2;
  } 
  g->gc.arenastop--;
}

static void sweep_arena(global_State *g, MSize i, MSize celltop);

int lj_gc_isarena_active(global_State *g, GCArena *arena)
{
  return g->arena == arena || g->travarena == arena || g->llivedarena == arena;
}

GCArena *lj_gc_setactive_arena(lua_State *L, GCArena *arena, ArenaFlags newflags)
{
  global_State *g = G(L);
  GCArena *old = getarena_forflags(g, newflags);
  MSize id = arena->extra.id;
  MSize flags = lj_gc_arenaflags(g, id);
  lua_assert(lj_gc_getarenaid(g, arena) != -1);
#ifdef LJ_ENABLESTATS
  if (g->vmevent_data) {
    /* log_arenaactive((UserBuf *)g->vmevent_data, id, arena_topcellid(arena), flags); */
  }
#endif

  if ((g->gc.state == GCSsweep || g->gc.state == GCSsweepstring)) {
    lj_gc_setarenaflag(g, id, ArenaFlag_SweepNew);

    if (flags & ArenaFlag_Empty) {
      /* Don't try to sweep newly active arenas that are empty */
      lj_gc_setarenaflag(g, id, ArenaFlag_Swept);
    } else if (!(lj_gc_arenaflags(g, id) & ArenaFlag_Swept)) {
      sweep_arena(g, id, 0);
    }
  }
  /* Pre-emptively clear empty flag so we don't have to check every allocation */
  lj_gc_cleararenaflags(g, id, ArenaFlag_Empty);

  GCDEBUG("setactive_arena: %d\n", id);

  if (newflags & ArenaFlag_LongLived) {
    g->llivedarena = arena;
  } else if (flags & ArenaFlag_TravObjs) {
    g->travarena = arena;
  } else {
    lj_gc_cleararenaflags(g, id, ArenaFlag_TravObjs);
    g->arena = arena;
  }
  lj_gc_setarenaflag(g, id, newflags);
  return old;
}

void lj_gc_init(lua_State *L)
{
  global_State *g = G(L);
  g->gc.state = GCSpause;
  setgcref(g->gc.root, obj2gco(L));
  g->gc.total = sizeof(GG_State);
  g->gc.pause = LUAI_GCPAUSE;
  g->gc.stepmul = LUAI_GCMUL;
  setmref(g->gc.weak, lj_mem_newvec(L, 8, GCRef));
  g->gc.weakcapacity = 8;
  g->gc.weaknum = 0;

  g->gc.arenas = lj_mem_newvec(L, 16, GCArena*);
  g->gc.arenassz = 16;
  g->gc.arenastop = 0;
  g->gc.freelists = lj_mem_newvec(L, 16, ArenaFreeList);
  memset(g->gc.freelists, 0, sizeof(ArenaFreeList) * 16);

  register_arena(L, g->travarena, ArenaFlag_TravObjs|ArenaFlag_GGArena);
  arena_creategreystack(L, g->travarena);

  g->arena = lj_gc_newarena(L, 0);
  g->llivedarena = lj_gc_newarena(L, ArenaFlag_LongLived|ArenaFlag_TravObjs);
}

/* -- Collector ----------------------------------------------------------- */

static size_t gc_mark_threads(global_State *g)
{
  size_t m = 0;
  GCRef gray = g->gc.grayagain;
  while (gcref(gray) != NULL) {
    lua_State *th = gco2th(gcref(gray));
    gc_traverse_thread(g, th);
    lua_assert(gcref(gray) != gcref(th->gclist));
    gray = th->gclist;
  }
  if (!g->gc.isminor) {
    setgcrefnull(g->gc.grayagain);
  }
  return m;
}

void lj_gc_setdeferredmark(lua_State *L, GCobj *o)
{
  global_State *g = G(L);
  if (!gc_ishugeblock(o)) {
    GCArena *arena = ptr2arena(o);
    if (arena_adddefermark(L, arena, o)) {
      lj_gc_setarenaflag(g, arena_extrainfo(arena)->id, ArenaFlag_DeferMarks);
    }
  } else {
    lua_assert(0);
  }
}

static void gc_deferred_marking(global_State *g)
{
  MSize i = 0;
  for (; i < g->gc.arenastop; i++) {
    if (!(lj_gc_arenaflags(g, i) & ArenaFlag_DeferMarks)) {
      continue;
    }

    GCArena *arena = lj_gc_arenaref(g, i);
    MSize j = 0;
    CellIdChunk *list = arena_freelist(arena)->defermark;
    MSize listmarks = list->count >> 5;

    for (; j < list->count; j++) {
      GCCellID1 id = list->cells[j];
      GCobj *o = (GCobj *)arena_cell(arena, id);
      if (arena_cellismarked(arena, id)) {
        if (o->gch.gct == ~LJ_TTHREAD) {
          gc_traverse_thread(g, (lua_State *)o);
        }
      } else {

      }
    }
  }
}

static void sweep_threaduv(lua_State *th)
{
  GCupval *uv = gcrefp(th->openupval, GCupval);
  GCRef *prev = &th->openupval;

  for (; uv; uv = gcrefp(uv->nextgc, GCupval)) {
    if (arenaobj_iswhite(obj2gco(uv))) {
      GCDEBUG("sweep_thread_uv(%d, %d)\n", ptr2arena(obj2gco(uv))->extra.id, ptr2cell(obj2gco(uv)));
      setgcrefr(*prev, uv->nextgc);
    } else {
      prev = &uv->nextgc;
    }
  }
}

void gc_deferred_sweep(global_State *g)
{
  MSize i = 0;
  for (; i < g->gc.arenastop; i++) {
    if (!(lj_gc_arenaflags(g, i) & ArenaFlag_DeferMarks)) {
      continue;
    }

    GCArena *arena = lj_gc_arenaref(g, i);
    MSize j = 0;
    CellIdChunk *list = arena_freelist(arena)->defermark;
    for (; j < idlist_count(list); ) {
      GCCellID1 id = list->cells[j];
      GCobj *o = (GCobj *)arena_cell(arena, id);

      if (arena_cellismarked(arena, id)) {
        if (o->gch.gct == ~LJ_TTHREAD) {
          sweep_threaduv((lua_State *)o);
        } else if(o->gch.gct == ~LJ_TTAB) {
          idlist_remove(list, j, 0);
        }
        j++;
      } else {
        if (o->gch.gct == ~LJ_TTHREAD) {
          lj_state_free(g, (lua_State *)o);
        } else {

        }
        /* TODO: handle mark bits if we use them */
        lua_assert((list->count >> 15) == 0);
        idlist_remove(list, j, 0);
      }
    }
  }
}

static void atomic_check_still_weak(global_State* g)
{
  GCRef *weak = mref(g->gc.weak, GCRef);
  uint32_t n = g->gc.weaknum;
  uint32_t i = 0;
  g->gc.weaknum = 0;
  for (; i < n; ++i) {
    gc_traverse_tab(g, gco2tab(gcref(weak[i])));
  }
}

static int gc_mayclear(global_State *g, cTValue *o, uint8_t val)
{
  if (tvisgcv(o)) {
    if (tvisstr(o)) {
      gc_mark_str(g, strV(o));
      return 0;
    }
    if (iswhite(g, gcV(o)))
      return 1;
    if (val && tvistabud(o) && (gcV(o)->gch.marked & val))
      return 1;
  }
  return 0;
}

static void atomic_clear_weak(global_State *g)
{
  GCRef *weak = mref(g->gc.weak, GCRef);
  uint32_t wi = g->gc.weaknum;
  g->gc.weaknum = 0;
  while (wi--) {
    GCtab *t = gco2tab(gcref(weak[wi]));
    lua_assert((t->marked & LJ_GCFLAG_WEAK));
    //lua_assert((t->marked & LJ_GCFLAG_GREY));
    if (g->gc.isminor) {
     // cleargray(t);
    }
    if ((t->marked & LJ_GCFLAG_WEAKVAL)) {
      MSize i = t->asize;
      while (i) {
        TValue *tv = arrayslot(t, --i);
        if (gc_mayclear(g, tv, LJ_GCFLAG_FINALIZED))
          setnilV(tv);
      }
    }
    t->marked &= ~(LJ_GCFLAG_GREY | LJ_GCFLAG_WEAK);
    if (t->hmask) {
      Node *n = noderef(t->node);
      MSize i = t->hmask + 1;
      for (; i; --i, ++n) {
        if (tvisnil(&n->val))
          continue;
        if (gc_mayclear(g, &n->key, 0) ||
            gc_mayclear(g, &n->val, LJ_GCFLAG_FINALIZED))
          setnilV(&n->val);
      }
    }
  }
}

static void atomic_mark_misc(global_State *g)
{
#if LJ_HASFFI
  CTState *cts = mref(g->ctype_state, CTState);
  if (cts) {
    gc_markobj(g, obj2gco(cts->miscmap));
    gc_markobj(g, obj2gco(cts->finalizer));
    /*
    while (g->gc.sweeppos < cts->top) {
      CType *ct = cts->tab + g->gc.sweeppos++;
      gc_marknleaf(g, gcref(ct->name));
    }
    */
  }
#endif
}

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;
  VERIFYGC(g);
  SECTION_START(gc_atomic);
  lj_gc_drain_ssb(g); /* Mark anything left in the gray SSB buffer */
  VERIFYGC(g);
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  /* Empty the list of weak tables. */
  lua_assert(!iswhite(g, obj2gco(mainthread(g))));
  gc_markthread(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  atomic_mark_misc(g);
  gc_propagate_gray(g);  /* Propagate all of the above. */

  gc_deferred_marking(g);
  /* Empty the 2nd chance list. */
  gc_mark_threads(g);
  gc_propagate_gray(g);

  atomic_check_still_weak(g);
  
  atomic_enqueue_finalizers(L);
  VERIFYGC(g);
  /* Flag white objects in the arena finalizer list. */
  udsize = lj_gc_scan_finalizers(g, 0);
  VERIFYGC(g);
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */
  
  atomic_clear_weak(g);/* Clear dead and finalized entries from weak tables*/
  
  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* All marking done, clear weak tables. */
  gc_sweep_uv(g);
  gc_deferred_sweep(g);

  if ((g->gc.stateflags & GCSFLAG_TOMINOR) && 0) {
    lua_assert(!g->gc.isminor);
    g->gc.stateflags &= ~GCSFLAG_TOMINOR;
    g->gc.isminor = 15;
    GCDEBUG("MINORGC(Started)  count %d", g->gc.isminor);
  }

  /* Prepare for sweep phase. */
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
  assert_greyempty(g);
  SECTION_END(gc_atomic);
  VERIFYGC(g);
}

#if DEBUG
#define PreSweepArena(g, arena, i)  sweepcallback(g, arena, i, -1)
#define PostSweepArena(g, arena, i, count) sweepcallback(g, arena, i, count)
#else
#define PreSweepArena(g, arena, i)  
#define PostSweepArena(g, arena, i, count)
#endif

void sweepcallback(global_State *g, GCArena *arena, MSize i, int count);

static void sweep_arena(global_State *g, MSize i, MSize celltop)
{
  lua_assert(!(lj_gc_arenaflags(g, i) & ArenaFlag_Swept));
  GCArena *arena = lj_gc_arenaref(g, i);
  ArenaFreeList *freelist = g->gc.freelists+i;
  MSize count, empty, currtop = arena_topcellid(arena);
  PreSweepArena(g, arena, i);
  TicksStart();

  if (celltop) {
    lua_assert(celltop <= currtop);
    /* FIXME: The sweep functions mark in SIMD vector sized chunks which may
    ** go past the celltop sweep limit so set a simd sized chunk of cell mark
    ** bits past the sweep limit to black as well.
    */
    GCCellID endid = celltop+256;
    if (endid > MaxCellId) {
      endid = MaxCellId;
    }
    arena_setrangeblack(arena, celltop, endid);
  }
  if (g->gc.isminor) {
    count = arena_minorsweep(arena, celltop);
  } else {
    count = arena_majorsweep(arena, celltop);
  }

  if (celltop) {
    //arena_setrangewhite(arena, celltop, celltop+256);
  }
  empty = (count & 0x10000) == 0;

  /* The sweep found large free ranges that bump allocation can happen in */
  if (count & 0x20000) {
    lj_gc_cleararenaflags(g, i, ArenaFlag_NoBump);
  }
#ifdef LJ_ENABLESTATS
  if (g->vmevent_data) {
    /* log_arenasweep((UserBuf *)g->vmevent_data, i, empty, (uint32_t)TicksEnd(), count & 0xffff, currtop, lj_gc_arenaflags(g, i)); */
  } 
#endif

  PostSweepArena(g, arena, i, count);
  count &= 0xffff;
  /* If there no more live cells left flag the arena as empty and reset its bump and block state */
  if (empty) {
    lua_assert(g->gc.total > arena_totalobjmem(arena) && currtop > freelist->freecells);
    MSize amem = arena_totalobjmem(arena);
    /* Don't flag active arenas as empty because the empty flag is cleared
    ** on setting an arena active and is used to filter out arenas for grey queue
    ** processing during mark propagation.
    */
    if (!lj_gc_isarena_active(g, arena)) {
      lj_gc_setarenaflag(g, i, ArenaFlag_Empty);
    }
    lj_gc_cleararenaflags(g, i, ArenaFlag_NoBump|ArenaFlag_ScanFreeSpace);
    arena_reset(arena);
    g->gc.total -= amem;
    VERIFYGC(g);
  } else {
    freelist->freeobjcount += count;
    g->gc.deadnum += count;
    /* FIXME: Decide and test the right size threshold to trigger freespace scan */
    if (freelist->freeobjcount > 100) {
      lj_gc_setarenaflag(g, i, ArenaFlag_ScanFreeSpace);
    }
  }
  lj_gc_setarenaflag(g, i, ArenaFlag_Swept);
}

static void sweep_traces(global_State *g)
{
  jit_State *J = G2J(g);

  for (int i = J->sizetrace-1; i > 0; i--) {
    GCtrace *t = (GCtrace *)gcref(J->trace[i]);
    lua_assert(!t || t->traceno == i);
    if (t && iswhite(g, t)) {
      lj_trace_free(g, t);
    }
  }
}

static void arenasweep_start(global_State *g)
{
  MSize nontrav = 0, trav = 0;

  /* Turn off minor gc now if this is the last one */
  if (g->gc.isminor) {
    g->gc.isminor--;
  }

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    // arena_dumpwhitecells(g, lj_gc_arenaref(g, i));
  }

  /* Don't try to sweep active arenas if they were created while the gc is past the mark phase
  ** See lj_gc_setactive_arena where this checked and we flag the arena as swept.
  */
  if (!(lj_gc_arenaflags(g, g->arena->extra.id) & ArenaFlag_Swept)) {
    sweep_arena(g, g->arena->extra.id, arena_freelist(g->arena)->sweeplimit);
    lj_gc_setarenaflag(g, g->arena->extra.id, ArenaFlag_SweepNew);
  }

  if (g->travarena != g->arena && !(lj_gc_arenaflags(g, g->travarena->extra.id) & ArenaFlag_Swept)) {
    sweep_arena(g, g->travarena->extra.id, arena_freelist(g->travarena)->sweeplimit);
    lj_gc_setarenaflag(g, g->travarena->extra.id, ArenaFlag_SweepNew);
  }

  if (!(lj_gc_arenaflags(g, g->llivedarena->extra.id) & ArenaFlag_Swept)) {
    sweep_arena(g, g->llivedarena->extra.id, arena_freelist(g->llivedarena)->sweeplimit);
    lj_gc_setarenaflag(g, g->travarena->extra.id, ArenaFlag_SweepNew);
  }
  g->gc.curarena = 0;
  //g->gc.total -= g->gc.atotal;
}

static int arenasweep_step(global_State *g)
{
  MSize i;
  for (i = g->gc.curarena; i < g->gc.arenastop; i++) {
    if (!(lj_gc_arenaflags(g, i) & (ArenaFlag_Swept|ArenaFlag_Empty))) {
      sweep_arena(g, i, 0);
      g->gc.curarena = i+1;
      return GCSWEEPCOST;
    }
  }
  g->gc.curarena = g->gc.arenastop;
  /* All arenas sweepable have been swept */
  hugeblock_sweep(g);
  return 0;
}

static void gc_sweepstart(global_State *g)
{
  lua_assert(isblack(g, &g->strempty));
  lua_assert(isblack(g, mainthread(g)));
  g->gc.curarena = 0;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    lj_gc_cleararenaflags(g, i, ArenaFlag_Swept|ArenaFlag_SweepNew);
  }

  sweep_traces(g);
  g->gc.sweepstr = 0;

  /* Don't try to sweep active arenas if they were created while the gc is past the mark phase
  ** See lj_gc_setactive_arena where this checked and we flag the arena as swept.
  */
  lj_gc_setarenaflag(g, g->arena->extra.id, ArenaFlag_SweepNew);
  lj_gc_setarenaflag(g, g->travarena->extra.id, ArenaFlag_SweepNew);

  sweep_arena(g, g->travarena->extra.id, 0);
  
  /* Record celltop so we only sweep upto it and not objects created after the
  ** mark phase that will be still white
  */
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    arena_freelist(arena)->sweeplimit = arena->celltopid;
  }
}

static void gc_finish(lua_State *L);

static int gc_sweepend(lua_State *L)
{
  global_State *g = G(L);
  g->gc.sweepi = 0;
  g->gc.sweep = NULL;
  g->gc.curarena = 0;
  if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
    lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */

  if (!g->gc.isminor) {
    g->gc.stateflags |= GCSFLAG_TOMINOR;
  }

  if (g->gc.stateflags & GCSFLAG_HASFINALIZERS) {  /* Need any finalizations? */
    gc_setstate(g, GCSfinalize);
#if LJ_HASFFI
    g->gc.nocdatafin = 1;
#endif
    return 1;
  } else {  /* Otherwise skip this phase to help the JIT. */
    gc_finish(L); /* End of GC cycle. */
    return 0;
  }
}

#define ATOMIC_SWEEP 1
#define ATOMIC_PROPAGATE 1

/* Perform both sweepstring and sweep in single atomic call instead of 
** incrementally to help track down GC phase bugs. 
*/
static void gc_atomicsweep(lua_State *L)
{
  global_State *g = G(L);
  GCSize old = g->gc.total;

  if (g->gc.state == GCSatomic) {
    gc_setstate(g, GCSsweepstring);
    gc_sweepstart(g);   
  }

  if (g->gc.state == GCSsweepstring) {
    while (gc_sweepstring(g));
  }

  gc_setstate(g, GCSsweep);
  arenasweep_start(g);
  while (arenasweep_step(g));

  lua_assert(old >= g->gc.total);
  g->gc.estimate -= old - g->gc.total;
  gc_sweepend(L);
}

static void gc_finish(lua_State *L)
{
  global_State *g = G(L);
  MSize travnum = 0, emptycount = 0, empty_trav = 0, i;
  GCSize estdead = 0, free = 0, bumpleft = 0;
  gc_setstate(g, GCSpause);
  g->gc.stateflags &= ~GCSFLAG_HASFINALIZERS;
  g->gc.debt = 0;
  
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    MSize flags = lj_gc_arenaflags(g, i) & (ArenaFlag_Empty | ArenaFlag_Explicit |  ArenaFlag_TravObjs);
    GCArena *arena = lj_gc_arenaref(g, i);
    if (flags & ArenaFlag_Empty) {
      emptycount++;
      if (flags & ArenaFlag_TravObjs)
        empty_trav++;
    } else {
      MSize bumpspare = (MaxUsableCellId - arena->celltopid) * 16;
      MSize freesz = arena_get_freecellcount(arena) * 16;
      //lua_assert((freesz != 0 && freesz < ((arena_topcellid(arena)-MinCellId)) * 16) || g->gc.freelists[i].freeobjcount == 0);
      free += freesz;
      bumpleft += bumpspare;

      MSize avgsz = flags & ArenaFlag_TravObjs ? 32 : 24;
      estdead += (GCSize)(g->gc.freelists[i].freeobjcount * avgsz);
    }
  }

  if (emptycount && (g->gc.arenastop - emptycount) > 6) {
    for (MSize i = 0; i < g->gc.arenastop; ) {
      MSize flags = lj_gc_arenaflags(g, i) & (ArenaFlag_Empty | ArenaFlag_Explicit);
      if (flags == ArenaFlag_Empty && 0) {
        lj_gc_freearena(g, lj_gc_arenaref(g, i));
      } else {
        i++;
      }
    }
  }

  GCSize arena_total = (g->gc.arenastop - emptycount) * ArenaMaxObjMem - bumpleft;
  double freemb = free / (1024.0*1024.0);
  GCSize bumpsize = emptycount * ArenaMaxObjMem + bumpleft;
  GCSize real_atotal = arena_total - free;

  double trigratio = g->gc.arenagrowth/(double)g->gc.threshold;  
  double trig_atotal = real_atotal/(double)g->gc.threshold;

  GCDEBUG("GCFinish: freecells= %g MB, \n", freemb);

  GCSize basetotal = g->gc.hugemem + g->gc.ctotal;
  g->gc.total = basetotal + arena_total;
  g->gc.estimate = basetotal + arena_total;
  //g->gc.estimate -= free;

  if (emptycount > 6) {
    GCSize target = arena_total + (emptycount/2) *ArenaMaxObjMem;
    double agrowth = g->gc.arenagrowth / (double)ArenaMaxObjMem;
    //target += g->gc.arenagrowth g->gc.stepmul;
    target += basetotal;

    g->gc.threshold = target;
  } else {
    g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  }
  
  lua_assert(g->gc.threshold > g->gc.total);
  g->gc.arenagrowth = 0;
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate: {
    TIMER_START(gc_propagate);
#if ATOMIC_PROPAGATE
    GCSize total = 0;
    while(gc_propagate_gray(g));
#else
   GCSize total = gc_propagate_gray(g); /* Propagate one gray object. */
#endif
   TIMER_END(gc_propagate);
    if (total != 0) {
      return total;
    } else {
      gc_setstate(g, GCSatomic);  /* End of mark phase. */
      return 0;
    }
  }
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
#if ATOMIC_SWEEP
    gc_atomicsweep(L);
#else
    gc_setstate(g, GCSsweepstring);/* Start of sweep phase. */
    gc_sweepstart(g);
#endif
    return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
   // while (gc_sweepstring(g));
    TIMER_START(sweepstring);
    gc_sweepstring(g);  /* Sweep one chain. */
    if (g->gc.sweepstr > g->strmask) {
      /* All string hash chains sweeped. */
      TIMER_END(sweepstring);
      gc_setstate(g, GCSsweep);
      arenasweep_start(g);
    }
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    TIMER_END(sweepstring);
    return GCSWEEPCOST;
  }
  case GCSsweep: {
    GCSize old = g->gc.total;
    int sweepcost = arenasweep_step(g);
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;

    if (sweepcost != 0) {
      return sweepcost;
    }
    gc_sweepend(L);
    return GCSWEEPMAX*GCSWEEPCOST;
  }
  case GCSfinalize:
    if (g->gc.stateflags & GCSFLAG_HASFINALIZERS) {
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
        return LJ_MAX_MEM;
      TIMER_START(gc_finalize);
      /* Finalize one object. */
      if (gc_finalize_step(L)) {
        if (g->gc.estimate > GCFINALIZECOST)
          g->gc.estimate -= GCFINALIZECOST;
        TIMER_END(gc_finalize);
        return GCFINALIZECOST;
      }
      TIMER_END(gc_finalize);
    }
#if LJ_HASFFI
    if (!g->gc.nocdatafin) {
      lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
    }
#endif
    gc_finish(L);
    return 0;
  default:
    lua_assert(0);
    return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step_internal(lua_State *L)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = g->vmstate;
  int ret = 0;
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    lim -= (GCSize)gc_onestep(L);
    if (g->gc.state == GCSpause) {
      //g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    //g->gc.threshold = g->gc.total + GCSTEPSIZE;
    g->vmstate = ostate;
    ret = -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
   // g->gc.threshold = g->gc.total;
    g->vmstate = ostate;
    ret = 0;
  }
  return ret;
}

int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  TIMER_START(gc_step);
  int result = lj_gc_step_internal(L);
  TIMER_END(gc_step);
  return result;
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->cur_L));
  L->base = tvref(G(L)->jit_base);
  L->top = curr_topL(L);
  TIMER_START(gc_step);
  while (steps-- > 0 && lj_gc_step_internal(L) == 0){}
  TIMER_END(gc_step);
  if ((G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize)) {
    G(L)->gc.gcexit = 1;
    /* Return 1 to force a trace exit. */
    return 1;
  } else {
    return 0;
  }
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  SECTION_START(gc_fullgc);
  if (g->gc.state != GCSpause) {
    /*FIXME: redesign assumption that we have 2 whites is no longer true  */
    if (g->gc.state == GCSatomic) {  /* Caught somewhere in the middle. */
      gc_setstate(g, GCSsweepstring);  /* Fast forward to the sweep phase. */
      g->gc.sweepstr = 0;
    } else if (g->gc.state != GCSsweepstring) {

      if (g->gc.state == GCSpropagate) {
        for (MSize i = 0; i < g->gc.arenastop; i++) {
          arena_towhite(lj_gc_arenaref(g, i));
        }
        gc_setstate(g, GCSpause);
        g->gc.ssbsize = 0;
      } else {
        lua_assert(0);
      }
    }
    while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
      gc_onestep(L);  /* Finish sweep. */
  }

  if (0) {
    g->gc.isminor = 3;
  }

  lua_assert(g->gc.state == GCSfinalize || g->gc.state == GCSpause);
  /* Now perform a full GC. */
  if (g->gc.state != GCSpause) {
    gc_setstate(g, GCSpause);
  }

  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
  SECTION_END(gc_fullgc);
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lua_State *L = mainthread(g);
  lua_assert(!isdead(g, v) && !isdead(g, o));
  //lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  PERF_COUNTER(gc_barrierf);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.statebits & GCSneedsbarrier) {
    /* Move frontier forward. */
    lj_gc_appendgrayssb(g, o);
  }
  setgray(o);

  if ((g->gc.state == GCSsweep || g->gc.state == GCSsweepstring) && iswhite(g, v)) {
    arena_markcell(ptr2arena(v), ptr2cell(v));
    /* Don't clear gray */
    //makewhite(g, o);  /* Make it white to avoid the following barrier. */
  }
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
  lua_State *L = mainthread(g);
  lua_assert(tvisgcv(tv));
  PERF_COUNTER(gc_barrieruv);
/* Adjust the TValue pointer to upvalue its contained in */
#define TV2MARKED(x) \
  ((GCupval *)(((char*)x)-offsetof(GCupval, tv)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    gc_marktv(g, tv);
    //lj_gc_appendgrayssb(g, gcV(tv));
  } else if (g->gc.isminor) {
    if (arenaobj_isblack(TV2MARKED(tv))) {
      lj_gc_appendgrayssb(g, obj2gco(TV2MARKED(tv)));
      TV2MARKED(tv)->marked |= LJ_GCFLAG_GREY;
    }
  } else {
    TV2MARKED(tv)->marked |= LJ_GCFLAG_GREY;
  }
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  //lua_assert(0); /*TODO: open upvalue cell id list for arenas . could also be list of threads in the arena as well */

  if (tvisgcv(&uv->tv) && g->gc.statebits & GCSneedsbarrier) {
    if (arenaobj_isblack(o)) {
      gc_markobj(g, gcV(&uv->tv));
    }
  }
  cleargray(uv);
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    gc_marktrace(g, traceno);
  } else if (g->gc.isminor) {
    /* TODO: is this worth doing for short lived function/traces
    GCtrace *t = traceref(G2J(g), traceno);
    if (gcrefp(t->startpt, GCproto)->trace == traceno) {
      cleargray(gcref(t->startpt));
      // Starting proto might die before
      lj_gc_appendgrayssb(g, gcref(t->startpt));
    } else {
      gc_marktrace(g, traceno);
    }
    */
    gc_marktrace(g, traceno);
  }
}
#endif

void LJ_FASTCALL lj_gc_drain_ssb(global_State *g)
{
  MSize i;

  GCDEBUG("lj_gc_drain_ssb: size %d\n", g->gc.ssbsize);
  if (!g->gc.isminor && g->gc.state != GCSpropagate && g->gc.state != GCSatomic) {
    g->gc.ssbsize = 0;
    return;
  }

  TIMER_START(gc_emptygrayssb);
  /* Skip first dummy value */
  for (i = 0; i < g->gc.ssbsize; i++) {
    GCobj *o = gcref(g->gc.ssb[i]);
    if (!gc_ishugeblock(o)) {
      /* Skip false positive triggered barriers since fast barrier only checks greybit
      ** and not if the object being stored into is black
      */
      if (arenaobj_iswhite(o)) {
        continue;
      }
      arena_marktrav(g, o);
    } else {
      hugeblock_mark(g, o);
    }
  }
  g->gc.ssbsize = 0;
  TIMER_END(gc_emptygrayssb);
}

void lj_gc_setfixed(lua_State *L, GCobj *o)
{
  lua_assert(o->gch.gct == ~LJ_TSTR);
  if (o->gch.marked & LJ_GC_FIXED)
    return;
  o->gch.marked |= LJ_GC_FIXED;

  if (!gc_ishugeblock(o)) {
    GCArena *arena = ptr2arena(o);
    arena_setfixed(L, ptr2arena(o), o);
    /* Fixed objects should always be black */
    arena_markcell(arena, ptr2cell(o));
  } else {
    hugeblock_setfixed(G(L), o);
  }
}

/* -- Allocator ----------------------------------------------------------- */

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lua_assert((osz == 0) == (p == NULL));
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lua_assert((nsz == 0) == (p == NULL));
  lua_assert(checkptrGC(p));
  g->gc.total = (g->gc.total - osz) + nsz;
  g->gc.ctotal = (g->gc.ctotal - osz) + nsz;
  lua_assert(g->gc.total >= sizeof(GG_State));
  return p;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  GCobj *o = lj_mem_newgco_t(L, size, ~LJ_TCDATA);
  return o;
}

void * LJ_FASTCALL lj_mem_newcd(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = lj_mem_newgco_t(L, size, ~LJ_TCDATA);
  return o;
}

int lj_mem_tryreclaim(lua_State *L)
{
  global_State *g = G(L);
  MSize fallback = g->gc.arenastop, id = g->gc.arenastop;
  GCArena *arena = NULL;
  uint32_t wantedflags;
  lua_assert(0 && "TODO: emergency GC to reclaim some memory for allocation");
/*
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags arenaflags = lj_gc_arenaflags(g, i);

    if ((arenaflags & (ArenaFlag_Explicit|ArenaFlag_ScanFreeSpace)) ==
        ArenaFlag_ScanFreeSpace) {
    }
  }
  */
  return 0;
}

GCobj *findarenaspace(lua_State *L, GCSize osize, int flags)
{
  global_State *g = G(L);
  GCArena *arena = NULL, *curarena = getarena_forflags(g, flags);
  MSize cellnum = arena_roundcells(osize);


  /* FIXME: be smarter about large allocations */
  if (!arena_canbump(curarena, cellnum)) {
    lj_gc_setarenaflag(g, lj_gc_getarenaid(g, curarena), ArenaFlag_NoBump);
  }

  arena = lj_gc_findnewarena(L, flags);

  if (arena == NULL) {
    if (lj_mem_tryreclaim(L)) {
      arena = lj_gc_findnewarena(L, flags);
    } else {
      lj_err_mem(L);
    }
  }
  return (GCobj*)arena_alloc(arena, osize);
}

#define istrav(gct) ((gct) != ~LJ_TSTR && (gct) != ~LJ_TCDATA)

#include <stdio.h>

GCobj *lj_mem_newgco_t(lua_State *L, GCSize osize, uint32_t gct)
{
  global_State *g = G(L);
  GCobj *o ;

  if (osize < ArenaOversized) {
    MSize realsz = lj_round(osize, 16);
    GCArena *arena;
    TIMER_START(newgcobj);

    if (gct == ~LJ_TTRACE) {
      arena = g->llivedarena;
    } else {
      arena = istrav(gct) ? G(L)->travarena : G(L)->arena;
    }
    o = (GCobj*)arena_alloc(arena, osize);
    if (LJ_UNLIKELY(o == NULL)) {
      int flags = istrav(gct) ? ArenaFlag_TravObjs : 0;
      if (gct == ~LJ_TTRACE) {
        flags |= ArenaFlag_LongLived;
      }
      o = findarenaspace(L, osize, flags);
    }
    //GCDEBUG("Alloc(%d, %d) %s\n", ptr2arena(o)->extra.id, ptr2cell(o), lj_obj_itypename[gct]);
    g->gc.total += realsz;
    g->gc.arenagrowth += realsz;
    TIMER_END(newgcobj);
  } else {
    o = hugeblock_alloc(L, osize, gct);
  }
  VERIFYGC(g);
  setgray(o);

  return o;
}

void lj_mem_freegco(global_State *g, void *p, GCSize osize)
{
  if (!gc_ishugeblock(p)) {
    /* TODO: Free cell list */
    lua_assert(!arenaobj_isdead(p));

    arena_free(g, ptr2arena(p), p, osize);
    osize = lj_round(osize, 16);
    g->gc.total -= osize;
    g->gc.arenagrowth -= osize;
  } else {
    hugeblock_free(g, p, osize);
  }
}

void *lj_mem_reallocgc(lua_State *L, GCobj *owner, void *p, GCSize oldsz, GCSize newsz)
{
  global_State *g = G(L);
  void* mem;
  VERIFYGC_SKIPOBJ(g, owner);

  if (newsz) {
    if (newsz < ArenaOversized) {
      TIMER_START(allocgc);
      /* Try to shrink or extend the object inplace if theres free space after the object in the arena */
      if (oldsz && oldsz < ArenaOversized) {
        GCArena *arena = ptr2arena(p);
        if (oldsz < newsz) {
          /*if(arena_tryext(arena, oldsz, newsz))*/
        } else {
          lj_mem_shrinkobj(L, p, (MSize)oldsz, (MSize)newsz);
          TIMER_END(allocgc);
          return p;
        }
      }

      mem = arena_alloc(g->arena, newsz);
      if (mem == NULL) {
        mem = findarenaspace(L, newsz, 0);
      }
      //GCDEBUG("Alloc(%d, %d) %s\n", ptr2arena(mem)->extra.id, ptr2cell(mem), "TabPart");

      /* Set created object black while the gc is sweeping */
      if (((g->gc.statebits & GCSneedsbarrier) && arenaobj_isblack(owner)) || (g->gc.statebits & GCSmakeblack)) {
        arena_markcell(ptr2arena(mem), ptr2cell(mem));
      }
      newsz = lj_round(newsz, 16);
      g->gc.total += (GCSize)newsz;
      g->gc.arenagrowth += newsz;
      TIMER_END(allocgc);
    } else {
      mem = hugeblock_alloc(L, newsz, 0);
    }
    if (oldsz) {
      memcpy(mem, p, oldsz);
    }
  } else {
    mem = NULL;
  }

  //lua_assert(!p || oldsz > 0);
  if (oldsz) {
    lj_mem_freegco(G(L), p, oldsz);
  }
  VERIFYGC_SKIPOBJ(g, owner);
  return mem;
}

/* Alllocate aligned gc object min alignment is 8 */
GCobj *lj_mem_newagco(lua_State *L, GCSize osize, MSize align)
{
  global_State *g = G(L);
  GCobj *o;
  lua_assert(osize > 0 && align > 0 && align < ArenaMetadataSize);
  VERIFYGC(g);
  if ((osize+align) < ArenaOversized) {
    MSize arenasz = lj_round(osize, 16);
    o = (GCobj *)arena_allocalign(g->arena, osize, align);
    if (o == NULL) {
      lua_assert(0);/*FIXME rety for aligned allocations */
      o = findarenaspace(L, osize, 0);
    }

    g->gc.total += osize;
  } else {
    o = hugeblock_alloc(L, osize, ~LJ_TCDATA);
  }
  VERIFYGC(g);
  return o;
}

void lj_mem_shrinkobj(lua_State *L, GCobj *o, MSize osize, MSize newsz)
{
  global_State *g = G(L);
  GCSize slack = osize-newsz;
  GCArena *arena = ptr2arena(o);
  lua_assert(!gc_ishugeblock(o) && !arenaobj_isdead(o) && osize > newsz);
  VERIFYGC(g);
  /* Don't needlessly fragment free space if the returned memory is small */
  if (slack <= (CellSize*2)) {
    return;
  }

  /* TODO: set to correct mark color based on gc state */
  lua_assert(!arenaobj_isdead(o));

  slack = arena_shrinkobj(o, newsz);
  g->gc.total -= slack;
  VERIFYGC(g);
}

#ifdef LJ_TGCVEC

void *lj_gcvec_realloc(lua_State *L, GCobj *owner, void *p, GCSize oldsz, GCSize newsz)
{
  if (oldsz != 0) {
    oldsz += sizeof(GCVecHeader);
    p = ((char *)p) - sizeof(GCVecHeader);
  }

  if (newsz != 0) {
    newsz += sizeof(GCVecHeader);
  }

  p = lj_mem_reallocgc(L, owner, p, oldsz, newsz);

  if (newsz != 0) {
    GCVecHeader *hdr = (GCVecHeader *)p;
    hdr->gct = LJ_TGCVEC;
    setgcrefp(hdr->nextgc, owner);
    p = hdr + 1;
  }
  
  return p;
}

void lj_gcvec_free(global_State *g, void *p, GCSize osize)
{
  GCVecHeader *hdr = (GCVecHeader *)(((char *)p) - sizeof(GCVecHeader));
  lua_assert(hdr->gct == LJ_TGCVEC);
  lj_mem_freegco(g, hdr, osize +  sizeof(GCVecHeader));
}

#endif

void gc_mark_gcvec(global_State *g, void *v, MSize size)
{
#ifdef LJ_TGCVEC
  GCVecHeader *hdr = (GCVecHeader *)(((char *)v) - sizeof(GCVecHeader));
  lua_assert(hdr->gct == LJ_TGCVEC);
  arena_markgcvec(g, hdr, size);
#else
  arena_markgcvec(g, v, size);
#endif
}
