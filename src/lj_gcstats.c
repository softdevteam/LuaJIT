/*
** GC snapshot and stats system
*/

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

#include "lj_gcutil.h"
#include "lj_gcstats.h"
#include <stdio.h>


static void gcstats_walklist(global_State *g, GCobj *liststart, GCObjStat* stats_result);
static void tablestats(GCtab* t, GCStatsTable* result);

size_t gcstats_strings(lua_State *L, GCObjStat* result);


size_t basesizes[~LJ_TNUMX] = {
  0,         // LJ_TNIL	   (~0u)
  0,         // LJ_TFALSE  (~1u)
  0,         // LJ_TTRUE   (~2u)
  0,         // LJ_TLIGHTUD  (~3u)
  sizeof(GCstr),   // LJ_TSTR	   (~4u)
  sizeof(GCupval),   // LJ_TUPVAL	   (~5u) 
  sizeof(lua_State), // LJ_TTHREAD   (~6u)
  sizeof(GCproto),   // LJ_TPROTO	   (~7u)
  sizeof(GCfunc),  // LJ_TFUNC	   (~8u)
  sizeof(GCtrace),   // LJ_TTRACE	   (~9u)
  sizeof(GCcdata),   // LJ_TCDATA	   (~10u)
  sizeof(GCtab),   // LJ_TTAB	   (~11u)
  sizeof(GCudata),   // LJ_TUDATA	   (~12u)
};

const char dynamicsize[~LJ_TNUMX] = {
  0,         // LJ_TNIL	   (~0u)
  0,         // LJ_TFALSE  (~1u)
  0,         // LJ_TTRUE   (~2u)
  0,         // LJ_TLIGHTUD  (~3u)
  0,   // LJ_TSTR	   (~4u)
  sizeof(GCupval),   // LJ_TUPVAL	   (~5u) 
  1, // LJ_TTHREAD   (~6u)
  sizeof(GCproto),   // LJ_TPROTO	   (~7u)
  0,  // LJ_TFUNC	   (~8u)
  0,   // LJ_TTRACE	   (~9u)
  sizeof(GCcdata),   // LJ_TCDATA	   (~10u)
  1,   // LJ_TTAB	   (~11u)
  0,   // LJ_TUDATA	   (~12u)
};

const char typeconverter[~LJ_TNUMX] = {
  -1,          // LJ_TNIL	   (~0u)
  -1,          // LJ_TFALSE  (~1u)
  -1,          // LJ_TTRUE   (~2u)
  -1,          // LJ_TLIGHTUD  (~3u)
  gcobj_string,    // LJ_TSTR    (~4u)
  gcobj_upvalue,     // LJ_TUPVAL	 (~5u) 
  gcobj_thread,    // LJ_TTHREAD   (~6u)
  gcobj_funcprototype, // LJ_TPROTO	 (~7u)
  gcobj_function,    // LJ_TFUNC	 (~8u)
  gcobj_trace,     // LJ_TTRACE  (~9u)
  gcobj_cdata,     // LJ_TCDATA  (~10u)
  gcobj_table,     // LJ_TTAB	   (~11u)
  gcobj_udata,     // LJ_TUDATA	 (~12u)
};

const int8_t invtypeconverter[gcobj_MAX] = {
  (int8_t)~LJ_TSTR,    //  gcobj_string,     (~4u)
  (int8_t)~LJ_TUPVAL,  //  gcobj_upvalue,    (~5u) 
  (int8_t)~LJ_TTHREAD, //  gcobj_thread,     (~6u)
  (int8_t)~LJ_TPROTO,  //  gcobj_funcprototype,(~7u)
  (int8_t)~LJ_TFUNC,   //  gcobj_function,   (~8u)
  (int8_t)~LJ_TTRACE,  //  gcobj_trace,    (~9u)
  (int8_t)~LJ_TCDATA,  //  gcobj_cdata,    (~10u)
  (int8_t)~LJ_TTAB,    //  gcobj_table,    (~11u)
  (int8_t)~LJ_TUDATA,  //  gcobj_udata,    (~12u)
};      

//TODO: do counts of userdata based on grouping by hashtable pointer
LUA_API void gcstats_collect(lua_State *L, GCStats* result)
{
  global_State *g = G(L);
  GCObjStat objstats[~LJ_TNUMX] = {0};
  int i = 0;

  gcstats_walklist(g, gcref(g->gc.root), objstats);

  gcstats_strings(L, &objstats[~LJ_TSTR]);

  tablestats(tabV(&g->registrytv), &result->registry);
  tablestats(tabref(L->env), &result->globals);

  //Adjust the object slot indexes for external consumption
  for (i = 0; i < gcobj_MAX; i++)
  {
    memcpy(&result->objstats[i], &objstats[invtypeconverter[i]], sizeof(GCObjStat));
  }
}

size_t gcstats_strings(lua_State *L, GCObjStat* result)
{
  global_State *g = G(L);
  size_t count = 0, maxsize = 0, totalsize = 0;
  GCobj *o;

  for (MSize i = 0; i <= g->strmask; i++)
  {
    /* walk all the string hash chains. */
    o = gcref(g->strhash[i]);

    while (o != NULL)
    {
      size_t size = sizestring(&o->str);

      totalsize += size;
      count++;
      maxsize = size < maxsize ? maxsize : size;

      o = gcref(o->gch.nextgc);
    }
  }

  result->count = count;
  result->totalsize = totalsize;
  result->maxsize = maxsize;

  return count;
}

void gcstats_walklist(global_State *g, GCobj *liststart, GCObjStat* result)
{

  GCobj *o = liststart;
  GCObjStat stats[~LJ_TNUMX] = {0};

  if (liststart == NULL)
  {
    return;
  }

  while (o != NULL)
  {
    int gct = o->gch.gct;
    size_t size = gcobj_size(o);

    stats[gct].count++;
    stats[gct].totalsize += size;
    stats[gct].maxsize = stats[gct].maxsize > size ? stats[gct].maxsize : size;

    o = gcref(o->gch.nextgc);
  }

  for (size_t i = ~LJ_TSTR; i < ~LJ_TNUMX; i++)
  {
    result[i].count += stats[i].count;
    result[i].totalsize += stats[i].totalsize;
    result[i].maxsize = stats[i].maxsize > result[i].maxsize ? result[i].maxsize : stats[i].maxsize;
  }
}

typedef struct GCSnapshotHandle{
  lua_State *L;
  LJList list;
  SBuf sb;
} GCSnapshotHandle;

static int dump_gcobjects(lua_State *L, GCobj *liststart, LJList* list, SBuf* buf);

GCSnapshot* gcsnapshot_create(lua_State *L)
{
  GCSnapshotHandle* handle = lj_mem_newt(L, sizeof(GCSnapshotHandle)+ sizeof(GCSnapshot), GCSnapshotHandle);
  GCSnapshot* snapshot = (GCSnapshot*)&handle[1];

  handle->L = L;
  lj_buf_init(L, &handle->sb);
  lj_list_init(L, &handle->list, 32, SnapshotObj);

  dump_gcobjects(L, gcref(G(L)->gc.root), &handle->list, &handle->sb);

  snapshot->count = handle->list.count;
  snapshot->objects = (SnapshotObj*)handle->list.list;
  snapshot->gcmem = sbufB(&handle->sb);
  snapshot->gcmem_size = sbuflen(&handle->sb);

  snapshot->handle = handle;

  return snapshot;
}

LUA_API void gcsnapshot_free(GCSnapshot* snapshot)
{
  GCSnapshotHandle* handle = snapshot->handle;
  global_State* g = G(handle->L);

  lj_buf_free(g, &handle->sb);
  lj_mem_freevec(g, handle->list.list, handle->list.capacity, SnapshotObj);

  lj_mem_free(g, handle, sizeof(GCSnapshotHandle) + sizeof(GCSnapshot));
}

/* designed to support light weight snapshots that don't have the objects raw memory */
LUA_API size_t gcsnapshot_getgcstats(GCSnapshot* snap, GCStats* gcstats)
{
  SnapshotObj* objects = snap->objects;
  GCObjStat* stats = gcstats->objstats;

  for (MSize i = 0; i < snap->count; i++) {
    size_t size = objects[i].typeandsize >> 4;
    gcobj_type type = (gcobj_type)(objects[i].typeandsize & 15);

    stats[type].count++;
    stats[type].totalsize += size;
    stats[type].maxsize = stats[type].maxsize > size ? stats[type].maxsize : size;
  }

  return snap->count;
}

static uint32_t dump_strings(lua_State *L, LJList* list, SBuf* buf);

static int dump_gcobjects(lua_State *L, GCobj *liststart, LJList* list, SBuf* buf)
{
  GCobj *o = liststart;
  SnapshotObj* entry;

  if (liststart == NULL) {
    return 0;
  }

  while (o != NULL) {
    int gct = o->gch.gct;
    size_t size = gcobj_size(o);

    entry = lj_list_current(L, *list, SnapshotObj);

    if (size >= (1 << 28)) {
      //TODO: Overflow side list of sizes
      size = (1 << 28) - 1;
    }

    entry->typeandsize = ((uint32_t)size << 4) | typeconverter[gct];
    entry->address = (uint32_t)(uintptr_t)o;
    lj_list_increment(L, *list, SnapshotObj);

    if (gct != ~LJ_TTAB && gct != ~LJ_TTHREAD) {
      lj_buf_putmem(buf, o, (MSize)size);
    } else if (gct == ~LJ_TTAB) {
      lj_buf_putmem(buf, o, sizeof(GCtab));

      if (o->tab.asize != 0) {
        lj_buf_putmem(buf, tvref(o->tab.array), o->tab.asize*sizeof(TValue));
      }

      if (o->tab.hmask != 0) {
        lj_buf_putmem(buf, noderef(o->tab.node), (o->tab.hmask+1)*sizeof(Node));
      }

    } else if (gct == ~LJ_TTHREAD) {
      lj_buf_putmem(buf, o, sizeof(lua_State));
      lj_buf_putmem(buf, tvref(o->th.stack), o->th.stacksize*sizeof(TValue));
    }

    o = gcref(o->gch.nextgc);
  }

  dump_strings(L, list, buf);

  lj_buf_putmem(buf, L2GG(L), (MSize)sizeof(GG_State));
  return list->count;
}

static uint32_t dump_strings(lua_State *L, LJList* list, SBuf* buf)
{
  global_State *g = G(L);
  SnapshotObj* entry;
  uint32_t count = 0;

  for (MSize i = 0; i <= g->strmask; i++) {
    /* walk all the string hash chains. */
    GCobj *o = gcref(g->strhash[i]);

    while (o != NULL) {
      size_t size = sizestring(&o->str);

      if (size >= (1 << 28)) {
        //TODO: Overflow side list of sizes
        size = (1 << 28) - 1;
      }

      lj_buf_putmem(buf, o, (MSize)size);

      entry = lj_list_current(L, *list, SnapshotObj);
      entry->typeandsize = ((uint32_t)size << 4) | typeconverter[~LJ_TSTR];
      entry->address = (uint32_t)(uintptr_t)o;
      lj_list_increment(L, *list, SnapshotObj);

      o = gcref(o->gch.nextgc);
      count++;
    }
  }

  return count;
}

LUA_API int gcsnapshot_validate(GCSnapshot* snapshot)
{
  return validatedump(snapshot->count, snapshot->objects, snapshot->gcmem, snapshot->gcmem_size);
}

int validatedump(int count, SnapshotObj* objects, char* objectmem, size_t mem_size) 
{
  char* position = objectmem;
  size_t size;
  GCobj* o;
  gcobj_type type;

  for (int i = 0; i < count; i++) {
    o = (GCobj*)position;
    size = objects[i].typeandsize >> 4;
    type = (gcobj_type)(objects[i].typeandsize & 15);

    //Check the type we have in the pointer array matchs the ones in the header of object we think our current position is meant tobe pointing at
    if (o->gch.gct != ((~LJ_TSTR)+ type)) {
      return -i;
    }

    position += size;
    if ((size_t)(position - objectmem) > mem_size) {
      return -i;
    }
  }

  return 0;
}

size_t writeheader(FILE* f, const char* name, uint32_t size)
{
  ChunkHeader header = { 0 };
  memcpy(&header.id, name, 4);
  header.length = size;

  return fwrite(&header, sizeof(header), 1, f);
}

size_t writeint(FILE* f, int32_t i)
{
  return fwrite(&i, 4, 1, f);
}

LUA_API void gcsnapshot_save(GCSnapshot* snapshot, FILE* f)
{

  //Save object array
  writeheader(f, "GCOB", (snapshot->count*sizeof(SnapshotObj))+4);
  writeint(f, snapshot->count);
  fwrite(snapshot->objects, sizeof(SnapshotObj)*snapshot->count, 1, f);

  //Save objects memory
  writeheader(f, "OMEM", (uint32_t)snapshot->gcmem_size);
  if (snapshot->gcmem_size != 0) {
    fwrite(snapshot->gcmem, snapshot->gcmem_size, 1, f);
  }
}

LUA_API void gcsnapshot_savetofile(GCSnapshot* snapshot, const char* path)
{
  FILE* f = fopen(path, "wb");
  gcsnapshot_save(snapshot, f);
  fclose(f);
}

void tablestats(GCtab* t, GCStatsTable* result)
{
  TValue* array = tvref(t->array);
  Node *node = noderef(t->node);
  uint32_t arrayCount = 0, hashcount = 0, hashcollsision = 0;

  for (size_t i = 0; i < t->asize; i++) {
    if (!tvisnil(array + i)) {
      arrayCount++;
    }
  }

  for (uint32_t i = 0; i < t->hmask+1; i++) {
    if (!tvisnil(&node[i].val)) {
      hashcount++;
    }

    if (noderef(node[i].next) != NULL) {
      hashcollsision++;
    }
  }

  result->arraycapacity = t->asize;
  result->arraysize = arrayCount;

  result->hashsize = hashcount;
  result->hashcapacity = t->hmask+1;
  result->hashcollisions = hashcollsision;
}

size_t gcobj_size(GCobj *o)
{
  int size = 0;

  switch (o->gch.gct)
  {
    case ~LJ_TSTR:
      return sizestring(&o->str);

    case ~LJ_TTAB: {
      GCtab* t = &o->tab;
      size = sizeof(GCtab) + sizeof(TValue) * t->asize;
      if(t->hmask != 0)size += sizeof(Node) * (t->hmask + 1);

      return size;
    }

    case ~LJ_TUDATA:
      return sizeudata(&o->ud);

    case ~LJ_TCDATA:
      size = sizeof(GCcdata);

      //TODO: lookup the size in the cstate and maybe cache the result into an array the same size as the cstate id table
      //Use an array of bytes for size and if size > 255 lookup id in hashtable instead
      if (cdataisv(&o->cd))
      {
        size += sizecdatav(&o->cd);
      }
      return size;

    case ~LJ_TFUNC: {
      GCfunc *fn = gco2func(o);
      return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) : sizeCfunc((MSize)fn->c.nupvalues);
    }

    case ~LJ_TPROTO:
      return gco2pt(o)->sizept;

    case ~LJ_TTHREAD:
      return sizeof(lua_State) + sizeof(TValue) * gco2th(o)->stacksize;

    case ~LJ_TTRACE: {
      GCtrace *T = gco2trace(o);
      return ((sizeof(GCtrace) + 7)&~7) + (T->nins - T->nk)*sizeof(IRIns) + T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
    }

    default:
      return 0;
  }
}
