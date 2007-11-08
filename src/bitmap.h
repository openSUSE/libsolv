/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * bitmap.h
 * 
 */

#ifndef BITMAP_H
#define BITMAP_H

typedef struct _Map {
  unsigned char *map;
  int size;
} Map;

#define MAPZERO(m) (memset((m)->map, 0, (m)->size))
#define MAPSET(m, n) ((m)->map[(n) >> 3] |= 1 << ((n) & 7))
#define MAPCLR(m, n) ((m)->map[(n) >> 3] &= ~(1 << ((n) & 7)))
#define MAPTST(m, n) ((m)->map[(n) >> 3] & (1 << ((n) & 7)))

extern void map_init(Map *m, int n);
extern void map_free(Map *m);
extern void map_clone(Map *t, Map *s);

#endif /* BITMAP_H */
