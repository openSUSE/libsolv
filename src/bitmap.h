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

#ifndef SATSOLVER_BITMAP_H
#define SATSOLVER_BITMAP_H

typedef struct _Map {
  unsigned char *map;
  int size;
} Map;

#define MAPZERO(m) (memset((m)->map, 0, (m)->size))
#define MAPSET(m, n) ((m)->map[(n) >> 3] |= 1 << ((n) & 7)) // Set Bit
#define MAPCLR(m, n) ((m)->map[(n) >> 3] &= ~(1 << ((n) & 7))) // Reset Bit
#define MAPTST(m, n) ((m)->map[(n) >> 3] & (1 << ((n) & 7))) // Test Bit

static inline void
map_empty(Map *m)
{
  MAPZERO(m);
}

extern void map_init(Map *m, int n);
extern void map_init_clone(Map *t, Map *s);
extern void map_free(Map *m);

#endif /* SATSOLVER_BITMAP_H */
