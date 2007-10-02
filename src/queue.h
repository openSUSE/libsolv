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

// clear queue

#define QUEUEEMPTY(q) ((q)->alloc ? ((q)->left += ((q)->elements - (q)->alloc) + (q)->count, (q)->elements = (q)->alloc, (q)->count = 0) : ((q)->left += (q)->count, (q)->count = 0))

extern void clonequeue(Queue *t, Queue *s);
extern void queueinit(Queue *q);
extern void queueinit_buffer(Queue *q, Id *buf, int size);
extern void queuefree(Queue *q);
extern Id queueshift(Queue *q);
extern void queuepush(Queue *q, Id id);
extern void queuepushunique(Queue *q, Id id);

#endif /* QUEUE_H */
