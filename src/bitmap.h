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

#if 0
#define MAPINIT(m, n) ((m)->size = ((n) + 7) >> 3, (m)->map = calloc((m)->size, 1))
#endif

#define MAPZERO(m) (memset((m)->map, 0, (m)->size))
#define MAPSET(m, n) ((m)->map[(n) >> 3] |= 1 << ((n) & 7))
#define MAPCLR(m, n) ((m)->map[(n) >> 3] &= ~(1 << ((n) & 7)))
#define MAPTST(m, n) ((m)->map[(n) >> 3] & (1 << ((n) & 7)))

extern void mapinit(Map *m, int n);
extern void mapfree(Map *m);
extern void clonemap(Map *t, Map *s);

#endif /* BITMAP_H */
