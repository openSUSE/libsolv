/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * queue.h
 * 
 */

#ifndef SATSOLVER_QUEUE_H
#define SATSOLVER_QUEUE_H

#include "pooltypes.h"

typedef struct _Queue {
  Id *elements;		/* pointer to elements */
  int count;		/* current number of elements in queue */
  Id *alloc;		/* this is whats actually allocated, elements > alloc if shifted */
  int left;		/* space left in alloc *after* elements+count */
} Queue;


extern void queue_alloc_one(Queue *q);

/* clear queue */
static inline void
queue_empty(Queue *q)
{
  if (q->alloc)
    {
      q->left += (q->elements - q->alloc) + q->count;
      q->elements = q->alloc;
    }
  else
    q->left += q->count;
  q->count = 0;
}

static inline Id
queue_shift(Queue *q)
{
  if (!q->count)
    return 0;
  q->count--;
  return *q->elements++;
}

static inline Id
queue_pop(Queue *q)
{
  if (!q->count)
    return 0;
  q->left++;
  return q->elements[--q->count];
}

static inline void
queue_unshift(Queue *q, Id id)
{
  if (q->alloc && q->alloc != q->elements)
    {
      *--q->elements = id;
      q->count++;
      return;
    }
  if (!q->left)
    queue_alloc_one(q);
  if (q->count)
    memmove(q->elements + 1, q->elements, sizeof(Id) * q->count);
  q->count++;
  q->elements[0] = id;
  q->left--;
}

static inline void
queue_push(Queue *q, Id id)
{
  if (!q->left)
    queue_alloc_one(q);
  q->elements[q->count++] = id;
  q->left--;
}

static inline void
queue_pushunique(Queue *q, Id id)
{
  int i;
  for (i = q->count; i > 0; )
    if (q->elements[--i] == id)
      return;
  queue_push(q, id);
}

static inline void
queue_push2(Queue *q, Id id1, Id id2)
{
  queue_push(q, id1);
  queue_push(q, id2);
}

extern void queue_init(Queue *q);
extern void queue_init_buffer(Queue *q, Id *buf, int size);
extern void queue_init_clone(Queue *t, Queue *s);
extern void queue_free(Queue *q);

#endif /* SATSOLVER_QUEUE_H */
