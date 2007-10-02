/*
 * queue.c
 *
 */

#include <stdlib.h>
#include <string.h>

#include "queue.h"

void
clonequeue(Queue *t, Queue *s)
{
  t->alloc = t->elements = malloc((s->count + 8) * sizeof(Id));
  if (s->count)
    memcpy(t->alloc, s->elements, s->count * sizeof(Id));
  t->count = s->count;
  t->left = 8;
}

void
queueinit(Queue *q)
{
  q->alloc = q->elements = 0;
  q->count = q->left = 0;
}

void
queueinit_buffer(Queue *q, Id *buf, int size)
{
  q->alloc = 0;
  q->elements = buf;
  q->count = 0;
  q->left = size;
}

void
queuefree(Queue *q)
{
  if (q->alloc)
    free(q->alloc);
  q->alloc = q->elements = 0;
  q->count = q->left = 0;
}

Id
queueshift(Queue *q)
{
  if (!q->count)
    return 0;
  q->count--;
  return *q->elements++;
}

void
queuepush(Queue *q, Id id)
{
  if (!q->left)
    {
      if (q->alloc && q->alloc != q->elements)
	{
	  memmove(q->alloc, q->elements, q->count * sizeof(Id));
	  q->left += q->elements - q->alloc;
	  q->elements = q->alloc;
	}
      else if (q->alloc)
	{
	  q->elements = q->alloc = realloc(q->alloc, (q->count + 8) * sizeof(Id));
	  q->left += 8;
	}
      else
	{
	  q->alloc = malloc((q->count + 8) * sizeof(Id));
	  if (q->count)
	    memcpy(q->alloc, q->elements, q->count * sizeof(Id));
	  q->elements = q->alloc;
	  q->left += 8;
	}
    }
  q->elements[q->count++] = id;
  q->left--;
}

void
queuepushunique(Queue *q, Id id)
{
  int i;
  for (i = q->count; i > 0; )
    if (q->elements[--i] == id)
      return;
  queuepush(q, id);
}


