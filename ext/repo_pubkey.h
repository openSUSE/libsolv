/*
 * Copyright (c) 2013, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include "repo.h"

extern int repo_add_rpmdb_pubkeys(Repo *repo, int flags);
extern Id repo_add_pubkey(Repo *repo, const char *key, int flags);

/* signature parsing */
typedef struct _solvsig {
  unsigned char *sigpkt;
  int sigpktl;
  Id htype;
  unsigned int created;
  unsigned int expires;
  char keyid[17];
} Solvsig;

Solvsig *solvsig_create(FILE *fp);
void solvsig_free(Solvsig *ss);
Id solvsig_verify(Solvsig *ss, Repo *repo, void *chk);

/* raw signature verification */
int solv_verify_sig(const unsigned char *pubdata, int pubdatal, unsigned char *sigpkt, int sigpktl, void *chk);
Id repo_verify_sigdata(Repo *repo, unsigned char *sigdata, int sigdatal, const char *keyid);


