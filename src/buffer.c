/* impl.c.buffer: ALLOCATION BUFFER IMPLEMENTATION
 *
 * $HopeName: !buffer.c(trunk.8) $
 * Copyright (C) 1996 Harlequin Group, all rights reserved
 *
 * Since allocation buffers are exposed, most of the documentation
 * is in the interface (buffer.h).  This comment documents the
 * internal interface, between the pool and the buffer
 * implementation.
 * 
 * The pool must allocate the buffer descriptor and initialize it by
 * calling BufferInit.  The descriptor this creates will fall
 * through to the fill method on the first allocation.  In general,
 * pools should not assign resources to the buffer until the first
 * allocation, since the buffer may never be used.
 * 
 * The pool may update the base, init, alloc, and limit fields when
 * the fallback methods are called.  In addition, the pool may set
 * the limit to zero at any time.  The effect of this is either:
 * 
 *   1. cause the _next_ allocation in the buffer to fall through to
 *      the buffer fill method, and allow the buffer to be flushed
 *      and relocated;
 *
 *   2. cause the buffer trip method to be called if the client was
 *      between reserve and commit.
 *
 * A buffer may not be relocated under other circumstances because
 * there is a race between updating the descriptor and the client
 * allocation sequence.
 */

#include "std.h"
#include "lib.h"
#include "buffer.h"
#include "pool.h"
#include "space.h"
#include "shield.h"
#include "trace.h"

SRCID("$HopeName: !buffer.c(trunk.8) $");


DequeNode BufferPoolDeque(Buffer buffer)
{
  AVER(ISVALID(Buffer, buffer));
  return &buffer->poolDeque;
}

Pool BufferPool(Buffer buffer)
{
  Deque deque;
  Pool pool;

  AVER(ISVALID(Buffer, buffer));

  deque = DequeNodeParent(BufferPoolDeque(buffer));
  pool = PARENT(PoolStruct, bufferDeque, deque);
  
  return pool;
}

Ap BufferAp(Buffer buffer)
{
  AVER(ISVALID(Buffer, buffer));
  return &buffer->ap;
}

/* This method must be thread-safe.  See impl.c.mpsi.thread-safety. */
Buffer BufferOfAp(Ap ap)
{
  return PARENT(BufferStruct, ap, ap);
}

/* This method must be thread-safe.  See impl.c.mpsi.thread-safety. */
Space BufferSpace(Buffer buffer)
{
  return buffer->space;
}


Error BufferCreate(Buffer *bufferReturn, Pool pool)
{
  AVER(bufferReturn != NULL);
  AVER(ISVALID(Pool, pool));
  AVER(pool->class->bufferCreate != NULL);
  
  return (*pool->class->bufferCreate)(bufferReturn, pool);
}

void BufferDestroy(Buffer buffer)
{
  Pool pool;

  AVER(ISVALID(Buffer, buffer));

  pool = BufferPool(buffer);

  AVER(pool->class->bufferDestroy != NULL);
  AVER(buffer->ap.init == buffer->ap.alloc);

  (*pool->class->bufferDestroy)(pool, buffer);
}


Bool BufferIsValid(Buffer buffer, ValidationType validParam)
{
  AVER(buffer != NULL);
  AVER(buffer->sig == BufferSig);
  AVER(ISVALIDNESTED(DequeNode, &buffer->poolDeque));
  AVER(buffer->base <= buffer->ap.init);
  AVER(buffer->ap.init <= buffer->ap.alloc);
  AVER(buffer->ap.alloc <= buffer->ap.limit || buffer->ap.limit == 0);
  /* buffer->space */
  /* AVER(buffer->alignment == BufferPool(buffer)->alignment); */
  AVER(IsPoT(buffer->alignment));
  AVER(IsAligned(buffer->alignment, buffer->base));
  AVER(IsAligned(buffer->alignment, buffer->ap.init));
  AVER(IsAligned(buffer->alignment, buffer->ap.alloc));
  AVER(IsAligned(buffer->alignment, buffer->ap.limit));
  return TRUE;
}


void BufferSet(Buffer buffer, Addr base, Addr init, Addr limit)
{
  AVER(ISVALID(Buffer, buffer));
  
  buffer->base = base;
  buffer->ap.init = init;
  buffer->ap.alloc = init;
  buffer->ap.limit = limit;
}


void BufferReset(Buffer buffer)
{
  AVER(ISVALID(Buffer, buffer));
  
  buffer->base = 0;
  buffer->ap.init = 0;
  buffer->ap.alloc = 0;
  buffer->ap.limit = 0;
}


Bool BufferIsReset(Buffer buffer)
{
  AVER(ISVALID(Buffer, buffer));
  
  if(buffer->base == 0 &&
     buffer->ap.init == 0 &&
     buffer->ap.alloc == 0 &&
     buffer->ap.limit == 0)
    return TRUE;
  
  return FALSE;
}


Bool BufferIsReady(Buffer buffer)
{
  AVER(ISVALID(Buffer, buffer));
  
  if(buffer->ap.init == buffer->ap.alloc)
    return TRUE;
  
  return FALSE;
}


void BufferInit(Buffer buffer, Pool pool)
{
  AVER(ISVALID(Pool, pool));
  AVER(buffer != NULL);

  buffer->base = 0;
  buffer->ap.init = 0;
  buffer->ap.alloc = 0;
  buffer->ap.limit = 0;
  buffer->space = PoolSpace(pool);
  buffer->alignment = pool->alignment;
  buffer->exposed = FALSE;
  buffer->p = NULL;
  buffer->i = 0;
  buffer->shieldMode = ProtNONE;
  buffer->grey = TraceSetEmpty;

  DequeNodeInit(&buffer->poolDeque);
  DequeAppend(&pool->bufferDeque, &buffer->poolDeque);

  buffer->sig = BufferSig;

  AVER(ISVALID(Buffer, buffer));
}


void BufferFinish(Buffer buffer)
{
  AVER(ISVALID(Buffer, buffer));
  AVER(BufferIsReset(buffer));
  AVER(buffer->exposed == FALSE);

  DequeNodeRemove(&buffer->poolDeque);

  buffer->sig = SigInvalid;
}


/* This is the reserve sequence that the mutator must go through to */
/* reserve space for a proto-object. */

Error BufferReserve(Addr *pReturn, Buffer buffer, Addr size)
{
  Addr next;

  AVER(pReturn != NULL);
  AVER(ISVALID(Buffer, buffer));
  AVER(size > 0);
  AVER(IsAligned(BufferPool(buffer)->alignment, size));
  AVER(BufferIsReady(buffer));

  /* Is there enough room in the unallocated portion of the buffer to */
  /* satisfy the request?  If so, just increase the alloc marker and */
  /* return a pointer to the area below it. */

  next = buffer->ap.alloc + size;
  if(next > buffer->ap.alloc && next <= buffer->ap.limit)
  {
    buffer->ap.alloc = next;
    *pReturn = buffer->ap.init;
    return ErrSUCCESS;
  }
  
  /* If the buffer can't accommodate the request, fall through to the */
  /* pool-specific allocation method. */

  return BufferFill(pReturn, buffer, size);
}


Error BufferFill(Addr *pReturn, Buffer buffer, Addr size)
{
  Error e;
  Pool pool;

  AVER(pReturn != NULL);
  AVER(ISVALID(Buffer, buffer));
  AVER(size > 0);
  AVER(IsAligned(BufferPool(buffer)->alignment, size));
  AVER(BufferIsReady(buffer));

  pool = BufferPool(buffer);
  e = (*pool->class->bufferFill)(pReturn, pool, buffer, size);
  
  AVER(ISVALID(Buffer, buffer));

  return e;
}


/* After initializing the proto-object, the mutator calls commit to */
/* tell the pool that it is valid.  Commit may return FALSE to */
/* indicate that the client must go back and reserve again. */

Bool BufferCommit(Buffer buffer, Addr p, Addr size)
{
  AVER(ISVALID(Buffer, buffer));
  AVER(size > 0);
  AVER(IsAligned(BufferPool(buffer)->alignment, size));
  
  /* If a flip occurs before this point, the pool will see init */
  /* below the object, so it will be trashed and the commit */
  /* must fail when trip is called.  The pool will also see */
  /* a pointer p which points to the invalid object at init. */

  AVER(p == buffer->ap.init);
  AVER(buffer->ap.init + size == buffer->ap.alloc);
    
  /* Atomically update the init pointer to declare that the object */
  /* is initialized (though it may be invalid if a flip occurred). */

  buffer->ap.init = buffer->ap.alloc;
  
  /* **** Memory barrier here on the DEC Alpha. */

  /* If a flip occurs at this point, the pool will see init */
  /* above the object, which is valid, so it will be collected */
  /* the commit must succeed when trip is called.  The pointer */
  /* p will have been fixed up. */

  /* trip the buffer if a flip has occurred. */

  if(buffer->ap.limit == 0)
    return BufferTrip(buffer, p, size);

  /* No flip occurred, so succeed. */

  return TRUE;
}


Bool BufferTrip(Buffer buffer, Addr p, Addr size)
{
  Pool pool;

  AVER(ISVALID(Buffer, buffer));
  AVER(size > 0);
  AVER(IsAligned(BufferPool(buffer)->alignment, size));
  
  pool = BufferPool(buffer);
  return (*pool->class->bufferTrip)(pool, buffer, p, size);
}


void BufferExpose(Buffer buffer)
{
  Pool pool;

  AVER(ISVALID(Buffer, buffer));
  AVER(BufferPool(buffer)->class->bufferExpose != NULL);

  buffer->exposed = TRUE;
  pool = BufferPool(buffer);
  (*pool->class->bufferExpose)(pool, buffer);
}

void BufferCover(Buffer buffer)
{
  Pool pool;

  AVER(ISVALID(Buffer, buffer));
  AVER(BufferPool(buffer)->class->bufferCover != NULL);

  buffer->exposed = FALSE;
  pool = BufferPool(buffer);
  (*pool->class->bufferCover)(pool, buffer);
}


Error BufferDescribe(Buffer buffer, LibStream stream)
{
  AVER(ISVALID(Buffer, buffer));
  AVER(stream != NULL);

  LibFormat(stream,
            "Buffer %p {\n"
            "  Pool %p\n"
            "  alignment %lu\n"
            "  base 0x%lX  init 0x%lX  alloc 0x%lX  limit 0x%lX\n"
            "  grey 0x%lX  shieldMode %lu"
            "} Buffer %p\n",
            (void *)buffer,
            (void *)BufferPool(buffer),
            (unsigned long)buffer->alignment,
            (unsigned long)buffer->base,
            (unsigned long)buffer->ap.init,
            (unsigned long)buffer->ap.alloc,
            (unsigned long)buffer->ap.limit,
            (unsigned long)buffer->grey,
            (unsigned long)buffer->shieldMode,
            (void *)buffer);

  return ErrSUCCESS;
}
