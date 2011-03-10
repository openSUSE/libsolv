#
# WARNING: for perl iterator/array support you need to run
#   sed -i -e 's/SvTYPE(tsv) == SVt_PVHV/SvTYPE(tsv) == SVt_PVHV || SvTYPE(tsv) == SVt_PVAV/'
# on the generated c code
#

#
##if defined(SWIGRUBY)
#  %rename("to_s") string();
##endif
##if defined(SWIGPYTHON)
#  %rename("__str__") string();
##endif
#

%module solv

#if defined(SWIGPYTHON)
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
#endif
#if defined(SWIGPERL)
%typemap(in) Queue {
  AV *av;
  int i, size;
  queue_init(&$1);
  if (!SvROK($input) || SvTYPE(SvRV($input)) != SVt_PVAV)
    SWIG_croak("Argument $argnum is not an array reference.");
  av = (AV*)SvRV($input);
  size = av_len(av);
  for (i = 0; i <= size; i++) {
    SV **sv = av_fetch(av, i, 0);
    int v;
    int e = SWIG_AsVal_int(*sv, &v);
    if (!SWIG_IsOK(e)) {
      SWIG_croak("list must contain only integers");
    }
    queue_push(&$1, v);
  }
}
# AV *o = newAV();
# av_push(o, SvREFCNT_inc(SWIG_From_int($1.elements[i])));
# $result = newRV_noinc((SV*)o); argvi++;
#
%typemap(out) Queue {
  int i;
  if (argvi + $1.count + 1>= items) {
    EXTEND(sp, items - (argvi + $1.count + 1) + 1);
  }
  for (i = 0; i < $1.count; i++)
    ST(argvi++) = SvREFCNT_inc(SWIG_From_int($1.elements[i]));
  queue_free(&$1);
  $result = 0;
}
#endif
%typemap(arginit) Queue {
  queue_init(&$1);
}
%typemap(freearg) Queue {
  queue_free(&$1);
}

#if defined(SWIGRUBY)
%typemap(in) Queue {
  int size, i;
  VALUE *o;
  queue_init(&$1);
  size = RARRAY($input)->len;
  i = 0;
  o = RARRAY($input)->ptr;
  for (i = 0; i < size; i++, o++) {
    int v;
    int e = SWIG_AsVal_int(*o, &v);
    if (!SWIG_IsOK(e))
      SWIG_croak("list must contain only integers");
    queue_push(&$1, v);
  }
}
%typemap(out) Queue {
  int i;
  VALUE o = rb_ary_new2($1.count);
  for (i = 0; i < $1.count; i++)
    rb_ary_store(o, i, SWIG_From_int($1.elements[i]));
  queue_free(&$1);
  $result = o;
}
%typemap(arginit) Queue {
  queue_init(&$1);
}
%typemap(freearg) Queue {
  queue_free(&$1);
}
#endif

#if defined(SWIGPERL)

# work around a swig bug
%{
#undef SWIG_CALLXS
#ifdef PERL_OBJECT 
#  define SWIG_CALLXS(_name) TOPMARK=MARK-PL_stack_base;_name(cv,pPerl) 
#else 
#  ifndef MULTIPLICITY 
#    define SWIG_CALLXS(_name) TOPMARK=MARK-PL_stack_base;_name(cv) 
#  else 
#    define SWIG_CALLXS(_name) TOPMARK=MARK-PL_stack_base;_name(PERL_GET_THX, cv) 
#  endif 
#endif 
%}


%define perliter(class)
  %perlcode {
    sub class##::FETCH {
      my $i = ${##class##::ITERATORS}{$_[0]};
      if ($i) {
        $_[1] == $i->[0] - 1 ? $i->[1] : undef;
      } else {
        $_[0]->__getitem__($_[1]);
      }
    }
    sub class##::FETCHSIZE {
      my $i = ${##class##::ITERATORS}{$_[0]};
      if ($i) {
        ($i->[1] = $_[0]->__next__()) ? ++$i->[0]  : 0;
      } else {
        $_[0]->__len__();
      }
    }
  }
%enddef

%{

#define SWIG_PERL_ITERATOR      0x80

SWIGRUNTIMEINLINE SV *
SWIG_Perl_NewArrayObj(SWIG_MAYBE_PERL_OBJECT void *ptr, swig_type_info *t, int flags) {
  SV *result = sv_newmortal();
  if (ptr && (flags & (SWIG_SHADOW | SWIG_POINTER_OWN))) {
    SV *self;
    SV *obj=newSV(0);
    AV *array=newAV();
    HV *stash;
    sv_setref_pv(obj, (char *) SWIG_Perl_TypeProxyName(t), ptr);
    stash=SvSTASH(SvRV(obj));
    if (flags & SWIG_POINTER_OWN) {
      HV *hv;
      GV *gv=*(GV**)hv_fetch(stash, "OWNER", 5, TRUE);
      if (!isGV(gv))
        gv_init(gv, stash, "OWNER", 5, FALSE);
      hv=GvHVn(gv);
      hv_store_ent(hv, obj, newSViv(1), 0);
    }
    if (flags & SWIG_PERL_ITERATOR) {
      HV *hv;
      GV *gv=*(GV**)hv_fetch(stash, "ITERATORS", 9, TRUE);
      AV *av=newAV();
      if (!isGV(gv))
        gv_init(gv, stash, "ITERATORS", 9, FALSE);
      hv=GvHVn(gv);
      hv_store_ent(hv, obj, newRV_inc((SV *)av), 0);
    }
    sv_magic((SV *)array, (SV *)obj, 'P', Nullch, 0);
    SvREFCNT_dec(obj);
    self=newRV_noinc((SV *)array);
    sv_setsv(result, self);
    SvREFCNT_dec((SV *)self);
    sv_bless(result, stash);
  } else {
    sv_setref_pv(result, (char *) SWIG_Perl_TypeProxyName(t), ptr);
  }
  return result;
}

%}

%typemap(out) Perlarray {
  ST(argvi) = SWIG_Perl_NewArrayObj(SWIG_PERL_OBJECT_CALL SWIG_as_voidptr(result), $1_descriptor, $owner | $shadow); argvi++;
}
%typemap(out) Perliterator {
  ST(argvi) = SWIG_Perl_NewArrayObj(SWIG_PERL_OBJECT_CALL SWIG_as_voidptr(result), $1_descriptor, $owner | $shadow | SWIG_PERL_ITERATOR); argvi++;
}

%typemap(out) Pool_solvable_iterator * = Perlarray;
%typemap(out) Pool_solvable_iterator * solvables_iter = Perliterator;
%typemap(out) Pool_repo_iterator * = Perlarray;
%typemap(out) Pool_repo_iterator * repos_iter = Perliterator;
%typemap(out) Repo_solvable_iterator * = Perlarray;
%typemap(out) Repo_solvable_iterator * solvables_iter = Perliterator;
%typemap(out) Dataiterator * = Perliterator;

#endif



#if defined(SWIGPYTHON)
typedef PyObject *AppObjectPtr;
%typemap(out) AppObjectPtr {
  $result = $1;
  Py_INCREF($result);
}
#endif
#if defined(SWIGPERL)
typedef SV *AppObjectPtr;
%typemap(in) AppObjectPtr {
  $1 = SvROK($input) ? SvRV($input) : 0;
}
%typemap(out) AppObjectPtr {
  $result = $1 ? newRV_inc($1) : newSV(0);
  argvi++;
}
#endif
#if defined(SWIGRUBY)
typedef VALUE AppObjectPtr;
%typemap(in) AppObjectPtr {
  $1 = (void *)$input;
}
%typemap(out) AppObjectPtr {
  $result = (VALUE)$1;
}
#endif


%include "cdata.i"
#ifndef SWIGPERL
%include "file.i"
#endif
%include "typemaps.i"

%{
#include "stdio.h"
#include "sys/stat.h"

#include "pool.h"
#include "solver.h"
#include "policy.h"
#include "solverdebug.h"
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

#define SOLVER_SOLUTION_ERASE -100
#define SOLVER_SOLUTION_REPLACE -101
typedef struct chksum Chksum;
typedef int bool;
typedef void *AppObjectPtr;

typedef struct {
  Pool *pool;
  Id id;
} XId;

typedef struct {
  Pool *pool;
  Id id;
} XSolvable;

typedef struct {
  Solver *solv;
  Id id;
} XRule;

typedef struct {
  Repo *repo;
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

typedef struct {
  Solver *solv;
  Id rid;
  Id type;
  Id source;
  Id target;
  Id dep;
} Ruleinfo;

typedef Dataiterator Datamatch;

%}

#ifdef SWIGRUBY
%mixin Dataiterator "Enumerable";
%mixin Pool_solvable_iterator "Enumerable";
%mixin Pool_repo_iterator "Enumerable";
%mixin Repo_solvable_iterator "Enumerable";
#endif

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
} XId;

typedef struct {
  Pool* const pool;
  Id const id;
} XSolvable;

%nodefaultctor Ruleinfo;
typedef struct {
  Solver* const solv;
  Id const type;
  Id const dep;
} Ruleinfo;

typedef struct {
  Solver* const solv;
  Id const id;
} XRule;

typedef struct {
  Repo* const repo;
  Id const id;
} XRepodata;

# put before pool/repo so we can access the constructor
%nodefaultdtor Dataiterator;
typedef struct {} Dataiterator;
typedef struct {} Pool_solvable_iterator;
typedef struct {} Pool_repo_iterator;
typedef struct {} Repo_solvable_iterator;

%nodefaultctor Datamatch;
%nodefaultdtor Datamatch;
typedef struct {
  Pool * const pool;
  Repo * const repo;
  const Id solvid;
} Datamatch;

typedef struct {
  Pool * const pool;
  Id how;
  Id what;
} Job;

%nodefaultctor Pool;
%nodefaultdtor Pool;
typedef struct {
  AppObjectPtr appdata;
} Pool;

%nodefaultctor Repo;
%nodefaultdtor Repo;
typedef struct _Repo {
  Pool * const pool;
  const char * const name;
  int priority;
  int subpriority;
  int const nsolvables;
  AppObjectPtr appdata;
} Repo;

%nodefaultctor Solver;
%nodefaultdtor Solver;
typedef struct {
  Pool * const pool;
  bool fixsystem;
  bool allowdowngrade;
  bool allowarchchange;
  bool allowvendorchange;
  bool allowuninstall;
  bool updatesystem;
  bool noupdateprovide;
  bool dosplitprovides;
  bool dontinstallrecommended;
  bool ignorealreadyrecommended;
  bool dontshowinstalledrecommended;
  bool distupgrade;
  bool distupgrade_removeunsupported;
  bool noinfarchcheck;
} Solver;

typedef struct chksum {
} Chksum;

%rename(xfopen) sat_xfopen;
%rename(xfopen_fd) sat_xfopen_fd;
%rename(xfopen_dup) sat_xfopen_dup;
%rename(xfclose) sat_xfclose;
%rename(xfileno) sat_xfileno;

FILE *sat_xfopen(const char *fn, const char *mode = 0);
FILE *sat_xfopen_fd(const char *fn, int fd, const char *mode = 0);
FILE *sat_xfopen_dup(const char *fn, int fd, const char *mode = 0);
int sat_xfclose(FILE *fp);
int sat_fileno(FILE *fp);

%{
  SWIGINTERN int sat_xfclose(FILE *fp) {
    return fclose(fp);
  }
  SWIGINTERN int sat_fileno(FILE *fp) {
    return fileno(fp);
  }
  SWIGINTERN FILE *sat_xfopen_dup(const char *fn, int fd, const char *mode) {
    fd = dup(fd);
    return fd == -1 ? 0 : sat_xfopen_fd(fn, fd, mode);
  }
%}

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

  const char *str() {
    return pool_job2str($self->pool, $self->how, $self->what, 0);
  }

  Queue solvableids() {
    Pool *pool = $self->pool;
    Id p, pp, how;
    Queue q;
    queue_init(&q);
    how = $self->how & SOLVER_SELECTMASK;
    FOR_JOB_SELECT(p, pp, how, $self->what)
      queue_push(&q, p);
    return q;
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def solvables(self, *args):
      return [ self.pool.solvables[id] for id in self.solvableids(*args) ]
  }
#endif
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Job::solvables {
      my ($self, @args) = @_;
      return map {$self->{'pool'}->{'solvables'}->[$_]} $self->solvableids(@args);
    }
  }
#endif
#if defined(SWIGRUBY)
%init %{
rb_eval_string(
    "class Solv::Job\n"
    "  def solvables\n"
    "    solvableids.collect do |id|\n"
    "      pool.solvables[id]\n"
    "    end\n"
    "  end\n"
    "end\n"
);
%}
#endif

}

%extend Chksum {
  Chksum(Id type) {
    return (Chksum *)sat_chksum_create(type);
  }
  Chksum(Id type, const char *hex) {
    unsigned char buf[64];
    int l = sat_chksum_len(type);
    if (!l)
      return 0;
    if (sat_hex2bin(&hex, buf, sizeof(buf)) != l || hex[0])
      return 0;
    return (Chksum *)sat_chksum_create_from_bin(type, buf);
  }
  ~Chksum() {
    sat_chksum_free($self, 0);
  }
  Id const type;
  %{
  SWIGINTERN Id Chksum_type_get(Chksum *chksum) {
    return sat_chksum_get_type(chksum);
  }
  %}
  void add(const char *str) {
    sat_chksum_add($self, str, strlen((char *)str));
  }
  void add_fp(FILE *fp) {
    char buf[4096];
    int l;
    while ((l = fread(buf, 1, sizeof(buf), fp)) > 0)
      sat_chksum_add($self, buf, l);
    rewind(fp);         /* convenience */
  }
  void add_fd(int fd) {
    char buf[4096];
    int l;
    while ((l = read(fd, buf, sizeof(buf))) > 0)
      sat_chksum_add($self, buf, l);
    lseek(fd, 0, 0);    /* convenience */
  }
  void add_stat(const char *filename) {
    struct stat stb;
    if (stat(filename, &stb))
      memset(&stb, 0, sizeof(stb));
    sat_chksum_add($self, &stb.st_dev, sizeof(stb.st_dev));
    sat_chksum_add($self, &stb.st_ino, sizeof(stb.st_ino));
    sat_chksum_add($self, &stb.st_size, sizeof(stb.st_size));
    sat_chksum_add($self, &stb.st_mtime, sizeof(stb.st_mtime));
  }
  bool matches(Chksum *othersum) {
    int l;
    const unsigned char *b, *bo;
    if (!othersum)
      return 0;
    if (sat_chksum_get_type($self) != sat_chksum_get_type(othersum))
      return 0;
    b = sat_chksum_get($self, &l);
    bo = sat_chksum_get(othersum, 0);
    return memcmp(b, bo, l) == 0;
  }
  SWIGCDATA raw() {
    int l;
    const unsigned char *b;
    b = sat_chksum_get($self, &l);
    return cdata_void((void *)b, l);
  }
  %newobject hex;
  char *hex() {
    int l;
    const unsigned char *b;
    char *ret, *rp;

    b = sat_chksum_get($self, &l);
    ret = sat_malloc(2 * l + 1);
    sat_bin2hex(b, l, ret);
    return ret;
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
    int ecode = 0;
    int vresult = 0;
    Py_DECREF(args);
    if (!result)
      return 0; /* exception */
    ecode = SWIG_AsVal_int(result, &vresult);
    Py_DECREF(result);
    return SWIG_IsOK(ecode) ? vresult : 0;
  }
  %}
  void set_loadcallback(PyObject *callable) {
    if ($self->loadcallback == loadcallback)
      Py_DECREF($self->loadcallbackdata);
    if (callable)
      Py_INCREF(callable);
    pool_setloadcallback($self, callable ? loadcallback : 0, callable);
  }
#endif
#if defined(SWIGPERL)
  %{

SWIGINTERN int loadcallback(Pool *pool, Repodata *data, void *d) {
  int count;
  int ret = 0;
  dSP;
  XRepodata *xd = new_XRepodata(data->repo, data - data->repo->repodata);

  ENTER;
  SAVETMPS;
  PUSHMARK(SP);
  XPUSHs(SWIG_NewPointerObj(SWIG_as_voidptr(xd), SWIGTYPE_p_XRepodata, SWIG_OWNER | SWIG_SHADOW));
  PUTBACK;
  count = perl_call_sv((SV *)d, G_EVAL|G_SCALAR);
  SPAGAIN;
  if (count)
    ret = POPi;
  PUTBACK;
  FREETMPS;
  LEAVE;
  return ret;
}

  %}
  void set_loadcallback(SV *callable) {
    if ($self->loadcallback == loadcallback)
      SvREFCNT_dec($self->loadcallbackdata);
    if (callable)
      SvREFCNT_inc(callable);
    pool_setloadcallback($self, callable ? loadcallback : 0, callable);
  }
#endif

  void free() {
#if defined(SWIGPYTHON) || defined(SWIGPERL)
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
  %newobject lookup_checksum;
  Chksum *lookup_checksum(Id entry, Id keyname) {
    Id type = 0;
    const unsigned char *b = pool_lookup_bin_checksum($self, entry, keyname, &type);
    return sat_chksum_create_from_bin(type, b);
  }

  %newobject Dataiterator;
  Dataiterator *Dataiterator(Id p, Id key, const char *match, int flags) {
    return new_Dataiterator($self, 0, p, key, match, flags);
  }
  const char *solvid2str(Id solvid) {
    return solvid2str($self, solvid);
  }
  void addfileprovides() {
    pool_addfileprovides($self);
  }
  Queue addfileprovides_ids() {
    Queue r;
    Id *addedfileprovides = 0;
    queue_init(&r);
    pool_addfileprovides_ids($self, $self->installed, &addedfileprovides);
    if (addedfileprovides) {
      for (; *addedfileprovides; addedfileprovides++)
        queue_push(&r, *addedfileprovides);
    }
    return r;
  }
  void createwhatprovides() {
    pool_createwhatprovides($self);
  }

  %newobject solvables;
  Pool_solvable_iterator * const solvables;
  %{
  SWIGINTERN Pool_solvable_iterator * Pool_solvables_get(Pool *pool) {
    return new_Pool_solvable_iterator(pool);
  }
  %}
  %newobject solvables_iter;
  Pool_solvable_iterator * solvables_iter() {
    return new_Pool_solvable_iterator($self);
  }

  %newobject repos;
  Pool_repo_iterator * const repos;
  %{
  SWIGINTERN Pool_repo_iterator * Pool_repos_get(Pool *pool) {
    return new_Pool_repo_iterator(pool);
  }
  %}
  %newobject repos_iter;
  Pool_repo_iterator * repos_iter() {
    return new_Pool_repo_iterator($self);
  }

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
  Queue matchprovidingids(const char *match, int flags) {
    Pool *pool = $self;
    Queue q;
    Id id;
    queue_init(&q);
    if (!flags) {
      for (id = 1; id < pool->ss.nstrings; id++)
        if (pool->whatprovides[id])
          queue_push(&q, id);
    } else {
      Datamatcher ma;
      if (!datamatcher_init(&ma, match, flags)) {
        for (id = 1; id < pool->ss.nstrings; id++)
          if (pool->whatprovides[id] && datamatcher_match(&ma, id2str(pool, id)))
            queue_push(&q, id);
        datamatcher_free(&ma);
      }
    }
    return q;
  }
  # move to job?
  Job *Job(Id how, Id what) {
    return new_Job($self, how, what);
  }

#if defined(SWIGPYTHON)
  %pythoncode {
    def providers(self, *args):
      return [ self.solvables[id] for id in self.providerids(*args) ]
  }
#endif
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Pool::providers {
      my ($self, @args) = @_;
      return map {$self->{'solvables'}->[$_]} $self->providerids(@args);
    }
  }
#endif
#if defined(SWIGRUBY)
%init %{
rb_eval_string(
    "class Solv::Pool\n"
    "  def providers(dep)\n"
    "    providerids(dep).collect do |id|\n"
    "      solvables[id]\n"
    "    end\n"
    "  end\n"
    "end\n"
  );
%}
#endif

  Id towhatprovides(Queue q) {
    return pool_queuetowhatprovides($self, &q);
  }
  bool isknownarch(Id id) {
    Pool *pool = $self;
    if (!id || id == ID_EMPTY)
      return 0;
    if (id == ARCH_SRC || id == ARCH_NOSRC || id == ARCH_NOARCH)
      return 1;
    if (pool->id2arch && (id > pool->lastarch || !pool->id2arch[id]))
      return 0;
    return 1;
  }

  %newobject Solver;
  Solver *Solver() {
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
  static const int SOLV_ADD_NO_STUBS = SOLV_ADD_NO_STUBS ; /* repo_solv */

  void free(int reuseids = 0) {
    repo_free($self, reuseids);
  }
  void empty(int reuseids = 0) {
    repo_empty($self, reuseids);
  }
#ifdef SWIGRUBY
  %rename("isempty?") isempty();
#endif
  bool isempty() {
    return !$self->nsolvables;
  }
  bool add_solv(const char *name, int flags = 0) {
    FILE *fp = fopen(name, "r");
    int r;
    if (!fp)
      return 0;
    r = repo_add_solv_flags($self, fp, flags);
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
  Id add_rpm(const char *name, int flags = 0) {
    return repo_add_rpm($self, name, flags);
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

  %newobject Dataiterator;
  Dataiterator *Dataiterator(Id p, Id key, const char *match, int flags) {
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
    return new_Repo_solvable_iterator(repo);
  }
  %}
  %newobject solvables_iter;
  Repo_solvable_iterator *solvables_iter() {
    return new_Repo_solvable_iterator($self);
  }

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
#ifdef SWIGRUBY
  %rename("iscontiguous?") iscontiguous();
#endif
  bool iscontiguous() {
    int i;
    for (i = $self->start; i < $self->end; i++)
      if ($self->pool->solvables[i].repo != $self)
        return 0;
    return 1;
  }
  XRepodata *first_repodata() {
     int i;
     if (!$self->nrepodata)
       return 0;
     /* make sure all repodatas but the first are extensions */
     if ($self->repodata[0].loadcallback)
        return 0;
     for (i = 1; i < $self->nrepodata; i++)
       if (!$self->repodata[i].loadcallback)
         return 0;       /* oops, not an extension */
     return new_XRepodata($self, 0);
   }
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
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Dataiterator *__iter__() {
    Dataiterator *ndi;
    ndi = sat_calloc(1, sizeof(*ndi));
    dataiterator_init_clone(ndi, $self);
    return ndi;
  }
  %rename("next") __next__();
  %exception __next__ {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
#endif

#ifdef SWIGPERL
  perliter(solv::Dataiterator)
#endif

  %newobject __next__;
  Datamatch *__next__() {
    Dataiterator *ndi;
    if (!dataiterator_step($self)) {
      return 0;
    }
    ndi = sat_calloc(1, sizeof(*ndi));
    dataiterator_init_clone(ndi, $self);
    return ndi;
  }
#ifdef SWIGRUBY
  void each() {
    Datamatch *d;
    while ((d = Dataiterator___next__($self)) != 0) {
      rb_yield(SWIG_NewPointerObj(SWIG_as_voidptr(d), SWIGTYPE_p_Datamatch, SWIG_POINTER_OWN | 0));
    }
  }
#endif
  void prepend_keyname(Id key) {
    dataiterator_prepend_keyname($self, key);
  }
  void skip_solvable() {
    dataiterator_skip_solvable($self);
  }
}

%extend Datamatch {
  ~Datamatch() {
    dataiterator_free($self);
    sat_free($self);
  }
  %newobject solvable;
  XSolvable * const solvable;
  %{
  SWIGINTERN XSolvable *Datamatch_solvable_get(Dataiterator *di) {
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
  void setpos_parent() {
    dataiterator_setpos_parent($self);
  }
}

%extend Pool_solvable_iterator {
  Pool_solvable_iterator(Pool *pool) {
    Pool_solvable_iterator *s;
    s = sat_calloc(1, sizeof(*s));
    s->pool = pool;
    return s;
  }
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Pool_solvable_iterator *__iter__() {
    Pool_solvable_iterator *s;
    s = sat_calloc(1, sizeof(*s));
    *s = *$self;
    return s;
  }
  %rename("next") __next__();
  %exception __next__ {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
#endif

#ifdef SWIGPERL
  perliter(solv::Pool_solvable_iterator)
#endif
  %newobject __next__;
  XSolvable *__next__() {
    Pool *pool = $self->pool;
    XSolvable *s;
    if ($self->id >= pool->nsolvables)
      return 0;
    while (++$self->id < pool->nsolvables)
      if (pool->solvables[$self->id].repo)
        return new_XSolvable(pool, $self->id);
    return 0;
  }
#ifdef SWIGRUBY
  void each() {
    XSolvable *n;
    while ((n = Pool_solvable_iterator___next__($self)) != 0) {
      rb_yield(SWIG_NewPointerObj(SWIG_as_voidptr(n), SWIGTYPE_p_XSolvable, SWIG_POINTER_OWN | 0));
    }
  }
#endif
  %newobject __getitem__;
  XSolvable *__getitem__(Id key) {
    Pool *pool = $self->pool;
    if (key > 0 && key < pool->nsolvables && pool->solvables[key].repo)
      return new_XSolvable(pool, key);
    return 0;
  }
  int __len__() {
    return $self->pool->nsolvables;
  }
}

%extend Pool_repo_iterator {
  Pool_repo_iterator(Pool *pool) {
    Pool_repo_iterator *s;
    s = sat_calloc(1, sizeof(*s));
    s->pool = pool;
    return s;
  }
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Pool_repo_iterator *__iter__() {
    Pool_repo_iterator *s;
    s = sat_calloc(1, sizeof(*s));
    *s = *$self;
    return s;
  }
  %rename("next") __next__();
  %exception __next__ {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
#endif
  %newobject __next__;
  Repo *__next__() {
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
#ifdef SWIGRUBY
  void each() {
    Repo *n;
    while ((n = Pool_repo_iterator___next__($self)) != 0) {
      rb_yield(SWIG_NewPointerObj(SWIG_as_voidptr(n), SWIGTYPE_p__Repo, SWIG_POINTER_OWN | 0));
    }
  }
#endif
  Repo *__getitem__(Id key) {
    Pool *pool = $self->pool;
    if (key > 0 && key < pool->nrepos + 1)
      return pool_id2repo(pool, key);
    return 0;
  }
  int __len__() {
    return $self->pool->nrepos + 1;
  }
}

%extend Repo_solvable_iterator {
  Repo_solvable_iterator(Repo *repo) {
    Repo_solvable_iterator *s;
    s = sat_calloc(1, sizeof(*s));
    s->repo = repo;
    return s;
  }
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Repo_solvable_iterator *__iter__() {
    Repo_solvable_iterator *s;
    s = sat_calloc(1, sizeof(*s));
    *s = *$self;
    return s;
  }
  %rename("next") __next__();
  %exception __next__ {
    $action
    if (!result) {
      PyErr_SetString(PyExc_StopIteration,"no more matches");
      return NULL;
    }
  }
#endif
  %newobject __next__;
  XSolvable *__next__() {
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
#ifdef SWIGRUBY
  void each() {
    XSolvable *n;
    while ((n = Repo_solvable_iterator___next__($self)) != 0) {
      rb_yield(SWIG_NewPointerObj(SWIG_as_voidptr(n), SWIGTYPE_p_XSolvable, SWIG_POINTER_OWN | 0));
    }
  }
#endif
  %newobject __getitem__;
  XSolvable *__getitem__(Id key) {
    Repo *repo = $self->repo;
    Pool *pool = repo->pool;
    if (key > 0 && key < pool->nsolvables && pool->solvables[key].repo == repo)
      return new_XSolvable(pool, key);
    return 0;
  }
  int __len__() {
    return $self->repo->pool->nsolvables;
  }
}

%extend XId {
  XId(Pool *pool, Id id) {
    XId *s;
    if (!id)
      return 0;
    s = sat_calloc(1, sizeof(*s));
    s->pool = pool;
    s->id = id;
    return s;
  }
  const char *str() {
    return dep2str($self->pool, $self->id);
  }
}

%extend XSolvable {
  XSolvable(Pool *pool, Id id) {
    XSolvable *s;
    if (!id)
      return 0;
    s = sat_calloc(1, sizeof(*s));
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
  %newobject lookup_checksum;
  Chksum *lookup_checksum(Id keyname) {
    Id type = 0;
    const unsigned char *b = pool_lookup_bin_checksum($self->pool, $self->id, keyname, &type);
    return sat_chksum_create_from_bin(type, b);
  }
  const char *lookup_location(int *OUTPUT) {
    return solvable_get_location($self->pool->solvables + $self->id, OUTPUT);
  }
#ifdef SWIGRUBY
  %rename("installable?") installable();
#endif
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
  const char * const vendor;
  %{
    SWIGINTERN const char *XSolvable_vendor_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return id2str(pool, pool->solvables[xs->id].vendor);
    }
  %}
  Id const vendorid;
  %{
    SWIGINTERN Id XSolvable_vendorid_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].vendor;
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
  %newobject findproblemrule;
  XRule *findproblemrule() {
    Id r = solver_findproblemrule($self->solv, $self->id);
    return new_XRule($self->solv, r);
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
    def findallproblemrules(self, unfiltered=0):
      return [ XRule(self.solv, i) for i in self.findallproblemrules_helper(unfiltered) ]
    def solutions(self):
      return [ Solution(self, i) for i in range(1, self.solution_count() + 1) ];
  }
#endif
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Problem::findallproblemrules {
      my ($self, $unfiltered) = @_;
      return map {solv::XRule->new($self->{'solv'}, $_)} $self->findallproblemrules_helper($unfiltered);
    }
    sub solv::Problem::solutions {
      my ($self) = @_;
      return map {solv::Solution->new($self, $_)} 1..($self->solution_count());
    }
  }
#endif
#if defined(SWIGRUBY)
%init %{
rb_eval_string(
    "class Solv::Problem\n"
    "  def solutions()\n"
    "    (1..solution_count).collect do |id| ; Solv::Solution.new(self,id) ; end\n"
    "  end\n"
    "end\n"
  );
%}
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
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Solution::elements {
      my ($self) = @_;
      return map {solv::Solutionelement->new($self, $_)} 1..($self->element_count());
    }
  }
#endif
#if defined(SWIGRUBY)
%init %{
rb_eval_string(
    "class Solv::Solution\n"
    "  def elements()\n"
    "    (1..element_count).collect do |id| ; Solv::Solutionelement.new(self,id) ; end\n"
    "  end\n"
    "end\n"
  );
%}
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
      e->type = e->rp ? SOLVER_SOLUTION_REPLACE : SOLVER_SOLUTION_ERASE;
    } else {
      e->type = e->p;
      e->p = e->rp;
      e->rp = 0;
    }
    return e;
  }
  int illegalreplace() {
    if ($self->type != SOLVER_SOLUTION_REPLACE || $self->p <= 0 || $self->rp <= 0)
      return 0;
    return policy_is_illegal($self->solv, $self->solv->pool->solvables + $self->p, $self->solv->pool->solvables + $self->rp, 0);
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
  %newobject Job;
  Job *Job() {
    if ($self->type == SOLVER_SOLUTION_INFARCH || $self->type == SOLVER_SOLUTION_DISTUPGRADE)
      return new_Job($self->solv->pool, SOLVER_INSTALL|SOLVER_SOLVABLE, $self->p);
    if ($self->type == SOLVER_SOLUTION_REPLACE)
      return new_Job($self->solv->pool, SOLVER_INSTALL|SOLVER_SOLVABLE, $self->rp);
    if ($self->type == SOLVER_SOLUTION_ERASE)
      return new_Job($self->solv->pool, SOLVER_ERASE|SOLVER_SOLVABLE, $self->p);
    return 0;
  }
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
  static const int SOLVER_SOLUTION_ERASE = SOLVER_SOLUTION_ERASE;
  static const int SOLVER_SOLUTION_REPLACE = SOLVER_SOLUTION_REPLACE;

  static const int POLICY_ILLEGAL_DOWNGRADE = POLICY_ILLEGAL_DOWNGRADE;
  static const int POLICY_ILLEGAL_ARCHCHANGE = POLICY_ILLEGAL_ARCHCHANGE;
  static const int POLICY_ILLEGAL_VENDORCHANGE = POLICY_ILLEGAL_VENDORCHANGE;

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
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Solver::solve {
      my ($self, $jobs) = @_;
      my @j = map {($_->{'how'}, $_->{'what'})} @$jobs;
      my $nprob = $self->solve_helper(\@j);
      return map {solv::Problem->new($self, $_)} 1..$nprob;
    }
  }
#endif
#if defined(SWIGRUBY)
%init %{
rb_eval_string(
    "class Solv::Solver\n"
    "  def solve(jobs)\n"
    "    jl = []\n"
    "    jobs.each do |j| ; jl << j.how << j.what ; end\n"
    "    nprob = solve_helper(jl)\n"
    "    (1..nprob).collect do |id| ; Solv::Problem.new(self,id) ; end\n"
    "  end\n"
    "end\n"
  );
%}
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
#ifdef SWIGRUBY
  %rename("isempty?") isempty();
#endif
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
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Transaction::classify {
      my ($self, $mode) = @_;
      $mode ||= 0;
      my @r = $self->classify_helper($mode);
      my @res;
      while (@r) {
        my ($type, $cnt, $fromid, $toid) = splice(@r, 0, 4);
        next if $type == $solv::Transaction::SOLVER_TRANSACTION_IGNORE;
        push @res, [$type, [ map {$self->{'pool'}->{'solvables'}->[$_]} $self->classify_pkgs_helper($mode, $type, $fromid, $toid) ], $fromid, $toid];
      }
      return @res;
    }
  }
#endif
  Queue newpackages_helper() {
    Queue q;
    int cut;
    queue_init(&q);
    cut = transaction_installedresult(self, &q);
    queue_truncate(&q, cut);
    return q;
  }
  Queue keptpackages_helper() {
    Queue q;
    int cut;
    queue_init(&q);
    cut = transaction_installedresult(self, &q);
    if (cut)
      queue_deleten(&q, 0, cut);
    return q;
  }
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
    def newpackages(self):
      return [ self.pool.solvables[i] for i in self.newpackages_helper() ]
    def keptpackages(self):
      return [ self.pool.solvables[i] for i in self.keptpackages_helper() ]
    def steps(self):
      return [ self.pool.solvables[i] for i in self.steps_helper() ]
  }
#endif
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Transaction::newpackages {
      my ($self) = @_;
      return map {$self->{'pool'}->{'solvables'}->[$_]} $self->newpackages_helper();
    }
    sub solv::Transaction::keptpackages {
      my ($self) = @_;
      return map {$self->{'pool'}->{'solvables'}->[$_]} $self->newpackages_helper();
    }
    sub solv::Transaction::steps {
      my ($self) = @_;
      return map {$self->{'pool'}->{'solvables'}->[$_]} $self->steps_helper();
    }
  }
#endif
  int calc_installsizechange() {
    return transaction_calc_installsizechange($self);
  }
  void order(int flags) {
    transaction_order($self, flags);
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
  Ruleinfo *info() {
    Ruleinfo *ri = sat_calloc(1, sizeof(*ri));
    ri->solv = $self->solv;
    ri->type = solver_ruleinfo($self->solv, $self->id, &ri->source, &ri->target, &ri->dep);
    return ri;
  }
}

%extend Ruleinfo {
  XSolvable * const solvable;
  XSolvable * const othersolvable;
  %{
    SWIGINTERN XSolvable *Ruleinfo_solvable_get(Ruleinfo *ri) {
      return new_XSolvable(ri->solv->pool, ri->source);
    }
    SWIGINTERN XSolvable *Ruleinfo_othersolvable_get(Ruleinfo *ri) {
      return new_XSolvable(ri->solv->pool, ri->target);
    }
  %}
  const char *problemstr() {
    return solver_problemruleinfo2str($self->solv, $self->type, $self->source, $self->target, $self->dep);
  }
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
  void set_bin_checksum(Id solvid, Id keyname, Chksum *chksum) {
    const unsigned char *buf = sat_chksum_get(chksum, 0);
    if (buf)
      repodata_set_bin_checksum($self->repo->repodata + $self->id, solvid, keyname, sat_chksum_get_type(chksum), buf);
  }
  const char *lookup_str(Id solvid, Id keyname) {
    return repodata_lookup_str($self->repo->repodata + $self->id, solvid, keyname);
  }
  Queue lookup_idarray(Id solvid, Id keyname) {
    Queue r;
    queue_init(&r);
    repodata_lookup_idarray($self->repo->repodata + $self->id, solvid, keyname, &r);
    return r;
  }
  %newobject lookup_checksum;
  Chksum *lookup_checksum(Id solvid, Id keyname) {
    Id type = 0;
    const unsigned char *b = repodata_lookup_bin_checksum($self->repo->repodata + $self->id, solvid, keyname, &type);
    return sat_chksum_create_from_bin(type, b);
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
  bool read_solv_flags(FILE *fp, int flags = 0) {
    Repodata *data = $self->repo->repodata + $self->id;
    int r, oldstate = data->state;
    data->state = REPODATA_LOADING;
    r = repo_add_solv_flags(data->repo, fp, flags | REPO_USE_LOADING);
    if (r)
      data->state = oldstate;
    return r;
  }
  void extend_to_repo() {
    Repodata *data = $self->repo->repodata + $self->id;
    repodata_extend_block(data, data->repo->start, data->repo->end - data->repo->start);
  }
}

