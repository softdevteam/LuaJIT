#ifndef _LJ_USRBUF_H
#define _LJ_USRBUF_H

#include "lua.h"

typedef enum UBufAction {
  UBUF_CLOSE,
  UBUF_INIT,
  UBUF_FLUSH,
  UBUF_GROW_OR_FLUSH,
  UBUF_RESET,
} UBufAction;

struct UserBuf;
typedef struct UserBuf UserBuf;

typedef int (*UBufHandler)(UserBuf *buff, UBufAction action, void *arg);

typedef struct UserBuf {
  /* These could point to a plain buffer or a mem mapped file */
  char *p;
  char *e;
  char *b;
  void *state;
  UBufHandler bufhandler;
  lua_State *L;
  int userflags;
} UserBuf;

typedef struct UBufInitArgs {
  const char *path;
  int minbufspace;
} UBufInitArgs;

#define ubufsz(ub) ((size_t)((ub)->e - (ub)->b))
#define ubuflen(ub) ((size_t)((ub)->p - (ub)->b))
#define ubufleft(ub) ((size_t)((ub)->e - (ub)->p))
#define setubufP(ub, q)	((ub)->p = (q))

#define ubufB(ub) ((ub)->b)
#define ubufP(ub) ((ub)->p)
#define ubufL(ub) ((ub)->L)

char *ubuf_more2(UserBuf *ub, size_t sz);
char *ubuf_need2(UserBuf *ub, size_t sz);

#ifndef LJ_AINLINE
#define LJ_AINLINE inline
#endif

static LJ_AINLINE char *ubuf_need(UserBuf *ub, size_t sz)
{
  if (LJ_UNLIKELY(sz > ubufsz(ub)))
    return ubuf_need2(ub, sz);
  return ubufB(ub);
}

static LJ_AINLINE char *ubuf_more(UserBuf *ub, size_t sz)
{
  if (LJ_UNLIKELY(sz > ubufleft(ub)))
    return ubuf_more2(ub, sz);
  return ubufP(ub);
}

static LJ_AINLINE UserBuf *ubuf_putmem(UserBuf *ub, const void *q, size_t len)
{
  char *p = ubuf_more(ub, len);
  p = (char *)memcpy(p, q, len) + len;
  setubufP(ub, p);
  return ub;
}

static inline int ubuf_reset(UserBuf *ub)
{
  lua_assert(ubufB(ub));
  return ub->bufhandler(ub, UBUF_RESET, NULL);
}

static inline int ubuf_flush(UserBuf *ub)
{
  lua_assert(ubufB(ub));
  return ub->bufhandler(ub, UBUF_FLUSH, NULL);
}

static inline int ubuf_free(UserBuf *ub)
{
  if (!ub->bufhandler) {
    return 1;
  }
  return ub->bufhandler(ub, UBUF_CLOSE, NULL);
}

int membuf_doaction(UserBuf *ub, UBufAction action, void *arg);
int filebuf_doaction(UserBuf *ub, UBufAction action, void *arg);
int mmapbuf_doaction(UserBuf *ub, UBufAction action, void *arg);

static inline int ubuf_init_mem(UserBuf *ub, int minbufspace)
{
  UBufInitArgs args = {0};
  args.minbufspace = minbufspace;
  ub->bufhandler = membuf_doaction;
  return membuf_doaction(ub, UBUF_INIT, &args);
}

static inline int ubuf_init_file(UserBuf *ub, const char* path)
{
  UBufInitArgs args = {0};
  ub->bufhandler = filebuf_doaction;
  args.path = (void *)path;
  return filebuf_doaction(ub, UBUF_INIT, &args);
}

static inline int ubuf_init_mmap(UserBuf *ub, const char* path, int windowsize)
{
  UBufInitArgs args = {0};
  lua_assert(path);
  ub->bufhandler = mmapbuf_doaction;
  args.minbufspace = windowsize;
  args.path = (void *)path;
  return mmapbuf_doaction(ub, UBUF_INIT, &args);
}

#endif
