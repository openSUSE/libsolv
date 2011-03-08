#ifndef SAT_XFOPEN_H
#define SAT_XFOPEN_H

extern FILE *sat_xfopen(const char *fn, const char *mode);
extern FILE *sat_xfopen_fd(const char *fn, int fd, const char *mode);

#endif
