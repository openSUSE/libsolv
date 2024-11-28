/*
 * Copyright (c) 2012, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

struct tarhead {
  FILE *fp;
  unsigned char blk[512];
  int type;
  long long length;
  char *path;
  int eof;
  int ispax;

  int off;
  int end;
};

extern void tarhead_init(struct tarhead *th, FILE *fp);
extern void tarhead_free(struct tarhead *th);
extern int tarhead_next(struct tarhead *th);
extern void tarhead_skip(struct tarhead *th);
extern size_t tarhead_gets(struct tarhead *th, char **line, size_t *allocp);
