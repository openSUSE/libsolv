#
##if defined(SWIGRUBY)
#  %rename("to_s") string();
##endif
##if defined(SWIGPYTHON)
#  %rename("__str__") string();
##endif
#

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

#define SOLVER_SOLUTION_DEINSTALL -100
#define SOLVER_SOLUTION_REPLACE -101
typedef struct chksum Chksum;
typedef int bool;

typedef struct {
  Pool* pool;
  Id id;
} XSolvable;

typedef struct {
  Solver* solv;
  Id id;
} XRule;

typedef struct {
  Repo* repo;
  Id id;
} XRepodata;

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
  Pool *pool;
  Id how;
  Id what;
} Job;

typedef struct {
  Solver *solv;
  Id id;
} Problem;

typedef struct {
  Solver *solv;
  Id problemid;
  Id id;
} Solution;

typedef struct {
  Solver *solv;
  Id problemid;
  Id solutionid;
  Id id;

  Id type;
  Id p;
  Id rp;
} Solutionelement;

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


typedef struct {
  Pool* const pool;
  Id const id;
} XSolvable;

typedef struct {
  Solver* const solv;
  Id const id;
} XRule;

typedef struct {
  Repo* const repo;
  Id const id;
} XRepodata;

# put before pool/repo so we can access the constructor
%nodefaultctor Dataiterator;
%nodefaultdtor Dataiterator;
typedef struct _Dataiterator {
  Pool * const pool;
  Repo * const repo;
  const Id solvid;
} Dataiterator;

typedef struct {
  Pool * const pool;
  Id how;
  Id what;
} Job;

%nodefaultctor Pool;
%nodefaultdtor Pool;
typedef struct {
} Pool;

%nodefaultctor Repo;
%nodefaultdtor Repo;
typedef struct _Repo {
  Pool * const pool;
  const char * const name;
  int priority;
  int subpriority;
  int const nsolvables;
#if defined(SWIGRUBY)
  VALUE appdata;
#endif
#if defined(SWIGPERL)
  SV *appdata;
#endif
} Repo;

%nodefaultctor Pool_solvable_iterator;
typedef struct {} Pool_solvable_iterator;

%nodefaultctor Pool_repo_iterator;
typedef struct {} Pool_repo_iterator;

%nodefaultctor Repo_solvable_iterator;
typedef struct {} Repo_solvable_iterator;

%nodefaultctor Solver;
%nodefaultdtor Solver;
typedef struct {
  Pool * const pool;
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
%rename(xfileno) sat_xfileno;

FILE *sat_xfopen(const char *fn);
FILE *sat_xfopen_fd(const char *fn, int fd);
%inline {
  int sat_xfclose(FILE *fp) {
    return fclose(fp);
  }
  int sat_xfileno(FILE *fp) {
    return fileno(fp);
  }
}

typedef struct {
  Solver * const solv;
  Id const id;
} Problem;

typedef struct {
  Solver * const solv;
  Id const problemid;
  Id const id;
} Solution;

typedef struct {
  Solver *const solv;
  Id const problemid;
  Id const solutionid;
  Id const id;
  Id const type;
} Solutionelement;

%nodefaultctor Transaction;
%nodefaultdtor Transaction;
typedef struct {
  Pool * const pool;
} Transaction;



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

  Job(Pool *pool, Id how, Id what) {
    Job *job = sat_calloc(1, sizeof(*job));
    job->pool = pool;
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
    sat_chksum_add($self, str, strlen((char *)str));
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
#if defined(SWIGPYTHON)
  %{
  SWIGINTERN int loadcallback(Pool *pool, Repodata *data, void *d) {
    XRepodata *xd = new_XRepodata(data->repo, data - data->repo->repodata);
    PyObject *args = Py_BuildValue("(O)", SWIG_NewPointerObj(SWIG_as_voidptr(xd), SWIGTYPE_p_XRepodata, SWIG_POINTER_OWN | 0));
    PyObject *result = PyEval_CallObject((PyObject *)d, args);
    if (!result)
      return 0; /* exception */
    int ecode = 0;
    int vresult = 0;
    Py_DECREF(args);
    ecode = SWIG_AsVal_int(result, &vresult);
    Py_DECREF(result);
    return SWIG_IsOK(ecode) ? vresult : 0;
  }
  %}
  void set_loadcallback(PyObject *callable) {
    if (!callable) {
      if ($self->loadcallback == loadcallback) {
        Py_DECREF($self->loadcallbackdata);
        pool_setloadcallback($self, 0, 0);
      }
      return;
    }
    Py_INCREF(callable);
    pool_setloadcallback($self, loadcallback, callable);
  }
#endif
  void free() {
#if defined(SWIGPYTHON)
    Pool_set_loadcallback($self, 0);
#endif
    pool_free($self);
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
  Id id2langid(Id id, const char *lang, int create=1) {
    return pool_id2langid($self, id, lang, create);
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
  %newobject solvables;
  Pool_solvable_iterator * const solvables;
  %{
  SWIGINTERN Pool_solvable_iterator * Pool_solvables_get(Pool *pool) {
    Pool_solvable_iterator *s;
    s = sat_calloc(1, sizeof(*s));
    s->pool = pool;
    s->id = 0;
    return s;
  }
  %}
  %newobject repos;
  Pool_repo_iterator * const repos;
  %{
  SWIGINTERN Pool_repo_iterator * Pool_repos_get(Pool *pool) {
    Pool_repo_iterator *s;
    s = sat_calloc(1, sizeof(*s));
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
  Job *Job(Id how, Id what) {
    return new_Job($self, how, what);
  }

#if defined(SWIGPYTHON)
  %pythoncode {
    def jobsolvables (self, *args):
      return [ self.solvables[id] for id in self.jobsolvids(*args) ]
    def providers(self, *args):
      return [ self.solvables[id] for id in self.providerids(*args) ]
  }
#endif

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
  static const int SUSETAGS_RECORD_SHARES = SUSETAGS_RECORD_SHARES; /* repo_susetags */

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
  void internalize() {
    repo_internalize($self);
  }
  const char *lookup_str(Id entry, Id keyname) {
    return repo_lookup_str($self, entry, keyname);
  }
  Id lookup_id(Id entry, Id keyname) {
    return repo_lookup_id($self, entry, keyname);
  }
  unsigned int lookup_num(Id entry, Id keyname, unsigned int notfound = 0) {
    return repo_lookup_num($self, entry, keyname, notfound);
  }
  void write(FILE *fp) {
    repo_write($self, fp, repo_write_stdkeyfilter, 0, 0);
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
    s = sat_calloc(1, sizeof(*s));
    s->repo = repo;
    s->id = 0;
    return s;
  }
  %}

  XRepodata *add_repodata(int flags = 0) {
    Repodata *rd = repo_add_repodata($self, flags);
    return new_XRepodata($self, rd - $self->repodata);
  }

  void create_stubs() {
    Repodata *data;
    if (!$self->nrepodata)
      return;
    data = $self->repodata  + ($self->nrepodata - 1);
    if (data->state != REPODATA_STUB)
      repodata_create_stubs(data);
  }
#if defined(SWIGPYTHON)
  PyObject *appdata;
  %{
  SWIGINTERN void Repo_appdata_set(Repo *repo, PyObject *o) {
    repo->appdata = o;
  }
  SWIGINTERN PyObject *Repo_appdata_get(Repo *repo) {
    PyObject *o = repo->appdata;
    Py_INCREF(o);
    return o;
  }
  %}
#endif
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
    Dataiterator *di = sat_calloc(1, sizeof(*di));
    dataiterator_init(di, pool, repo, p, key, match, flags);
    return di;
  }
  ~Dataiterator() {
    dataiterator_free($self);
    sat_free($self);
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
    ndi = sat_calloc(1, sizeof(*ndi));
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
  XSolvable * const solvable;
  %{
  SWIGINTERN XSolvable *Dataiterator_solvable_get(Dataiterator *di) {
    return new_XSolvable(di->pool, di->solvid);
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
  XSolvable *next() {
    Pool *pool = $self->pool;
    XSolvable *s;
    if ($self->id >= pool->nsolvables)
      return 0;
    while (++$self->id < pool->nsolvables)
      if (pool->solvables[$self->id].repo)
        return new_XSolvable(pool, $self->id);
    return 0;
  }
  %newobject __getitem__;
  XSolvable *__getitem__(Id key) {
    Pool *pool = $self->pool;
    if (key > 0 && key < pool->nsolvables && pool->solvables[key].repo)
      return new_XSolvable(pool, key);
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
  XSolvable *next() {
    Repo *repo = $self->repo;
    Pool *pool = repo->pool;
    XSolvable *s;
    if (repo->start > 0 && $self->id < repo->start)
      $self->id = repo->start - 1;
    if ($self->id >= repo->end)
      return 0;
    while (++$self->id < repo->end)
      if (pool->solvables[$self->id].repo == repo)
        return new_XSolvable(pool, $self->id);
    return 0;
  }
  %newobject __getitem__;
  XSolvable *__getitem__(Id key) {
    Repo *repo = $self->repo;
    Pool *pool = repo->pool;
    if (key > 0 && key < pool->nsolvables && pool->solvables[key].repo == repo)
      return new_XSolvable(pool, key);
    return 0;
  }
}

%extend XSolvable {
  XSolvable(Pool *pool, Id id) {
    if (!id)
      return 0;
    XSolvable *s = sat_calloc(1, sizeof(*s));
    s->pool = pool;
    s->id = id;
    return s;
  }
  const char *str() {
    return solvid2str($self->pool, $self->id);
  }
  const char *lookup_str(Id keyname) {
    return pool_lookup_str($self->pool, $self->id, keyname);
  }
  Id lookup_id(Id keyname) {
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
  const char *lookup_location(int *OUTPUT) {
    return solvable_get_location($self->pool->solvables + $self->id, OUTPUT);
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
    SWIGINTERN const char *XSolvable_name_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return id2str(pool, pool->solvables[xs->id].name);
    }
  %}
  Id const nameid;
  %{
    SWIGINTERN Id XSolvable_nameid_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].name;
    }
  %}
  const char * const evr;
  %{
    SWIGINTERN const char *XSolvable_evr_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return id2str(pool, pool->solvables[xs->id].evr);
    }
  %}
  Id const evrid;
  %{
    SWIGINTERN Id XSolvable_evrid_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].evr;
    }
  %}
  const char * const arch;
  %{
    SWIGINTERN const char *XSolvable_arch_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return id2str(pool, pool->solvables[xs->id].arch);
    }
  %}
  Id const archid;
  %{
    SWIGINTERN Id XSolvable_archid_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].arch;
    }
  %}
  Repo * const repo;
  %{
    SWIGINTERN Repo *XSolvable_repo_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].repo;
    }
  %}
}

%extend Problem {
  Problem(Solver *solv, Id id) {
    Problem *p;
    p = sat_calloc(1, sizeof(*p));
    p->solv = solv;
    p->id = id;
    return p;
  }
  Id findproblemrule_helper() {
    return solver_findproblemrule($self->solv, $self->id);
  }
  Queue findallproblemrules_helper(int unfiltered=0) {
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
  int solution_count() {
    return solver_solution_count($self->solv, $self->id);
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def findproblemrule(self):
      return XRule(self.solv, self.findproblemrule_helper())
    def findallproblemrules(self, unfiltered=0):
      return [ XRule(self.solv, i) for i in self.findallproblemrules_helper(unfiltered) ]
    def solutions(self):
      return [ Solution(self, i) for i in range(1, self.solution_count() + 1) ];
  }
#endif
}

%extend Solution {
  Solution(Problem *p, Id id) {
    Solution *s;
    s = sat_calloc(1, sizeof(*s));
    s->solv = p->solv;
    s->problemid = p->id;
    s->id = id;
    return s;
  }
  int element_count() {
    return solver_solutionelement_count($self->solv, $self->problemid, $self->id);
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def elements(self):
      return [ Solutionelement(self, i) for i in range(1, self.element_count() + 1) ];
  }
#endif
}

%extend Solutionelement {
  Solutionelement(Solution *s, Id id) {
    Solutionelement *e;
    e = sat_calloc(1, sizeof(*e));
    e->solv = s->solv;
    e->problemid = s->problemid;
    e->solutionid = s->id;
    e->id = id;
    solver_next_solutionelement(e->solv, e->problemid, e->solutionid, e->id - 1, &e->p, &e->rp);
    if (e->p > 0) {
      e->type = e->rp ? SOLVER_SOLUTION_REPLACE : SOLVER_SOLUTION_DEINSTALL;
    } else {
      e->type = e->p;
      e->p = e->rp;
      e->rp = 0;
    }
    return e;
  }
  %newobject solvable;
  XSolvable * const solvable;
  %newobject replacement;
  XSolvable * const replacement;
  int const jobidx;
  %{
    SWIGINTERN XSolvable *Solutionelement_solvable_get(Solutionelement *e) {
      return new_XSolvable(e->solv->pool, e->p);
    }
    SWIGINTERN XSolvable *Solutionelement_replacement_get(Solutionelement *e) {
      return new_XSolvable(e->solv->pool, e->rp);
    }
    SWIGINTERN int Solutionelement_jobidx_get(Solutionelement *e) {
      return (e->p - 1) / 2;
    }
  %}
}

%extend Solver {
  static const int SOLVER_RULE_UNKNOWN = SOLVER_RULE_UNKNOWN;
  static const int SOLVER_RULE_RPM = SOLVER_RULE_RPM;
  static const int SOLVER_RULE_RPM_NOT_INSTALLABLE = SOLVER_RULE_RPM_NOT_INSTALLABLE;
  static const int SOLVER_RULE_RPM_NOTHING_PROVIDES_DEP = SOLVER_RULE_RPM_NOTHING_PROVIDES_DEP;
  static const int SOLVER_RULE_RPM_PACKAGE_REQUIRES = SOLVER_RULE_RPM_PACKAGE_REQUIRES;
  static const int SOLVER_RULE_RPM_SELF_CONFLICT = SOLVER_RULE_RPM_SELF_CONFLICT;
  static const int SOLVER_RULE_RPM_PACKAGE_CONFLICT = SOLVER_RULE_RPM_PACKAGE_CONFLICT;
  static const int SOLVER_RULE_RPM_SAME_NAME = SOLVER_RULE_RPM_SAME_NAME;
  static const int SOLVER_RULE_RPM_PACKAGE_OBSOLETES = SOLVER_RULE_RPM_PACKAGE_OBSOLETES;
  static const int SOLVER_RULE_RPM_IMPLICIT_OBSOLETES = SOLVER_RULE_RPM_IMPLICIT_OBSOLETES;
  static const int SOLVER_RULE_RPM_INSTALLEDPKG_OBSOLETES = SOLVER_RULE_RPM_INSTALLEDPKG_OBSOLETES;
  static const int SOLVER_RULE_UPDATE = SOLVER_RULE_UPDATE;
  static const int SOLVER_RULE_FEATURE = SOLVER_RULE_FEATURE;
  static const int SOLVER_RULE_JOB = SOLVER_RULE_JOB;
  static const int SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP = SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP;
  static const int SOLVER_RULE_DISTUPGRADE = SOLVER_RULE_DISTUPGRADE;
  static const int SOLVER_RULE_INFARCH = SOLVER_RULE_INFARCH;
  static const int SOLVER_RULE_CHOICE = SOLVER_RULE_CHOICE;
  static const int SOLVER_RULE_LEARNT = SOLVER_RULE_LEARNT;

  static const int SOLVER_SOLUTION_JOB = SOLVER_SOLUTION_JOB;
  static const int SOLVER_SOLUTION_INFARCH = SOLVER_SOLUTION_INFARCH;
  static const int SOLVER_SOLUTION_DISTUPGRADE = SOLVER_SOLUTION_DISTUPGRADE;
  static const int SOLVER_SOLUTION_DEINSTALL = SOLVER_SOLUTION_DEINSTALL;
  static const int SOLVER_SOLUTION_REPLACE = SOLVER_SOLUTION_REPLACE;

  ~Solver() {
    solver_free($self);
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def solve(self, jobs):
      j = []
      for job in jobs: j += [job.how, job.what]
      nprob = self.solve_helper(j)
      return [ Problem(self, pid) for pid in range(1, nprob + 1) ]
  }
#endif
  int solve_helper(Queue jobs) {
    solver_solve($self, &jobs);
    return solver_problem_count($self);
  }
  %newobject transaction;
  Transaction *transaction() {
    Transaction *t;
    t = sat_calloc(1, sizeof(*t));
    transaction_init_clone(t, &$self->trans);
    return t;
  }
}

%extend Transaction {
  static const int SOLVER_TRANSACTION_IGNORE = SOLVER_TRANSACTION_IGNORE;
  static const int SOLVER_TRANSACTION_ERASE = SOLVER_TRANSACTION_ERASE;
  static const int SOLVER_TRANSACTION_REINSTALLED = SOLVER_TRANSACTION_REINSTALLED;
  static const int SOLVER_TRANSACTION_DOWNGRADED = SOLVER_TRANSACTION_DOWNGRADED;
  static const int SOLVER_TRANSACTION_CHANGED = SOLVER_TRANSACTION_CHANGED;
  static const int SOLVER_TRANSACTION_UPGRADED = SOLVER_TRANSACTION_UPGRADED;
  static const int SOLVER_TRANSACTION_OBSOLETED = SOLVER_TRANSACTION_OBSOLETED;
  static const int SOLVER_TRANSACTION_INSTALL = SOLVER_TRANSACTION_INSTALL;
  static const int SOLVER_TRANSACTION_REINSTALL = SOLVER_TRANSACTION_REINSTALL;
  static const int SOLVER_TRANSACTION_DOWNGRADE = SOLVER_TRANSACTION_DOWNGRADE;
  static const int SOLVER_TRANSACTION_CHANGE = SOLVER_TRANSACTION_CHANGE;
  static const int SOLVER_TRANSACTION_UPGRADE = SOLVER_TRANSACTION_UPGRADE;
  static const int SOLVER_TRANSACTION_OBSOLETES = SOLVER_TRANSACTION_OBSOLETES;
  static const int SOLVER_TRANSACTION_MULTIINSTALL = SOLVER_TRANSACTION_MULTIINSTALL;
  static const int SOLVER_TRANSACTION_MULTIREINSTALL = SOLVER_TRANSACTION_MULTIREINSTALL;
  static const int SOLVER_TRANSACTION_MAXTYPE = SOLVER_TRANSACTION_MAXTYPE;
  static const int SOLVER_TRANSACTION_SHOW_ACTIVE = SOLVER_TRANSACTION_SHOW_ACTIVE;
  static const int SOLVER_TRANSACTION_SHOW_ALL = SOLVER_TRANSACTION_SHOW_ALL;
  static const int SOLVER_TRANSACTION_SHOW_OBSOLETES = SOLVER_TRANSACTION_SHOW_OBSOLETES;
  static const int SOLVER_TRANSACTION_SHOW_MULTIINSTALL = SOLVER_TRANSACTION_SHOW_MULTIINSTALL;
  static const int SOLVER_TRANSACTION_CHANGE_IS_REINSTALL = SOLVER_TRANSACTION_CHANGE_IS_REINSTALL;
  static const int SOLVER_TRANSACTION_MERGE_VENDORCHANGES = SOLVER_TRANSACTION_MERGE_VENDORCHANGES;
  static const int SOLVER_TRANSACTION_MERGE_ARCHCHANGES = SOLVER_TRANSACTION_MERGE_ARCHCHANGES;
  static const int SOLVER_TRANSACTION_RPM_ONLY = SOLVER_TRANSACTION_RPM_ONLY;
  static const int SOLVER_TRANSACTION_ARCHCHANGE = SOLVER_TRANSACTION_ARCHCHANGE;
  static const int SOLVER_TRANSACTION_VENDORCHANGE = SOLVER_TRANSACTION_VENDORCHANGE;
  static const int SOLVER_TRANSACTION_KEEP_ORDERDATA = SOLVER_TRANSACTION_KEEP_ORDERDATA;
  ~Transaction() {
    transaction_free($self);
    sat_free($self);
  }
  bool isempty() {
    return $self->steps.count == 0;
  }
  Queue classify_helper(int mode) {
    Queue q;
    queue_init(&q);
    transaction_classify($self, mode, &q);
    return q;
  }
  Queue classify_pkgs_helper(int mode, Id cl, Id from, Id to) {
    Queue q;
    queue_init(&q);
    transaction_classify_pkgs($self, mode, cl, from, to, &q);
    return q;
  }
  %newobject othersolvable;
  XSolvable *othersolvable(XSolvable *s) {
    Id op = transaction_obs_pkg($self, s->id);
    return new_XSolvable($self->pool, op);
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def classify(self, mode = 0):
      r = []
      cr = self.classify_helper(mode)
      for type, cnt, fromid, toid in zip(*([iter(cr)] * 4)):
        if type != self.SOLVER_TRANSACTION_IGNORE:
          r.append([ type, [ self.pool.solvables[j] for j in self.classify_pkgs_helper(mode, type, fromid, toid) ], fromid, toid ])
      return r
    }
#endif
  Queue installedresult_helper(int *OUTPUT) {
    Queue q;
    queue_init(&q);
    *OUTPUT = transaction_installedresult(self, &q);
    return q;
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def installedresult(self):
      r = self.installedresult_helper()
      newpkgs = r.pop()
      rn = [ self.pool.solvables[r[i]] for i in range(0, newpkgs) ]
      rk = [ self.pool.solvables[r[i]] for i in range(newpkgs, len(r)) ]
      return rn, rk
  }
#endif
  Queue steps_helper() {
    Queue q;
    queue_init_clone(&q, &$self->steps);
    return q;
  }
  int steptype(XSolvable *s, int mode) {
    return transaction_type($self, s->id, mode);
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def steps(self):
      return [ self.pool.solvables[i] for i in self.steps_helper() ]
  }
#endif
  int calc_installsizechange() {
    return transaction_calc_installsizechange($self);
  }
}

%extend XRule {
  XRule(Solver *solv, Id id) {
    if (!id)
      return 0;
    XRule *xr = sat_calloc(1, sizeof(*xr));
    xr->solv = solv;
    xr->id = id;
    return xr;
  }
  %apply Id *OUTPUT { Id *source, Id *target, Id *dep };
  int info_helper(Id *source, Id *target, Id *dep) {
    return solver_ruleinfo($self->solv, $self->id, source, target, dep);
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def info(self):
      type, source, target, dep = self.info_helper()
      if source:
          source = self.solv.pool.solvables[source]
      if target:
          target = self.solv.pool.solvables[target]
      return type, source, target, dep
  }
#endif
}


%extend XRepodata {
  XRepodata(Repo *repo, Id id) {
    XRepodata *xr = sat_calloc(1, sizeof(*xr));
    xr->repo = repo;
    xr->id = id;
    return xr;
  }
  Id new_handle() {
    return repodata_new_handle($self->repo->repodata + $self->id);
  }
  void set_id(Id solvid, Id keyname, Id id) {
    repodata_set_id($self->repo->repodata + $self->id, solvid, keyname, id);
  }
  void set_str(Id solvid, Id keyname, const char *str) {
    repodata_set_str($self->repo->repodata + $self->id, solvid, keyname, str);
  }
  void set_poolstr(Id solvid, Id keyname, const char *str) {
    repodata_set_poolstr($self->repo->repodata + $self->id, solvid, keyname, str);
  }
  void add_idarray(Id solvid, Id keyname, Id id) {
    repodata_add_idarray($self->repo->repodata + $self->id, solvid, keyname, id);
  }
  void add_flexarray(Id solvid, Id keyname, Id handle) {
    repodata_add_flexarray($self->repo->repodata + $self->id, solvid, keyname, handle);
  }
  void set_bin_checksum(Id solvid, Id keyname, Id chksumtype, const char *chksum) {
    repodata_set_bin_checksum($self->repo->repodata + $self->id, solvid, keyname, chksumtype, (const unsigned char *)chksum);
  }
  const char *lookup_str(Id solvid, Id keyname) {
    return repodata_lookup_str($self->repo->repodata + $self->id, solvid, keyname);
  }
  SWIGCDATA lookup_bin_checksum(Id solvid, Id keyname, Id *OUTPUT) {
    const unsigned char *b;
    *OUTPUT = 0;
    b = repodata_lookup_bin_checksum($self->repo->repodata + $self->id, solvid, keyname, OUTPUT);
    return cdata_void((char *)b, sat_chksum_len(*OUTPUT));
  }
  void internalize() {
    repodata_internalize($self->repo->repodata + $self->id);
  }
  void create_stubs() {
    repodata_create_stubs($self->repo->repodata + $self->id);
  }
  void write(FILE *fp) {
    repodata_write($self->repo->repodata + $self->id, fp, repo_write_stdkeyfilter, 0);
  }
}
