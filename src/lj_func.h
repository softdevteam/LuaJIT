/*
** Function handling (prototypes, functions and upvalues).
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_FUNC_H
#define _LJ_FUNC_H

#include "lj_obj.h"

/* Upvalues. */
LJ_FUNCA void LJ_FASTCALL lj_func_closeuv(lua_State *L, TValue *level);

/* Functions (closures). */
LJ_FUNC GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env);
LJ_FUNC GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env);
LJ_FUNCA GCfunc *lj_func_newL_gc(lua_State *L, uintptr_t pt_, GCfuncL *parent);

#endif
