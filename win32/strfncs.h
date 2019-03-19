#include <string.h>

int strcasecmp(const char *_l, const char *_r);
int strncasecmp(const char *_l, const char *_r, size_t n);

char *strcasestr(const char *h, const char *n);

char *strtok_r(char *s, const char *sep, char **p);
