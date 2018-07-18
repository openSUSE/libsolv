/*
 * Copyright (c) 2018, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

struct solv_zchunk;

extern struct solv_zchunk *solv_zchunk_open(FILE *fp, unsigned int streamid);
extern ssize_t solv_zchunk_read(struct solv_zchunk *zck, char *buf, size_t len);
extern int solv_zchunk_close(struct solv_zchunk *zck);

