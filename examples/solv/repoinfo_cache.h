
struct repoinfo;
struct stat;

extern void set_userhome(void);
extern char *calc_cachepath(Repo *repo, const char *repoext, int forcesystemloc);
extern void calc_cookie_fp(FILE *fp, Id chktype, unsigned char *out);
extern void calc_cookie_stat(struct stat *stb, Id chktype, unsigned char *cookie, unsigned char *out);

extern int usecachedrepo(struct repoinfo *cinfo, const char *repoext, int mark);
extern void  writecachedrepo(struct repoinfo *cinfo, const char *repoext, Repodata *repodata);

