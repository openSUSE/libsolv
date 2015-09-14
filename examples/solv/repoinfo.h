struct repoinfo {
  Repo *repo;

  int type;
  char *alias;
  char *name;
  int enabled;
  int autorefresh;
  char *baseurl;
  char *metalink;
  char *mirrorlist;
  char *path;
  int pkgs_gpgcheck;
  int repo_gpgcheck;
  int priority;
  int keeppackages;
  int metadata_expire;
  char **components;
  int ncomponents;
  int cookieset;
  unsigned char cookie[32];
  int extcookieset;
  unsigned char extcookie[32];
  int incomplete;
};

#define TYPE_UNKNOWN    0
#define TYPE_SUSETAGS   1
#define TYPE_RPMMD      2
#define TYPE_PLAINDIR   3
#define TYPE_DEBIAN     4
#define TYPE_MDK        5

#define TYPE_INSTALLED  16
#define TYPE_CMDLINE    17

#define METADATA_EXPIRE (60 * 15)

extern void sort_repoinfos(struct repoinfo *repoinfos, int nrepoinfos);
extern void free_repoinfos(struct repoinfo *repoinfos, int nrepoinfos);
extern void read_repos(Pool *pool, struct repoinfo *repoinfos, int nrepoinfos);
extern struct repoinfo *read_repoinfos(Pool *pool, int *nrepoinfosp);

extern int read_installed_repo(struct repoinfo *cinfo, Pool *pool);

extern int is_cmdline_package(const char *filename);
extern Id add_cmdline_package(Repo *repo, const char *filename);

extern void commit_transactionelement(Pool *pool, Id type, Id p, FILE *fp);

extern void add_ext_keys(Repodata *data, Id handle, const char *ext);
extern int load_stub(Pool *pool, Repodata *data, void *dp);

