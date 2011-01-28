%module solv

%typemap(in) (Id *idarray, int idarraylen) {
  /* Check if is a list */
  if (PyList_Check($input)) {
    int size = PyList_Size($input);
    int i = 0;
    $1 = (Id *)sat_malloc2(size, sizeof(Id));
    for (i = 0; i < size; i++) {
      PyObject *o = PyList_GetItem($input,i);
      int v;
      int e = SWIG_AsVal_int(o, &v);
      if (!SWIG_IsOK(e)) {
        SWIG_exception_fail(SWIG_ArgError(e), "list must contain integers");
        sat_free($1);
        return NULL;
      }
      $1[i] = v;
    }
    $2 = size;
  } else {
    PyErr_SetString(PyExc_TypeError,"not a list");
    return NULL;
  }
}
%typemap(freearg) (Id *idarray, int idarraylen) {
  sat_free($1);
}

%typemap(in) Queue {
  /* Check if is a list */
  queue_init(&$1);
  if (PyList_Check($input)) {
    int size = PyList_Size($input);
    int i = 0;
    for (i = 0; i < size; i++) {
      PyObject *o = PyList_GetItem($input,i);
      int v;
      int e = SWIG_AsVal_int(o, &v);
      if (!SWIG_IsOK(e)) {
        SWIG_exception_fail(SWIG_ArgError(e), "list must contain only integers");
        queue_free(&$1);
        return NULL;
      }
      queue_push(&$1, v);
    }
  } else {
    PyErr_SetString(PyExc_TypeError,"not a list");
    return NULL;
  }
}
%typemap(out) Queue {
  int i;
  PyObject *o = PyList_New($1.count);
  for (i = 0; i < $1.count; i++)
    PyList_SetItem(o, i, SWIG_From_int($1.elements[i]));
  queue_free(&$1);
  $result = o;
}
%typemap(arginit) Queue {
  queue_init(&$1);
}
%typemap(freearg) Queue {
  queue_free(&$1);
}


%include "cdata.i"
%include "file.i"
%include "typemaps.i"

%{
#include "stdio.h"
#include "pool.h"
#include "solver.h"
#include "repo_solv.h"
#include "chksum.h"

#include "repo_rpmdb.h"
#include "repo_rpmmd.h"
#include "repo_write.h"
#include "repo_products.h"
#include "repo_susetags.h"
#include "repo_updateinfoxml.h"
#include "repo_deltainfoxml.h"
#include "repo_repomdxml.h"
#include "repo_content.h"
#include "sat_xfopen.h"

#define true 1
#define false 1

typedef struct chksum Chksum;
typedef int bool;

typedef struct {
  Pool* pool;
  Id id;
} xSolvable;

typedef struct {
  Pool *pool;
  Id id;
} Pool_solvable_iterator;

typedef struct {
  Pool *pool;
  Id id;
} Pool_repo_iterator;

typedef struct {
  Repo *repo;
  Id id;
} Repo_solvable_iterator;

typedef struct {
  Id how;
  Id what;
} Job;

typedef struct {
  Solver *solv;
  Id id;
} Problem;

static inline xSolvable *xSolvable_create(Pool *pool, Id id)
{
  xSolvable *s = sat_calloc(sizeof(*s), 1);
  s->pool = pool;
  s->id = id;
  return s;
}

%}

typedef int Id;

%include "knownid.h"

# from repodata.h
%constant Id SOLVID_META;
%constant Id SOLVID_POS;

%constant int REL_EQ;
%constant int REL_GT;
%constant int REL_LT;
%constant int REL_ARCH;


# put before pool/repo so we can access the constructor
%nodefaultctor Dataiterator;
%nodefaultdtor Dataiterator;
typedef struct _Dataiterator {
  Pool * const pool;
  Repo * const repo;
  const Id solvid;
} Dataiterator;

%nodefaultdtor Pool;
typedef struct {
} Pool;

%nodefaultctor Repo;
%nodefaultdtor Repo;
typedef struct _Repo {
  const char * const name;
  int priority;
  int subpriority;
  PyObject *appdata;
} Repo;

%nodefaultctor xSolvable;
typedef struct {
  Pool* const pool;
  const Id id;
} xSolvable;

%nodefaultctor Pool_solvable_iterator;
typedef struct {} Pool_solvable_iterator;
%nodefaultctor Pool_repo_iterator;
typedef struct {} Pool_repo_iterator;
%nodefaultctor Repo_solvable_iterator;
typedef struct {} Repo_solvable_iterator;

%nodefaultctor Solver;
%nodefaultdtor Solver;
typedef struct {
  bool ignorealreadyrecommended;
  bool dosplitprovides;
  bool fixsystem;
  bool allowuninstall;
  bool distupgrade;
  bool allowdowngrade;
  bool allowarchchange;
  bool allowvendorchange;
} Solver;

typedef struct chksum {} Chksum;

%rename(xfopen) sat_xfopen;
%rename(xfopen_fd) sat_xfopen_fd;
%rename(xfclose) sat_xfclose;

FILE *sat_xfopen(const char *fn);
FILE *sat_xfopen_fd(const char *fn, int fd);
%inline {
  int sat_xfclose(FILE *fp) {
    return fclose(fp);
  }
}
typedef struct {
  Id how;
  Id what;
} Job;

typedef struct {
  Solver *solv;
  Id id;
} Problem;

%extend Job {
  static const Id SOLVER_SOLVABLE = SOLVER_SOLVABLE;
  static const Id SOLVER_SOLVABLE_NAME = SOLVER_SOLVABLE_NAME;
  static const Id SOLVER_SOLVABLE_PROVIDES = SOLVER_SOLVABLE_PROVIDES;
  static const Id SOLVER_SOLVABLE_ONE_OF = SOLVER_SOLVABLE_ONE_OF;
  static const Id SOLVER_SOLVABLE_REPO = SOLVER_SOLVABLE_REPO;
  static const Id SOLVER_SOLVABLE_ALL = SOLVER_SOLVABLE_ALL;
  static const Id SOLVER_SELECTMASK = SOLVER_SELECTMASK;
  static const Id SOLVER_NOOP = SOLVER_NOOP;
  static const Id SOLVER_INSTALL = SOLVER_INSTALL;
  static const Id SOLVER_ERASE = SOLVER_ERASE;
  static const Id SOLVER_UPDATE = SOLVER_UPDATE;
  static const Id SOLVER_WEAKENDEPS = SOLVER_WEAKENDEPS;
  static const Id SOLVER_NOOBSOLETES = SOLVER_NOOBSOLETES;
  static const Id SOLVER_LOCK = SOLVER_LOCK;
  static const Id SOLVER_DISTUPGRADE = SOLVER_DISTUPGRADE;
  static const Id SOLVER_VERIFY = SOLVER_VERIFY;
  static const Id SOLVER_DROP_ORPHANED = SOLVER_DROP_ORPHANED;
  static const Id SOLVER_USERINSTALLED = SOLVER_USERINSTALLED;
  static const Id SOLVER_JOBMASK = SOLVER_JOBMASK;
  static const Id SOLVER_WEAK = SOLVER_WEAK;
  static const Id SOLVER_ESSENTIAL = SOLVER_ESSENTIAL;
  static const Id SOLVER_CLEANDEPS = SOLVER_CLEANDEPS;
  static const Id SOLVER_SETEV = SOLVER_SETEV;
  static const Id SOLVER_SETEVR = SOLVER_SETEVR;
  static const Id SOLVER_SETARCH = SOLVER_SETARCH;
  static const Id SOLVER_SETVENDOR = SOLVER_SETVENDOR;
  static const Id SOLVER_SETREPO = SOLVER_SETREPO;
  static const Id SOLVER_NOAUTOSET = SOLVER_NOAUTOSET;
  static const Id SOLVER_SETMASK = SOLVER_SETMASK;

  Job(Id how, Id what) {
    Job *job = sat_calloc(sizeof(*job), 1);
    job->how = how;
    job->what = what;
    return job;
  }
}

%extend Chksum {
  Chksum(Id type) {
    return (Chksum *)sat_chksum_create(type);
  }
  ~Chksum() {
    sat_chksum_free($self, 0);
  }
  void add(const char *str) {
    sat_chksum_add($self, str, strlen((char *)$self));
  }
  void addfp(FILE *fp) {
    char buf[4096];
    int l;
    while ((l = fread(buf, 1, sizeof(buf), fp)) > 0)
      sat_chksum_add($self, buf, l);
    rewind(fp);         /* convenience */
  }
  void addfd(int fd) {
    char buf[4096];
    int l;
    while ((l = read(fd, buf, sizeof(buf))) > 0)
      sat_chksum_add($self, buf, l);
    lseek(fd, 0, 0);    /* convenience */
  }
  bool matches(char *othersum) {
    int l;
    unsigned char *b;
    b = sat_chksum_get($self, &l);
    return memcmp(b, (void *)othersum, l) == 0;
  }
  SWIGCDATA raw() {
    int l;
    unsigned char *b;
    b = sat_chksum_get($self, &l);
    return cdata_void((void *)b, l);
  }
}



%extend Pool {
  Pool() {
    Pool *pool = pool_create();
    return pool;
  }
  ~Pool() {
  }
  void set_debuglevel(int level) {
    pool_setdebuglevel($self, level);
  }
  Id str2id(const char *str, int create=1) {
    return str2id($self, str, create);
  }
  const char *id2str(Id id) {
    return id2str($self, id);
  }
  const char *dep2str(Id id) {
    return dep2str($self, id);
  }
  Id rel2id(Id name, Id evr, int flags, int create=1) {
    return rel2id($self, name, evr, flags, create);
  }
  void setarch(const char *arch) {
    pool_setarch($self, arch);
  }
  Repo *add_repo(const char *name) {
    return repo_create($self, name);
  }
  const char *lookup_str(Id entry, Id keyname) {
    return pool_lookup_str($self, entry, keyname);
  }
  Id lookup_id(Id entry, Id keyname) {
    return pool_lookup_id($self, entry, keyname);
  }
  unsigned int lookup_num(Id entry, Id keyname, unsigned int notfound = 0) {
    return pool_lookup_num($self, entry, keyname, notfound);
  }
  bool lookup_void(Id entry, Id keyname) {
    return pool_lookup_void($self, entry, keyname);
  }
  SWIGCDATA lookup_bin_checksum(Id entry, Id keyname, Id *OUTPUT) {
    int l;
    const unsigned char *b;
    *OUTPUT = 0;
    b = pool_lookup_bin_checksum($self, entry, keyname, OUTPUT);
    if (!b)
      return cdata_void(0, 0);
    return cdata_void((char *)b, sat_chksum_len(*OUTPUT));
  }
  const char *lookup_checksum(Id entry, Id keyname, Id *OUTPUT) {
    Id type = 0;
    *OUTPUT = 0;
    const char *b = pool_lookup_checksum($self, entry, keyname, OUTPUT);
    return b;
  }
  %newobject dataiterator_new;
  Dataiterator *dataiterator_new(Id p, Id key,  const char *match, int flags) {
    return new_Dataiterator($self, 0, p, key, match, flags);
  }
  const char *solvid2str(Id solvid) {
    return solvid2str($self, solvid);
  }
  void addfileprovides() {
    pool_addfileprovides($self);
  }
  void createwhatprovides() {
    pool_createwhatprovides($self);
  }
  Pool_solvable_iterator * const solvables;
  %{
  SWIGINTERN Pool_solvable_iterator * Pool_solvables_get(Pool *pool) {
    Pool_solvable_iterator *s;
    s = sat_calloc(sizeof(*s), 1);
    s->pool = pool;
    s->id = 0;
    return s;
  }
  %}
  Pool_repo_iterator * const repos;
  %{
  SWIGINTERN Pool_repo_iterator * Pool_repos_get(Pool *pool) {
    Pool_repo_iterator *s;
    s = sat_calloc(sizeof(*s), 1);
    s->pool = pool;
    s->id = 0;
    return s;
  }
  %}
  Repo *installed;
  %{
  SWIGINTERN void Pool_installed_set(Pool *pool, Repo *installed) {
    pool_set_installed(pool, installed);
  }
  Repo *Pool_installed_get(Pool *pool) {
    return pool->installed;
  }
  %}

  Queue providerids(Id dep) {
    Pool *pool = $self;
    Queue q;
    Id p, pp;
    queue_init(&q);
    FOR_PROVIDES(p, pp, dep)
      queue_push(&q, p);
    return q;
  }
  Queue allprovidingids() {
    Pool *pool = $self;
    Queue q;
    Id id;
    queue_init(&q);
    for (id = 1; id < pool->ss.nstrings; id++)
      if (pool->whatprovides[id])
        queue_push(&q, id);
    return q;
  }
  # move to job?
  Queue jobsolvids(Job *job) {
    Pool *pool = $self;
    Id p, pp, how;
    Queue q;
    queue_init(&q);
    how = job->how & SOLVER_SELECTMASK;
    FOR_JOB_SELECT(p, pp, how, job->what)
      queue_push(&q, p);
    return q;
  }

  %pythoncode %{
    def jobsolvables (self, *args):
      return [ self.solvables[id] for id in self.jobsolvids(*args) ]
    def providers(self, *args):
      return [ self.solvables[id] for id in self.providerids(*args) ]
  %}

  Id towhatprovides(Queue q) {
    return pool_queuetowhatprovides($self, &q);
  }
  bool isknownarch(Id id) {
    Pool *pool = $self;
    if (id == ARCH_SRC || id == ARCH_NOSRC || id == ARCH_NOARCH)
      return 1;
    if (pool->id2arch && (id > pool->lastarch || !pool->id2arch[id]))
      return 0;
    return 1;
  }

  %newobject create_solver;
  Solver *create_solver() {
    return solver_create($self);
  }
}

%extend Repo {
  static const int REPO_REUSE_REPODATA = REPO_REUSE_REPODATA;
  static const int REPO_NO_INTERNALIZE = REPO_NO_INTERNALIZE;
  static const int REPO_LOCALPOOL = REPO_LOCALPOOL;
  static const int REPO_USE_LOADING = REPO_USE_LOADING;
  static const int REPO_EXTEND_SOLVABLES = REPO_EXTEND_SOLVABLES;
  static const int SOLV_ADD_NO_STUBS = SOLV_ADD_NO_STUBS;       /* repo_solv */

  void free(int reuseids = 0) {
    repo_free($self, reuseids);
  }
  bool add_solv(const char *name, int flags = 0) {
    FILE *fp = fopen(name, "r");
    if (!fp)
      return 0;
    int r = repo_add_solv_flags($self, fp, flags);
    fclose(fp);
    return r == 0;
  }
  bool add_solv(FILE *fp, int flags = 0) {
    return repo_add_solv_flags($self, fp, flags) == 0;
  }
  bool add_products(const char *proddir, int flags = 0) {
    repo_add_products($self, proddir, 0, flags);
    return 1;
  }
  bool add_rpmmd(FILE *fp, const char *language, int flags = 0) {
    repo_add_rpmmd($self, fp, language, flags);
    return 1;
  }
  bool add_rpmdb(Repo *ref, int flags = 0) {
    repo_add_rpmdb($self, ref, 0, flags);
    return 1;
  }
  bool add_susetags(FILE *fp, Id defvendor, const char *language, int flags = 0) {
    repo_add_susetags($self, fp, defvendor, language, flags);
    return 1;
  }
  bool add_repomdxml(FILE *fp, int flags = 0) {
    repo_add_repomdxml($self, fp, flags);
    return 1;
  }
  bool add_content(FILE *fp, int flags = 0) {
    repo_add_content($self, fp, flags);
    return 1;
  }
  bool add_updateinfoxml(FILE *fp, int flags = 0) {
    repo_add_updateinfoxml($self, fp, flags);
    return 1;
  }
  bool add_deltainfoxml(FILE *fp, int flags = 0) {
    repo_add_deltainfoxml($self, fp, flags);
    return 1;
  }
  bool write(FILE *fp, int flags = 0) {
    repo_write($self, fp, repo_write_stdkeyfilter, 0, 0);
    return 1;
  }
  # HACK, remove if no longer needed!
  bool write_first_repodata(FILE *fp, int flags = 0) {
    int oldnrepodata = $self->nrepodata;
    $self->nrepodata = 1;
    repo_write($self, fp, repo_write_stdkeyfilter, 0, 0);
    $self->nrepodata = oldnrepodata;
    return 1;
  }
  %newobject dataiterator_new;
  Dataiterator *dataiterator_new(Id p, Id key,  const char *match, int flags) {
    return new_Dataiterator($self->pool, $self, p, key, match, flags);
  }

  Id const id;
  %{
  SWIGINTERN Id Repo_id_get(Repo *repo) {
    return repo->repoid;
  }
  %}
  Repo_solvable_iterator * const solvables;
  %{
  SWIGINTERN Repo_solvable_iterator * Repo_solvables_get(Repo *repo) {
    Repo_solvable_iterator *s;
    s = sat_calloc(sizeof(*s), 1);
    s->repo = repo;
    s->id = 0;
    return s;
  }
  %}
}

%extend Dataiterator {
  static const int SEARCH_STRING = SEARCH_STRING;
  static const int SEARCH_SUBSTRING = SEARCH_SUBSTRING;
  static const int SEARCH_GLOB = SEARCH_GLOB;
  static const int SEARCH_REGEX = SEARCH_REGEX;
  static const int SEARCH_NOCASE = SEARCH_NOCASE;
  static const int SEARCH_FILES = SEARCH_FILES;
  static const int SEARCH_COMPLETE_FILELIST = SEARCH_COMPLETE_FILELIST;

  Dataiterator(Pool *pool, Repo *repo, Id p, Id key, const char *match, int flags) {
    Dataiterator *di = sat_calloc(sizeof(*di), 1);
    dataiterator_init(di, pool, repo, p, key, match, flags);
    return di;
  }
  ~Dataiterator() {
    dataiterator_free($self);
  }
  Dataiterator *__iter__() {
    return $self;
  }
  %exception next {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
  %newobject next;
  Dataiterator *next() {
    Dataiterator *ndi;
    if (!dataiterator_step($self)) {
      return 0;
    }
    ndi = sat_calloc(sizeof(*ndi), 1);
    dataiterator_init_clone(ndi, $self);
    return ndi;
  }
  void setpos_parent() {
    dataiterator_setpos_parent($self);
  }
  void prepend_keyname(Id key) {
    dataiterator_prepend_keyname($self, key);
  }
  void skip_solvable() {
    dataiterator_skip_solvable($self);
  }

  %newobject solvable;
  xSolvable * const solvable;
  %{
  SWIGINTERN xSolvable *Dataiterator_solvable_get(Dataiterator *di) {
    return di->solvid ? xSolvable_create(di->pool, di->solvid) : 0;
  }
  %}
  Id key_id() {
    return $self->key->name;
  }
  const char *key_idstr() {
    return id2str($self->pool, $self->key->name);
  }
  Id keytype_id() {
    return $self->key->type;
  }
  const char *keytype_idstr() {
    return id2str($self->pool, $self->key->type);
  }
  Id match_id() {
     return $self->kv.id;
  }
  const char *match_idstr() {
     return id2str($self->pool, $self->kv.id);
  }
  const char *match_str() {
     return $self->kv.str;
  }
  int match_num() {
     return $self->kv.num;
  }
  int match_num2() {
     return $self->kv.num2;
  }
}


%extend Pool_solvable_iterator {
  Pool_solvable_iterator *__iter__() {
    return $self;
  }
  %exception next {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
  %newobject next;
  xSolvable *next() {
    Pool *pool = $self->pool;
    xSolvable *s;
    if ($self->id >= pool->nsolvables)
      return 0;
    while (++$self->id < pool->nsolvables)
      if (pool->solvables[$self->id].repo)
        return xSolvable_create(pool, $self->id);
    return 0;
  }
  %newobject __getitem__;
  xSolvable *__getitem__(Id key) {
    Pool *pool = $self->pool;
    if (key > 0 && key < pool->nsolvables && pool->solvables[key].repo)
      return xSolvable_create(pool, key);
    return 0;
  }
}

%extend Pool_repo_iterator {
  Pool_repo_iterator *__iter__() {
    return $self;
  }
  %exception next {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
  Repo *next() {
    Pool *pool = $self->pool;
    if ($self->id >= pool->nrepos + 1)
      return 0;
    while (++$self->id < pool->nrepos + 1) {
      Repo *r = pool_id2repo(pool, $self->id);
      if (r)
        return r;
    }
    return 0;
  }
  Repo *__getitem__(Id key) {
    Pool *pool = $self->pool;
    if (key > 0 && key < pool->nrepos + 1)
      return pool_id2repo(pool, key);
    return 0;
  }
}

%extend Repo_solvable_iterator {
  Repo_solvable_iterator *__iter__() {
    return $self;
  }
  %exception next {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
  %newobject next;
  xSolvable *next() {
    Repo *repo = $self->repo;
    Pool *pool = repo->pool;
    xSolvable *s;
    if (repo->start > 0 && $self->id < repo->start)
      $self->id = repo->start - 1;
    if ($self->id >= repo->end)
      return 0;
    while (++$self->id < repo->end)
      if (pool->solvables[$self->id].repo == repo)
        return xSolvable_create(pool, $self->id);
    return 0;
  }
  %newobject __getitem__;
  xSolvable *__getitem__(Id key) {
    Repo *repo = $self->repo;
    Pool *pool = repo->pool;
    if (key > 0 && key < pool->nsolvables && pool->solvables[key].repo == repo)
      return xSolvable_create(pool, key);
    return 0;
  }
}

%extend xSolvable {
  const char *str() {
    return solvid2str($self->pool, $self->id);
  }
  const char *lookup_str(Id keyname) {
    return pool_lookup_str($self->pool, $self->id, keyname);
  }
  Id lookup_id(Id entry, Id keyname) {
    return pool_lookup_id($self->pool, $self->id, keyname);
  }
  unsigned int lookup_num(Id keyname, unsigned int notfound = 0) {
    return pool_lookup_num($self->pool, $self->id, keyname, notfound);
  }
  bool lookup_void(Id keyname) {
    return pool_lookup_void($self->pool, $self->id, keyname);
  }
  SWIGCDATA lookup_bin_checksum(Id keyname, Id *OUTPUT) {
    int l;
    const unsigned char *b;
    *OUTPUT = 0;
    b = pool_lookup_bin_checksum($self->pool, $self->id, keyname, OUTPUT);
    if (!b)
      return cdata_void(0, 0);
    return cdata_void((char *)b, sat_chksum_len(*OUTPUT));
  }
  const char *lookup_checksum(Id keyname, Id *OUTPUT) {
    Id type = 0;
    *OUTPUT = 0;
    const char *b = pool_lookup_checksum($self->pool, $self->id, keyname, OUTPUT);
    return b;
  }
  bool installable() {
    return pool_installable($self->pool, pool_id2solvable($self->pool, $self->id));
  }
  bool isinstalled() {
    Pool *pool = $self->pool;
    return pool->installed && pool_id2solvable(pool, $self->id)->repo == pool->installed;
  }

  const char * const name;
  %{
    SWIGINTERN const char *xSolvable_name_get(xSolvable *xs) {
      Pool *pool = xs->pool;
      return id2str(pool, pool->solvables[xs->id].name);
    }
  %}
  Id const nameid;
  %{
    SWIGINTERN Id xSolvable_nameid_get(xSolvable *xs) {
      return xs->pool->solvables[xs->id].name;
    }
  %}
  const char * const evr;
  %{
    SWIGINTERN const char *xSolvable_evr_get(xSolvable *xs) {
      Pool *pool = xs->pool;
      return id2str(pool, pool->solvables[xs->id].evr);
    }
  %}
  Id const evrid;
  %{
    SWIGINTERN Id xSolvable_evrid_get(xSolvable *xs) {
      return xs->pool->solvables[xs->id].evr;
    }
  %}
  const char * const arch;
  %{
    SWIGINTERN const char *xSolvable_arch_get(xSolvable *xs) {
      Pool *pool = xs->pool;
      return id2str(pool, pool->solvables[xs->id].arch);
    }
  %}
  Id const archid;
  %{
    SWIGINTERN Id xSolvable_archid_get(xSolvable *xs) {
      return xs->pool->solvables[xs->id].arch;
    }
  %}
  Repo * const repo;
  %{
    SWIGINTERN Repo *xSolvable_repo_get(xSolvable *xs) {
      return xs->pool->solvables[xs->id].repo;
    }
  %}
}

%extend Problem {
  Problem(Solver *solv, Id id) {
    Problem *p;
    p = sat_calloc(sizeof(*p), 1);
    p->solv = solv;
    p->id = id;
    return p;
  }
  Id findproblemrule() {
    return solver_findproblemrule($self->solv, $self->id);
  }
  Queue findallproblemrules(int unfiltered=0) {
    Solver *solv = $self->solv;
    Id probr;
    int i, j;
    Queue q;
    queue_init(&q);
    solver_findallproblemrules(solv, $self->id, &q);
    if (!unfiltered)
      {
        for (i = j = 0; i < q.count; i++)
          {
            probr = q.elements[i];
            if ((probr >= solv->updaterules && probr < solv->updaterules_end) || (probr >= solv->jobrules && probr < solv->jobrules_end))
              continue;
            q.elements[j++] = probr;
          }
        if (j)
          queue_truncate(&q, j);
      }
    return q;
  }
}

%extend Solver {
  ~Solver() {
    solver_free($self);
  }
  %pythoncode %{
    def solve(self, jobs):
      j = []
      for job in jobs: j += [job.how, job.what]
      nprob = self.solve_wrap(j)
      return [ Problem(self, pid) for pid in range(1, nprob + 1) ]
  %}
  int solve_wrap(Queue jobs) {
    solver_solve($self, &jobs);
    return solver_problem_count($self);
  }
  %apply Id *OUTPUT { Id *source, Id *target, Id *dep };
  int ruleinfo(Id ruleid, Id *source, Id *target, Id *dep) {
    return solver_ruleinfo($self, ruleid, source, target, dep);
  }
}
