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

#ifndef QUEUE_H
#define QUEUE_H

#include "pooltypes.h"

typedef struct _Queue {
  Id *elements;		// current elements
  int count;		// current number of elements (minimal size for elements pointer)
  Id *alloc;		// this is whats actually allocated, elements > alloc if shifted
  int left;		// space left in alloc *after* elements+count
} Queue;


extern void queue_alloc_one(Queue *q);

// clear queue
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

extern void queue_clone(Queue *t, Queue *s);
extern void queue_init(Queue *q);
extern void queue_init_buffer(Queue *q, Id *buf, int size);
extern void queue_free(Queue *q);

#endif /* QUEUE_H */
