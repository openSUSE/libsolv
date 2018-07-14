int verify_checksum(int fd, const char *file, const unsigned char *chksum, Id chksumtype);

FILE *curlfopen(struct repoinfo *cinfo, const char *file, int uncompress, const unsigned char *chksum, Id chksumtype, int markincomplete);

FILE *downloadpackage(Solvable *s, const char *loc);
int downloadchecksig(struct repoinfo *cinfo, FILE *fp, const char *sigurl, Pool **sigpool);

