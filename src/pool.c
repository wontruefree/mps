/*  ==== POOLS ====
 *
 *  $HopeName: MMsrc/!pool.c(MMdevel_dsm_0.1)$
 *
 *  Copyright (C) 1994,1995 Harlequin Group, all rights reserved
 *
 *  This is the implementation of the generic pool interface.  The
 *  functions here dispatch to pool-specific methods.
 *
 *  Notes
 *   2. Some explanatory comments would be nice.  richard 1994-08-25
 */

#include "std.h"
#include "lib.h"
#include "error.h"
#include "pool.h"
#include "poolst.h"
#include "space.h"
#include "ref.h"
#include "trace.h"
#include <stddef.h>
#include <stdarg.h>


#ifdef DEBUG_SIGN
static SigStruct PoolSigStruct;
#endif


#ifdef DEBUG_ASSERT

Bool PoolIsValid(Pool pool, ValidationType validParam)
{
  AVER(pool != NULL);
#ifdef DEBUG_SIGN
  AVER(ISVALIDNESTED(Sig, &PoolSigStruct));
  AVER(pool->sig == &PoolSigStruct);
#endif
  AVER(ISVALIDNESTED(DequeNode, &pool->spaceDeque));
  AVER(ISVALIDNESTED(Deque, &pool->segDeque));
  AVER(ISVALIDNESTED(Deque, &pool->bufferDeque));
  AVER(IsPoT(pool->alignment));
  return TRUE;
}

#endif /* DEBUG_ASSERT */


void PoolInit(Pool pool, Space space, PoolClass class)
{
  AVER(pool != NULL);
  AVER(ISVALID(Space, space));

  pool->class = class;
  DequeNodeInit(&pool->spaceDeque);
  DequeInit(&pool->segDeque);
  DequeInit(&pool->bufferDeque);
  pool->alignment = ARCH_ALIGNMOD;

#ifdef DEBUG_SIGN
  SigInit(&PoolSigStruct, "Pool");
  pool->sig = &PoolSigStruct;
#endif

  AVER(ISVALID(Pool, pool));

  DequeAppend(SpacePoolDeque(space), &pool->spaceDeque);
}


void PoolFinish(Pool pool)
{
  AVER(ISVALID(Pool, pool));

  DequeNodeRemove(&pool->spaceDeque);
  DequeNodeFinish(&pool->spaceDeque);

  DequeFinish(&pool->bufferDeque);
  DequeFinish(&pool->segDeque);

#ifdef DEBUG_SIGN
  pool->sig = SigInvalid;
#endif
}
  

Error PoolCreate(Pool *poolReturn, PoolClass class, Space space, ...)
{
  Error e;
  va_list arg;
  va_start(arg, space);
  e = PoolCreateV(poolReturn, class, space, arg);
  va_end(arg);
  return e;
}

Error PoolCreateV(Pool *poolReturn, PoolClass class,
                  Space space, va_list arg)
{
  Error e;

  SpaceLockClaim(space);
  AVER(poolReturn != NULL);
  AVER(ISVALID(Space, space));
  e = (*class->create)(poolReturn, space, arg);
  SpaceLockRelease(space);
  return e;
}

void PoolDestroy(Pool pool)
{
  Space space;

  space = PoolSpace(pool);
  SpaceLockClaim(space);

  AVER(ISVALID(Pool, pool));
  (*pool->class->destroy)(pool);

  SpaceLockRelease(space);
}


Error (PoolAllocP)(void **pReturn, Pool pool, size_t size)
{
  Error e;
  Space space;

  space = PoolSpace(pool);
  SpaceLockClaim(space);

  AVER(pReturn != NULL);
  AVER(ISVALID(Pool, pool));
  AVER(size > 0);

  e = (*pool->class->allocP)(pReturn, pool, size);
  if(e != ErrSUCCESS)
    goto return_e;

  /* Make sure that the allocated address was in the pool's memory. */  
  AVER(PoolHasAddr(pool, (Addr)*pReturn));

  e = ErrSUCCESS;
return_e:
  SpaceLockRelease(space);
  return e;
}


void PoolFreeP(Pool pool, void *old, size_t size)
{
  Space space;

  space = PoolSpace(pool);
  SpaceLockClaim(space);
  AVER(ISVALID(Pool, pool));
  AVER(old != NULL);
  AVER(PoolHasAddr(pool, (Addr)old));

  if(pool->class->freeP != NULL)
    (*pool->class->freeP)(pool, old, size);
  SpaceLockRelease(space);
}


Error PoolCondemn(Pool pool, Trace trace)
{
  AVER(pool->class->condemn != NULL);
  return (*pool->class->condemn)(pool, trace);
}


void PoolMark(Pool pool, Trace trace)
{
  if(pool->class->mark != NULL)
    (*pool->class->mark)(pool, trace);
}


Error PoolScan(Pool pool, Trace trace, RefRank rank)
{
  if(pool->class->scan != NULL)
    return (*pool->class->scan)(pool, trace, rank);
  return ErrSUCCESS;
}

Error PoolFix(Pool pool, Trace trace, RefRank rank, Arena arena, Addr *refIO)
{
  if(pool->class->fix != NULL)
    return (*pool->class->fix)(pool, trace, rank, arena, refIO);
  return ErrSUCCESS;
}

void PoolReclaim(Pool pool, Trace trace)
{
  AVER(pool->class->reclaim != NULL);
  (*pool->class->reclaim)(pool, trace);
}


Error PoolDescribe(Pool pool, LibStream stream)
{
  Space space;

  space = PoolSpace(pool);
  SpaceLockClaim(space);

  AVER(ISVALID(Pool, pool));
  AVER(stream != NULL);

  LibFormat(stream,
	  "Pool %p {\n"
	  "  Class %s\n"
	  "  alignment %lu\n",
	  pool,
	  pool->class->name,
	  (unsigned long)pool->alignment);

  if(DequeLength(&pool->bufferDeque) > 0)
  {
    DequeNode node = DequeFirst(&pool->bufferDeque);
    
    LibFormat(stream, "  Buffers\n");
    
    while(node != DequeSentinel(&pool->bufferDeque))
    {
      node = DequeNodeNext(node);
    }
  }

  if(pool->class->describe == NULL)
    LibFormat(stream, "  No class-specific description available.\n");
  else
    (void)(*pool->class->describe)(pool, stream);

  LibFormat(stream, "} Pool %p\n", pool);

  SpaceLockRelease(space);
  return ErrSUCCESS;
}


/*@@@@@*/
Space (PoolSpace)(Pool pool)
{
  return PARENT(SpaceStruct, poolDeque,
                DequeNodeParent(&pool->spaceDeque));
}

PoolClass (PoolGetClass)(Pool pool)
{
  AVER(ISVALID(Pool, pool));
  return pool->class;
}


Error PoolSegAlloc(Addr *segReturn, Pool pool, Addr size)
{
  Error e;
  Arena arena;
  Pool arpool;
  Addr seg;

  AVER(segReturn != NULL);
  AVER(ISVALID(Pool, pool));
  arena = SpaceArena(PoolSpace(pool));
  AVER(IsAligned(ArenaGrain(arena), size));

  arpool = PoolArenaPool(arena);
  e = PoolAllocP((void **)&seg, arpool, size);
  if(e != ErrSUCCESS)
    return(e);

  ArenaPut(arena, seg, ARENA_POOL, (void *)pool);

  *segReturn = seg;
  return(ErrSUCCESS);
}


void PoolSegFree(Pool pool, Addr seg, Addr size)
{
  Arena arena;
  Pool arpool;

  AVER(ISVALID(Pool, pool));

  arena = SpaceArena(PoolSpace(pool));
  arpool = PoolArenaPool(arena);

  ArenaPut(arena, seg, ARENA_POOL, (void *)arpool);

  PoolFreeP(arpool, (void *)seg, (size_t)size);
}


Pool PoolOfSeg(Arena arena, Addr seg)
{
  Pool pool;

  pool = (Pool)ArenaGet(arena, seg, ARENA_POOL);
  AVER(ISVALID(Pool, pool));

  return(pool);
}

Bool PoolOfAddr(Pool *poolReturn, Arena arena, Addr addr)
{
  Addr seg;
  
  AVER(poolReturn != NULL);
  AVER(ISVALID(Arena, arena));

  if(ArenaSegBase(&seg, arena, addr))
  {
    Pool pool = PoolOfSeg(arena, seg);
    *poolReturn = pool;
    return(TRUE);
  }
  
  return(FALSE);
}


Bool PoolHasAddr(Pool pool, Addr addr)
{
  Pool addrPool;
  Arena arena;

  AVER(ISVALID(Pool, pool));

  arena = SpaceArena(PoolSpace(pool));
  if(PoolOfAddr(&addrPool, arena, addr) && addrPool == pool)
    return(TRUE);
  else
    return(FALSE);
}

     
DequeNode (PoolSpaceDeque)(Pool pool)
{
  AVER(ISVALID(Pool, pool));

  return &pool->spaceDeque;
}


Deque (PoolBufferDeque)(Pool pool)
{
  AVER(ISVALID(Pool, pool));
  return &pool->bufferDeque;
}

Addr (PoolAlignment)(Pool pool)
{
  AVER(ISVALID(Pool, pool));
  return pool->alignment;
}
