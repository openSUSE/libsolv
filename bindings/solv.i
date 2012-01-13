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

#ifdef SWIGRUBY
%markfunc Pool "mark_Pool";
#endif

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

%define Queue2Array(type, step, con) %{
  int i;
  int cnt = $1.count / step;
  Id *idp = $1.elements;
  PyObject *o = PyList_New(cnt);
  for (i = 0; i < cnt; i++, idp += step)
    {
      Id id = *idp;
#define result resultx
      type result = con;
      $typemap(out, type)
      PyList_SetItem(o, i, $result);
#undef result
    }
  queue_free(&$1);
  $result = o;
%}

%enddef

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
%define Queue2Array(type, step, con) %{
  int i;
  int cnt = $1.count / step;
  Id *idp = $1.elements;
  if (argvi + cnt + 1 >= items) {
    EXTEND(sp, items - (argvi + cnt + 1) + 1);
  }
  for (i = 0; i < cnt; i++, idp += step)
    {
      Id id = *idp;
#define result resultx
      type result = con;
      $typemap(out, type)
      SvREFCNT_inc(ST(argvi - 1));
#undef result
    }
  queue_free(&$1);
  $result = 0;
%}
%enddef

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
      {
        SWIG_Error(SWIG_RuntimeError, "list must contain only integers");
        SWIG_fail;
      }
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
%define Queue2Array(type, step, con) %{
  int i;
  int cnt = $1.count / step;
  Id *idp = $1.elements;
  VALUE o = rb_ary_new2(cnt);
  for (i = 0; i < cnt; i++, idp += step)
    {
      Id id = *idp;
#define result resultx
      type result = con;
      $typemap(out, type)
      rb_ary_store(o, i, $result);
#undef result
    }
  queue_free(&$1);
  $result = o;
%}
%enddef
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
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "pool.h"
#include "poolarch.h"
#include "solver.h"
#include "policy.h"
#include "solverdebug.h"
#include "repo_solv.h"
#include "chksum.h"

#ifndef DEBIAN
#include "repo_rpmdb.h"
#endif
#include "repo_deb.h"
#include "repo_rpmmd.h"
#include "repo_write.h"
#include "repo_products.h"
#include "repo_susetags.h"
#include "repo_updateinfoxml.h"
#include "repo_deltainfoxml.h"
#include "repo_repomdxml.h"
#include "repo_content.h"
#include "solv_xfopen.h"

#define true 1
#define false 1

#define SOLVER_SOLUTION_ERASE                   -100
#define SOLVER_SOLUTION_REPLACE                 -101
#define SOLVER_SOLUTION_REPLACE_DOWNGRADE       -102
#define SOLVER_SOLUTION_REPLACE_ARCHCHANGE      -103
#define SOLVER_SOLUTION_REPLACE_VENDORCHANGE    -104

typedef struct chksum Chksum;
typedef int bool;
typedef void *AppObjectPtr;

typedef struct {
  Pool *pool;
  Id id;
} Dep;

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
  int how;
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

typedef struct {
  Transaction *transaction;
  int mode;
  Id type;
  int count;
  Id fromid;
  Id toid;
} TransactionClass;

typedef Dataiterator Datamatch;

%}

#ifdef SWIGRUBY
%mixin Dataiterator "Enumerable";
%mixin Pool_solvable_iterator "Enumerable";
%mixin Pool_repo_iterator "Enumerable";
%mixin Repo_solvable_iterator "Enumerable";
#endif

typedef int Id;
typedef unsigned int Offset;

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
} Dep;

typedef struct {
  Pool* const pool;
  Id const id;
} XSolvable;

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
  Id const solvid;
} Datamatch;

%nodefaultctor Datapos;
typedef struct {
  Repo * const repo;
} Datapos;

typedef struct {
  Pool * const pool;
  int how;
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
} Solver;

typedef struct chksum {
} Chksum;

%rename(xfopen) solv_xfopen;
%rename(xfopen_fd) solv_xfopen_fd;
%rename(xfopen_dup) solv_xfopen_dup;
%rename(xfclose) solv_xfclose;
%rename(xfileno) solv_xfileno;

FILE *solv_xfopen(const char *fn, const char *mode = 0);
FILE *solv_xfopen_fd(const char *fn, int fd, const char *mode = 0);
FILE *solv_xfopen_dup(const char *fn, int fd, const char *mode = 0);
int solv_xfclose(FILE *fp);
int solv_xfileno(FILE *fp);

%{
  SWIGINTERN int solv_xfclose(FILE *fp) {
    return fclose(fp);
  }
  SWIGINTERN int solv_xfileno(FILE *fp) {
    return fileno(fp);
  }
  SWIGINTERN FILE *solv_xfopen_dup(const char *fn, int fd, const char *mode) {
    fd = dup(fd);
    return fd == -1 ? 0 : solv_xfopen_fd(fn, fd, mode);
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

typedef struct {
  Transaction * const transaction;
  Id const type;
  Id const fromid;
  Id const toid;
  int const count;
} TransactionClass;


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

  Job(Pool *pool, int how, Id what) {
    Job *job = solv_calloc(1, sizeof(*job));
    job->pool = pool;
    job->how = how;
    job->what = what;
    return job;
  }

  %typemap(out) Queue solvables Queue2Array(XSolvable *, 1, new_XSolvable(arg1->pool, id));
  %newobject solvables;
  Queue solvables() {
    Pool *pool = $self->pool;
    Id p, pp, how;
    Queue q;
    queue_init(&q);
    how = $self->how & SOLVER_SELECTMASK;
    FOR_JOB_SELECT(p, pp, how, $self->what)
      queue_push(&q, p);
    return q;
  }

  bool __eq__(Job *j) {
    return $self->pool == j->pool && $self->how == j->how && $self->what == j->what;
  }
  bool __ne__(Job *j) {
    return !Job___eq__($self, j);
  }
#if defined(SWIGPERL)
  %rename("str") __str__;
#endif
  const char *__str__() {
    return pool_job2str($self->pool, $self->how, $self->what, 0);
  }
  const char *__repr__() {
    const char *str = pool_job2str($self->pool, $self->how, $self->what, ~0);
    return pool_tmpjoin($self->pool, "<Job ", str, ">");
  }
}

%extend Chksum {
  Chksum(Id type) {
    return (Chksum *)solv_chksum_create(type);
  }
  Chksum(Id type, const char *hex) {
    unsigned char buf[64];
    int l = solv_chksum_len(type);
    if (!l)
      return 0;
    if (solv_hex2bin(&hex, buf, sizeof(buf)) != l || hex[0])
      return 0;
    return (Chksum *)solv_chksum_create_from_bin(type, buf);
  }
  ~Chksum() {
    solv_chksum_free($self, 0);
  }
  Id const type;
  %{
  SWIGINTERN Id Chksum_type_get(Chksum *chksum) {
    return solv_chksum_get_type(chksum);
  }
  %}
  void add(const char *str) {
    solv_chksum_add($self, str, strlen((char *)str));
  }
  void add_fp(FILE *fp) {
    char buf[4096];
    int l;
    while ((l = fread(buf, 1, sizeof(buf), fp)) > 0)
      solv_chksum_add($self, buf, l);
    rewind(fp);         /* convenience */
  }
  void add_fd(int fd) {
    char buf[4096];
    int l;
    while ((l = read(fd, buf, sizeof(buf))) > 0)
      solv_chksum_add($self, buf, l);
    lseek(fd, 0, 0);    /* convenience */
  }
  void add_stat(const char *filename) {
    struct stat stb;
    if (stat(filename, &stb))
      memset(&stb, 0, sizeof(stb));
    solv_chksum_add($self, &stb.st_dev, sizeof(stb.st_dev));
    solv_chksum_add($self, &stb.st_ino, sizeof(stb.st_ino));
    solv_chksum_add($self, &stb.st_size, sizeof(stb.st_size));
    solv_chksum_add($self, &stb.st_mtime, sizeof(stb.st_mtime));
  }
  SWIGCDATA raw() {
    int l;
    const unsigned char *b;
    b = solv_chksum_get($self, &l);
    return cdata_void((void *)b, l);
  }
  %newobject hex;
  char *hex() {
    int l;
    const unsigned char *b;
    char *ret;

    b = solv_chksum_get($self, &l);
    ret = solv_malloc(2 * l + 1);
    solv_bin2hex(b, l, ret);
    return ret;
  }

  bool __eq__(Chksum *chk) {
    int l;
    const unsigned char *b, *bo;
    if (!chk)
      return 0;
    if (solv_chksum_get_type($self) != solv_chksum_get_type(chk))
      return 0;
    b = solv_chksum_get($self, &l);
    bo = solv_chksum_get(chk, 0);
    return memcmp(b, bo, l) == 0;
  }
  bool __ne__(Chksum *chk) {
    return !Chksum___eq__($self, chk);
  }
#if defined(SWIGRUBY)
  %rename("to_s") __str__;
  %rename("inspect") __repr__;
#endif
#if defined(SWIGPERL)
  %rename("str") __str__;
#endif
  %newobject __str__;
  const char *__str__() {
    const char *str;
    const char *h = 0;
    if (solv_chksum_isfinished($self))
      h = Chksum_hex($self);
    str = solv_dupjoin(solv_chksum_type2str(solv_chksum_get_type($self)), ":", h ? h : "unfinished");
    solv_free((void *)h);
    return str;
  }
  %newobject __repr__;
  const char *__repr__() {
    const char *h = Chksum___str__($self);
    const char *str = solv_dupjoin("<Chksum ", h, ">");
    solv_free((void *)h);
    return str;
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
    if ($self->loadcallback == loadcallback) {
      Py_DECREF($self->loadcallbackdata);
    }
    if (callable) {
      Py_INCREF(callable);
    }
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

#if defined(SWIGRUBY)
%{
  SWIGINTERN int loadcallback(Pool *pool, Repodata *data, void *d) {
    XRepodata *xd = new_XRepodata(data->repo, data - data->repo->repodata);
    VALUE callable = (VALUE)d;
    VALUE rd = SWIG_NewPointerObj(SWIG_as_voidptr(xd), SWIGTYPE_p_XRepodata, SWIG_POINTER_OWN | 0);
    VALUE res = rb_funcall(callable, rb_intern("call"), 1, rd);
    return res == Qtrue;
  }
  SWIGINTERN void mark_Pool(void *ptr) {
    Pool *pool = ptr;
    if (pool->loadcallback == loadcallback && pool->loadcallbackdata) {
      VALUE callable = (VALUE)pool->loadcallbackdata;
      rb_gc_mark(callable);
    }
  }
%}
  %typemap(in, numinputs=0) VALUE callable {
    $1 = rb_block_given_p() ? rb_block_proc() : 0;
  }
  void set_loadcallback(VALUE callable) {
    pool_setloadcallback($self, callable ? loadcallback : 0, (void *)callable);
  }
#endif

  void free() {
    Pool_set_loadcallback($self, 0);
    pool_free($self);
  }
  Id str2id(const char *str, bool create=1) {
    return pool_str2id($self, str, create);
  }
  const char *id2str(Id id) {
    return pool_id2str($self, id);
  }
  const char *dep2str(Id id) {
    return pool_dep2str($self, id);
  }
  Id rel2id(Id name, Id evr, int flags, bool create=1) {
    return pool_rel2id($self, name, evr, flags, create);
  }
  Id id2langid(Id id, const char *lang, bool create=1) {
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
    return solv_chksum_create_from_bin(type, b);
  }

  %newobject Dataiterator;
  Dataiterator *Dataiterator(Id p, Id key, const char *match, int flags) {
    return new_Dataiterator($self, 0, p, key, match, flags);
  }
  const char *solvid2str(Id solvid) {
    return pool_solvid2str($self, solvid);
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

  XSolvable *id2solvable(Id id) {
    return new_XSolvable($self, id);
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

  Repo *id2repo(Id id) {
    if (id < 1 || id >= $self->nrepos)
      return 0;
    return pool_id2repo($self, id);
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
          if (pool->whatprovides[id] && datamatcher_match(&ma, pool_id2str(pool, id)))
            queue_push(&q, id);
        datamatcher_free(&ma);
      }
    }
    return q;
  }

  Job *Job(int how, Id what) {
    return new_Job($self, how, what);
  }

  %typemap(out) Queue whatprovides Queue2Array(XSolvable *, 1, new_XSolvable(arg1, id));
  %newobject whatprovides;
  Queue whatprovides(Id dep) {
    Pool *pool = $self;
    Queue q;
    Id p, pp;
    queue_init(&q);
    FOR_PROVIDES(p, pp, dep)
      queue_push(&q, p);
    return q;
  }

  Id towhatprovides(Queue q) {
    return pool_queuetowhatprovides($self, &q);
  }

#ifdef SWIGRUBY
  %rename("isknownarch?") isknownarch;
#endif
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

  void free(bool reuseids = 0) {
    repo_free($self, reuseids);
  }
  void empty(bool reuseids = 0) {
    repo_empty($self, reuseids);
  }
#ifdef SWIGRUBY
  %rename("isempty?") isempty;
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

  Id add_solvable() {
    return repo_add_solvable($self);
  }

  bool add_products(const char *proddir, int flags = 0) {
    repo_add_products($self, proddir, 0, flags);
    return 1;
  }
  bool add_rpmmd(FILE *fp, const char *language, int flags = 0) {
    repo_add_rpmmd($self, fp, language, flags);
    return 1;
  }
#ifndef DEBIAN
  bool add_rpmdb(Repo *ref, int flags = 0) {
    repo_add_rpmdb($self, ref, 0, flags);
    return 1;
  }
  Id add_rpm(const char *name, int flags = 0) {
    return repo_add_rpm($self, name, flags);
  }
#endif
  bool add_debdb(int flags = 0) {
    repo_add_debdb($self, 0, flags);
    return 1;
  }
  Id add_deb(const char *name, int flags = 0) {
    return repo_add_deb($self, name, flags);
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
  bool write_first_repodata(FILE *fp) {
    int oldnrepodata = $self->nrepodata;
    $self->nrepodata = oldnrepodata > 2 ? 2 : oldnrepodata;
    repo_write($self, fp, repo_write_stdkeyfilter, 0, 0);
    $self->nrepodata = oldnrepodata;
    return 1;
  }

  Offset addid_dep(Offset olddeps, Id id, Id marker) {
    return repo_addid_dep($self, olddeps, id, marker);
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
  %rename("iscontiguous?") iscontiguous;
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
     if ($self->nrepodata < 2)
       return 0;
     /* make sure all repodatas but the first are extensions */
     if ($self->repodata[1].loadcallback)
        return 0;
     for (i = 2; i < $self->nrepodata; i++)
       if (!$self->repodata[i].loadcallback)
         return 0;       /* oops, not an extension */
     return new_XRepodata($self, 1);
   }

  bool __eq__(Repo *repo) {
    return $self == repo;
  }
  bool __ne__(Repo *repo) {
    return $self != repo;
  }
#if defined(SWIGPERL)
  %rename("str") __str__;
#endif
  %newobject __str__;
  const char *__str__() {
    char buf[20];
    if ($self->name)
      return solv_strdup($self->name);
    sprintf(buf, "Repo#%d", $self->repoid);
    return solv_strdup(buf);
  }
  %newobject __repr__;
  const char *__repr__() {
    char buf[20];
    if ($self->name)
      {
        sprintf(buf, "<Repo #%d ", $self->repoid);
        return solv_dupjoin(buf, $self->name, ">");
      }
    sprintf(buf, "<Repo #%d>", $self->repoid);
    return solv_strdup(buf);
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
    Dataiterator *di = solv_calloc(1, sizeof(*di));
    dataiterator_init(di, pool, repo, p, key, match, flags);
    return di;
  }
  ~Dataiterator() {
    dataiterator_free($self);
    solv_free($self);
  }
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Dataiterator *__iter__() {
    Dataiterator *ndi;
    ndi = solv_calloc(1, sizeof(*ndi));
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
    ndi = solv_calloc(1, sizeof(*ndi));
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
    solv_free($self);
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
    return pool_id2str($self->pool, $self->key->name);
  }
  Id type_id() {
    return $self->key->type;
  }
  const char *type_idstr() {
    return pool_id2str($self->pool, $self->key->type);
  }
  Id id() {
     return $self->kv.id;
  }
  const char *idstr() {
     return pool_id2str($self->pool, $self->kv.id);
  }
  const char *str() {
     return $self->kv.str;
  }
  int num() {
     return $self->kv.num;
  }
  int num2() {
     return $self->kv.num2;
  }
  void setpos() {
    dataiterator_setpos($self);
  }
  void setpos_parent() {
    dataiterator_setpos_parent($self);
  }
}

%extend Pool_solvable_iterator {
  Pool_solvable_iterator(Pool *pool) {
    Pool_solvable_iterator *s;
    s = solv_calloc(1, sizeof(*s));
    s->pool = pool;
    return s;
  }
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Pool_solvable_iterator *__iter__() {
    Pool_solvable_iterator *s;
    s = solv_calloc(1, sizeof(*s));
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
    s = solv_calloc(1, sizeof(*s));
    s->pool = pool;
    return s;
  }
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Pool_repo_iterator *__iter__() {
    Pool_repo_iterator *s;
    s = solv_calloc(1, sizeof(*s));
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
    if ($self->id >= pool->nrepos)
      return 0;
    while (++$self->id < pool->nrepos) {
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
    if (key > 0 && key < pool->nrepos)
      return pool_id2repo(pool, key);
    return 0;
  }
  int __len__() {
    return $self->pool->nrepos;
  }
}

%extend Repo_solvable_iterator {
  Repo_solvable_iterator(Repo *repo) {
    Repo_solvable_iterator *s;
    s = solv_calloc(1, sizeof(*s));
    s->repo = repo;
    return s;
  }
#if defined(SWIGPYTHON)
  %newobject __iter__;
  Repo_solvable_iterator *__iter__() {
    Repo_solvable_iterator *s;
    s = solv_calloc(1, sizeof(*s));
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

%extend Dep {
  Dep(Pool *pool, Id id) {
    Dep *s;
    if (!id)
      return 0;
    s = solv_calloc(1, sizeof(*s));
    s->pool = pool;
    s->id = id;
    return s;
  }
  const char *str() {
    return pool_dep2str($self->pool, $self->id);
  }
  bool __eq__(Dep *s) {
    return $self->pool == s->pool && $self->id == s->id;
  }
  bool __ne__(Dep *s) {
    return !Dep___eq__($self, s);
  }
#if defined(SWIGPERL)
  %rename("str") __str__;
#endif
  const char *__str__() {
    return pool_dep2str($self->pool, $self->id);
  }
  %newobject __repr__;
  const char *__repr__() {
    char buf[20];
    sprintf(buf, "<Id #%d ", $self->id);
    return solv_dupjoin(buf, pool_dep2str($self->pool, $self->id), ">");
  }
}

%extend XSolvable {
  XSolvable(Pool *pool, Id id) {
    XSolvable *s;
    if (!id || id >= pool->nsolvables)
      return 0;
    s = solv_calloc(1, sizeof(*s));
    s->pool = pool;
    s->id = id;
    return s;
  }
  const char *str() {
    return pool_solvid2str($self->pool, $self->id);
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
    return solv_chksum_create_from_bin(type, b);
  }
  const char *lookup_location(unsigned int *OUTPUT) {
    return solvable_get_location($self->pool->solvables + $self->id, OUTPUT);
  }
#ifdef SWIGRUBY
  %rename("installable?") installable;
#endif
  bool installable() {
    return pool_installable($self->pool, pool_id2solvable($self->pool, $self->id));
  }
#ifdef SWIGRUBY
  %rename("isinstalled?") isinstalled;
#endif
  bool isinstalled() {
    Pool *pool = $self->pool;
    return pool->installed && pool_id2solvable(pool, $self->id)->repo == pool->installed;
  }

  const char *name;
  %{
    SWIGINTERN void XSolvable_name_set(XSolvable *xs, const char *name) {
      Pool *pool = xs->pool;
      pool->solvables[xs->id].name = pool_str2id(pool, name, /*create=*/1);
    }
    SWIGINTERN const char *XSolvable_name_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return pool_id2str(pool, pool->solvables[xs->id].name);
    }
  %}
  Id nameid;
  %{
    SWIGINTERN void XSolvable_nameid_set(XSolvable *xs, Id nameid) {
      xs->pool->solvables[xs->id].name = nameid;
    }
    SWIGINTERN Id XSolvable_nameid_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].name;
    }
  %}
  const char *evr;
  %{
    SWIGINTERN void XSolvable_evr_set(XSolvable *xs, const char *evr) {
      Pool *pool = xs->pool;
      pool->solvables[xs->id].evr = pool_str2id(pool, evr, /*create=*/1);
    }
    SWIGINTERN const char *XSolvable_evr_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return pool_id2str(pool, pool->solvables[xs->id].evr);
    }
  %}
  Id evrid;
  %{
    SWIGINTERN void XSolvable_evrid_set(XSolvable *xs, Id evrid) {
      xs->pool->solvables[xs->id].evr = evrid;
    }
    SWIGINTERN Id XSolvable_evrid_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].evr;
    }
  %}
  const char *arch;
  %{
    SWIGINTERN void XSolvable_arch_set(XSolvable *xs, const char *arch) {
      Pool *pool = xs->pool;
      pool->solvables[xs->id].arch = pool_str2id(pool, arch, /*create=*/1);
    }
    SWIGINTERN const char *XSolvable_arch_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return pool_id2str(pool, pool->solvables[xs->id].arch);
    }
  %}
  Id archid;
  %{
    SWIGINTERN void XSolvable_archid_set(XSolvable *xs, Id archid) {
      xs->pool->solvables[xs->id].arch = archid;
    }
    SWIGINTERN Id XSolvable_archid_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].arch;
    }
  %}
  const char *vendor;
  %{
    SWIGINTERN void XSolvable_vendor_set(XSolvable *xs, const char *vendor) {
      Pool *pool = xs->pool;
      pool->solvables[xs->id].vendor = pool_str2id(pool, vendor, /*create=*/1);
    }
    SWIGINTERN const char *XSolvable_vendor_get(XSolvable *xs) {
      Pool *pool = xs->pool;
      return pool_id2str(pool, pool->solvables[xs->id].vendor);
    }
  %}
  Id vendorid;
  %{
    SWIGINTERN void XSolvable_vendorid_set(XSolvable *xs, Id vendorid) {
      xs->pool->solvables[xs->id].vendor = vendorid;
    }
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

  Offset provides;
  %{
    SWIGINTERN void XSolvable_provides_set(XSolvable *xs, Offset provides) {
      xs->pool->solvables[xs->id].provides = provides;
    }
    SWIGINTERN Id XSolvable_provides_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].provides;
    }
  %}

  Offset obsoletes;
  %{
    SWIGINTERN void XSolvable_obsoletes_set(XSolvable *xs, Offset obsoletes) {
      xs->pool->solvables[xs->id].obsoletes = obsoletes;
    }
    SWIGINTERN Id XSolvable_obsoletes_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].obsoletes;
    }
  %}

  Offset conflicts;
  %{
    SWIGINTERN void XSolvable_conflicts_set(XSolvable *xs, Offset conflicts) {
      xs->pool->solvables[xs->id].conflicts = conflicts;
    }
    SWIGINTERN Id XSolvable_conflicts_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].conflicts;
    }
  %}

  Offset requires;
  %{
    SWIGINTERN void XSolvable_requires_set(XSolvable *xs, Offset requires) {
      xs->pool->solvables[xs->id].requires = requires;
    }
    SWIGINTERN Id XSolvable_requires_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].requires;
    }
  %}

  Offset recommends;
  %{
    SWIGINTERN void XSolvable_recommends_set(XSolvable *xs, Offset recommends) {
      xs->pool->solvables[xs->id].recommends = recommends;
    }
    SWIGINTERN Id XSolvable_recommends_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].recommends;
    }
  %}

  Offset suggests;
  %{
    SWIGINTERN void XSolvable_suggests_set(XSolvable *xs, Offset suggests) {
      xs->pool->solvables[xs->id].suggests = suggests;
    }
    SWIGINTERN Id XSolvable_suggests_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].suggests;
    }
  %}

  Offset supplements;
  %{
    SWIGINTERN void XSolvable_supplements_set(XSolvable *xs, Offset supplements) {
      xs->pool->solvables[xs->id].supplements = supplements;
    }
    SWIGINTERN Id XSolvable_supplements_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].supplements;
    }
  %}

  Offset enhances;
  %{
    SWIGINTERN void XSolvable_enhances_set(XSolvable *xs, Offset enhances) {
      xs->pool->solvables[xs->id].enhances = enhances;
    }
    SWIGINTERN Id XSolvable_enhances_get(XSolvable *xs) {
      return xs->pool->solvables[xs->id].enhances;
    }
  %}

  bool __eq__(XSolvable *s) {
    return $self->pool == s->pool && $self->id == s->id;
  }
  bool __ne__(XSolvable *s) {
    return !XSolvable___eq__($self, s);
  }
#if defined(SWIGPERL)
  %rename("str") __str__;
#endif
  const char *__str__() {
    return pool_solvid2str($self->pool, $self->id);
  }
  %newobject __repr__;
  const char *__repr__() {
    char buf[20];
    sprintf(buf, "<Solvable #%d ", $self->id);
    return solv_dupjoin(buf, pool_solvid2str($self->pool, $self->id), ">");
  }
}

%extend Problem {
  Problem(Solver *solv, Id id) {
    Problem *p;
    p = solv_calloc(1, sizeof(*p));
    p->solv = solv;
    p->id = id;
    return p;
  }
  %newobject findproblemrule;
  XRule *findproblemrule() {
    Id r = solver_findproblemrule($self->solv, $self->id);
    return new_XRule($self->solv, r);
  }
  %newobject findallproblemrules;
  %typemap(out) Queue findallproblemrules Queue2Array(XRule *, 1, new_XRule(arg1->solv, id));
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
            SolverRuleinfo rclass;
            probr = q.elements[i];
            rclass = solver_ruleclass(solv, probr);
            if (rclass == SOLVER_RULE_UPDATE || rclass == SOLVER_RULE_JOB)
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
  %newobject solutions;
  %typemap(out) Queue solutions Queue2Array(Solution *, 1, new_Solution(arg1, id));
  Queue solutions() {
    Queue q;
    int i, cnt;
    queue_init(&q);
    cnt = solver_solution_count($self->solv, $self->id);
    for (i = 1; i <= cnt; i++)
      queue_push(&q, i);
    return q;
  }
}

%extend Solution {
  Solution(Problem *p, Id id) {
    Solution *s;
    s = solv_calloc(1, sizeof(*s));
    s->solv = p->solv;
    s->problemid = p->id;
    s->id = id;
    return s;
  }
  int element_count() {
    return solver_solutionelement_count($self->solv, $self->problemid, $self->id);
  }

  %newobject elements;
  %typemap(out) Queue elements Queue2Array(Solutionelement *, 4, new_Solutionelement(arg1->solv, arg1->problemid, arg1->id, id, idp[1], idp[2], idp[3]));
  Queue elements(bool expandreplaces=0) {
    Queue q;
    int i, cnt;
    queue_init(&q);
    cnt = solver_solutionelement_count($self->solv, $self->problemid, $self->id);
    for (i = 1; i <= cnt; i++)
      {
        Id p, rp, type;
        solver_next_solutionelement($self->solv, $self->problemid, $self->id, i - 1, &p, &rp);
        if (p > 0) {
          type = rp ? SOLVER_SOLUTION_REPLACE : SOLVER_SOLUTION_ERASE;
        } else {
          type = p;
          p = rp;
          rp = 0;
        }
        if (type == SOLVER_SOLUTION_REPLACE && expandreplaces) {
          int illegal = policy_is_illegal(self->solv, self->solv->pool->solvables + p, self->solv->pool->solvables + rp, 0);
          if (illegal) {
            if ((illegal & POLICY_ILLEGAL_DOWNGRADE) != 0) {
              queue_push2(&q, i, SOLVER_SOLUTION_REPLACE_DOWNGRADE);
              queue_push2(&q, p, rp);
            }
            if ((illegal & POLICY_ILLEGAL_ARCHCHANGE) != 0) {
              queue_push2(&q, i, SOLVER_SOLUTION_REPLACE_ARCHCHANGE);
              queue_push2(&q, p, rp);
            }
            if ((illegal & POLICY_ILLEGAL_VENDORCHANGE) != 0) {
              queue_push2(&q, i, SOLVER_SOLUTION_REPLACE_VENDORCHANGE);
              queue_push2(&q, p, rp);
            }
            continue;
          }
        }
        queue_push2(&q, i, type);
        queue_push2(&q, p, rp);
      }
    return q;
  }
}

%extend Solutionelement {
  Solutionelement(Solver *solv, Id problemid, Id solutionid, Id id, Id type, Id p, Id rp) {
    Solutionelement *e;
    e = solv_calloc(1, sizeof(*e));
    e->solv = solv;
    e->problemid = problemid;
    e->solutionid = id;
    e->id = id;
    e->type = type;
    e->p = p;
    e->rp = rp;
    return e;
  }
  const char *str() {
    Id p = $self->type;
    Id rp = $self->p;
    if (p == SOLVER_SOLUTION_ERASE)
      {
        p = rp;
        rp = 0;
      }
    else if (p == SOLVER_SOLUTION_REPLACE)
      {
        p = rp;
        rp = $self->rp;
      }
    else if (p == SOLVER_SOLUTION_REPLACE_DOWNGRADE)
      return pool_tmpjoin($self->solv->pool, "allow ", policy_illegal2str($self->solv, POLICY_ILLEGAL_DOWNGRADE, $self->solv->pool->solvables + $self->p, $self->solv->pool->solvables + $self->rp), 0);
    else if (p == SOLVER_SOLUTION_REPLACE_ARCHCHANGE)
      return pool_tmpjoin($self->solv->pool, "allow ", policy_illegal2str($self->solv, POLICY_ILLEGAL_ARCHCHANGE, $self->solv->pool->solvables + $self->p, $self->solv->pool->solvables + $self->rp), 0);
    else if (p == SOLVER_SOLUTION_REPLACE_VENDORCHANGE)
      return pool_tmpjoin($self->solv->pool, "allow ", policy_illegal2str($self->solv, POLICY_ILLEGAL_VENDORCHANGE, $self->solv->pool->solvables + $self->p, $self->solv->pool->solvables + $self->rp), 0);
    return solver_solutionelement2str($self->solv, p, rp);
  }
  %newobject replaceelements;
  %typemap(out) Queue replaceelements Queue2Array(Solutionelement *, 1, new_Solutionelement(arg1->solv, arg1->problemid, arg1->solutionid, arg1->id, id, arg1->p, arg1->rp));
  Queue replaceelements() {
    Queue q;
    int illegal;

    queue_init(&q);
    if ($self->type != SOLVER_SOLUTION_REPLACE || $self->p <= 0 || $self->rp <= 0)
      illegal = 0;
    else
      illegal = policy_is_illegal($self->solv, $self->solv->pool->solvables + $self->p, $self->solv->pool->solvables + $self->rp, 0);
    if ((illegal & POLICY_ILLEGAL_DOWNGRADE) != 0)
      queue_push(&q, SOLVER_SOLUTION_REPLACE_DOWNGRADE);
    if ((illegal & POLICY_ILLEGAL_ARCHCHANGE) != 0)
      queue_push(&q, SOLVER_SOLUTION_REPLACE_ARCHCHANGE);
    if ((illegal & POLICY_ILLEGAL_VENDORCHANGE) != 0)
      queue_push(&q, SOLVER_SOLUTION_REPLACE_VENDORCHANGE);
    if (!q.count)
      queue_push(&q, $self->type);
    return q;
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
      if (e->type != SOLVER_SOLUTION_JOB)
        return -1;
      return (e->p - 1) / 2;
    }
  %}
  %newobject Job;
  Job *Job() {
    if ($self->type == SOLVER_SOLUTION_JOB)
      return new_Job($self->solv->pool, SOLVER_NOOP, 0);
    if ($self->type == SOLVER_SOLUTION_INFARCH || $self->type == SOLVER_SOLUTION_DISTUPGRADE)
      return new_Job($self->solv->pool, SOLVER_INSTALL|SOLVER_SOLVABLE, $self->p);
    if ($self->type == SOLVER_SOLUTION_REPLACE || $self->type == SOLVER_SOLUTION_REPLACE_DOWNGRADE || $self->type == SOLVER_SOLUTION_REPLACE_ARCHCHANGE || $self->type == SOLVER_SOLUTION_REPLACE_VENDORCHANGE)
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
  static const int SOLVER_SOLUTION_REPLACE_DOWNGRADE = SOLVER_SOLUTION_REPLACE_DOWNGRADE;
  static const int SOLVER_SOLUTION_REPLACE_ARCHCHANGE = SOLVER_SOLUTION_REPLACE_ARCHCHANGE;
  static const int SOLVER_SOLUTION_REPLACE_VENDORCHANGE = SOLVER_SOLUTION_REPLACE_VENDORCHANGE;

  static const int POLICY_ILLEGAL_DOWNGRADE = POLICY_ILLEGAL_DOWNGRADE;
  static const int POLICY_ILLEGAL_ARCHCHANGE = POLICY_ILLEGAL_ARCHCHANGE;
  static const int POLICY_ILLEGAL_VENDORCHANGE = POLICY_ILLEGAL_VENDORCHANGE;

  static const int SOLVER_FLAG_ALLOW_DOWNGRADE = SOLVER_FLAG_ALLOW_DOWNGRADE;
  static const int SOLVER_FLAG_ALLOW_ARCHCHANGE = SOLVER_FLAG_ALLOW_ARCHCHANGE;
  static const int SOLVER_FLAG_ALLOW_VENDORCHANGE = SOLVER_FLAG_ALLOW_VENDORCHANGE;
  static const int SOLVER_FLAG_ALLOW_UNINSTALL = SOLVER_FLAG_ALLOW_UNINSTALL;
  static const int SOLVER_FLAG_NO_UPDATEPROVIDE = SOLVER_FLAG_NO_UPDATEPROVIDE;
  static const int SOLVER_FLAG_SPLITPROVIDES = SOLVER_FLAG_SPLITPROVIDES;
  static const int SOLVER_FLAG_IGNORE_RECOMMENDED = SOLVER_FLAG_IGNORE_RECOMMENDED;
  static const int SOLVER_FLAG_IGNORE_ALREADY_RECOMMENDED = SOLVER_FLAG_IGNORE_ALREADY_RECOMMENDED;
  static const int SOLVER_FLAG_NO_INFARCHCHECK = SOLVER_FLAG_NO_INFARCHCHECK;

  ~Solver() {
    solver_free($self);
  }

  int set_flag(int flag, int value) {
    return solver_set_flag($self, flag, value);
  }
  int get_flag(int flag) {
    return solver_get_flag($self, flag);
  }
#if defined(SWIGPYTHON)
  %pythoncode {
    def solve(self, jobs):
      j = []
      for job in jobs: j += [job.how, job.what]
      return self.solve_helper(j)
  }
#endif
#if defined(SWIGPERL)
  %perlcode {
    sub solv::Solver::solve {
      my ($self, $jobs) = @_;
      my @j = map {($_->{'how'}, $_->{'what'})} @$jobs;
      return $self->solve_helper(\@j);
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
    "    solve_helper(jl)\n"
    "  end\n"
    "end\n"
  );
%}
#endif
  %typemap(out) Queue solve_helper Queue2Array(Problem *, 1, new_Problem(arg1, id));
  %newobject solve_helper;
  Queue solve_helper(Queue jobs) {
    Queue q;
    int i, cnt;
    queue_init(&q);
    solver_solve($self, &jobs);
    cnt = solver_problem_count($self);
    for (i = 1; i <= cnt; i++)
      queue_push(&q, i);
    return q;
  }
  %newobject transaction;
  Transaction *transaction() {
    return solver_create_transaction($self);
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
  }
#ifdef SWIGRUBY
  %rename("isempty?") isempty;
#endif
  bool isempty() {
    return $self->steps.count == 0;
  }

  %newobject othersolvable;
  XSolvable *othersolvable(XSolvable *s) {
    Id op = transaction_obs_pkg($self, s->id);
    return new_XSolvable($self->pool, op);
  }

  %newobject allothersolvables;
  %typemap(out) Queue allothersolvables Queue2Array(XSolvable *, 1, new_XSolvable(arg1->pool, id));
  Queue allothersolvables(XSolvable *s) {
    Queue q;
    queue_init(&q);
    transaction_all_obs_pkgs($self, s->id, &q);
    return q;
  }

  %typemap(out) Queue classify Queue2Array(TransactionClass *, 4, new_TransactionClass(arg1, arg2, id, idp[1], idp[2], idp[3]));
  %newobject classify;
  Queue classify(int mode = 0) {
    Queue q;
    queue_init(&q);
    transaction_classify($self, mode, &q);
    return q;
  }

  %typemap(out) Queue newpackages Queue2Array(XSolvable *, 1, new_XSolvable(arg1->pool, id));
  %newobject newpackages;
  Queue newpackages() {
    Queue q;
    int cut;
    queue_init(&q);
    cut = transaction_installedresult(self, &q);
    queue_truncate(&q, cut);
    return q;
  }

  %typemap(out) Queue keptpackages Queue2Array(XSolvable *, 1, new_XSolvable(arg1->pool, id));
  %newobject keptpackages;
  Queue keptpackages() {
    Queue q;
    int cut;
    queue_init(&q);
    cut = transaction_installedresult(self, &q);
    if (cut)
      queue_deleten(&q, 0, cut);
    return q;
  }

  %typemap(out) Queue steps Queue2Array(XSolvable *, 1, new_XSolvable(arg1->pool, id));
  %newobject steps ;
  Queue steps() {
    Queue q;
    queue_init_clone(&q, &$self->steps);
    return q;
  }

  int steptype(XSolvable *s, int mode) {
    return transaction_type($self, s->id, mode);
  }
  int calc_installsizechange() {
    return transaction_calc_installsizechange($self);
  }
  void order(int flags) {
    transaction_order($self, flags);
  }
}

%extend TransactionClass {
  TransactionClass(Transaction *trans, int mode, Id type, int count, Id fromid, Id toid) {
    TransactionClass *cl = solv_calloc(1, sizeof(*cl));
    cl->transaction = trans;
    cl->mode = mode;
    cl->type = type;
    cl->count = count;
    cl->fromid = fromid;
    cl->toid = toid;
    return cl;
  }
  %newobject solvables;
  %typemap(out) Queue solvables Queue2Array(XSolvable *, 1, new_XSolvable(arg1->transaction->pool, id));
  Queue solvables() {
    Queue q;
    queue_init(&q);
    transaction_classify_pkgs($self->transaction, $self->mode, $self->type, $self->fromid, $self->toid, &q);
    return q;
  }
}

%extend XRule {
  XRule(Solver *solv, Id id) {
    if (!id)
      return 0;
    XRule *xr = solv_calloc(1, sizeof(*xr));
    xr->solv = solv;
    xr->id = id;
    return xr;
  }
  Ruleinfo *info() {
    Id type, source, target, dep;
    type =  solver_ruleinfo($self->solv, $self->id, &source, &target, &dep);
    return new_Ruleinfo($self, type, source, target, dep);
  }
  %typemap(out) Queue allinfos Queue2Array(Ruleinfo *, 4, new_Ruleinfo(arg1, id, idp[1], idp[2], idp[3]));
  %newobject allinfos;
  Queue allinfos() {
    Queue q;
    queue_init(&q);
    solver_allruleinfos($self->solv, $self->id, &q);
    return q;
  }

  bool __eq__(XRule *xr) {
    return $self->solv == xr->solv && $self->id == xr->id;
  }
  bool __ne__(XRule *xr) {
    return !XRule___eq__($self, xr);
  }
  %newobject __repr__;
  const char *__repr__() {
    char buf[20];
    sprintf(buf, "<Rule #%d>", $self->id);
    return solv_strdup(buf);
  }
}

%extend Ruleinfo {
  Ruleinfo(XRule *r, Id type, Id source, Id target, Id dep) {
    Ruleinfo *ri = solv_calloc(1, sizeof(*ri));
    ri->solv = r->solv;
    ri->rid = r->id;
    ri->type = type;
    ri->source = source;
    ri->target = target;
    ri->dep = dep;
    return ri;
  }
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
    XRepodata *xr = solv_calloc(1, sizeof(*xr));
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
  void set_checksum(Id solvid, Id keyname, Chksum *chksum) {
    const unsigned char *buf = solv_chksum_get(chksum, 0);
    if (buf)
      repodata_set_bin_checksum($self->repo->repodata + $self->id, solvid, keyname, solv_chksum_get_type(chksum), buf);
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
    return solv_chksum_create_from_bin(type, b);
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
  bool add_solv(FILE *fp, int flags = 0) {
    Repodata *data = $self->repo->repodata + $self->id;
    int r, oldstate = data->state;
    data->state = REPODATA_LOADING;
    r = repo_add_solv_flags(data->repo, fp, flags | REPO_USE_LOADING);
    if (r || data->state == REPODATA_LOADING)
      data->state = oldstate;
    return r;
  }
  void extend_to_repo() {
    Repodata *data = $self->repo->repodata + $self->id;
    repodata_extend_block(data, data->repo->start, data->repo->end - data->repo->start);
  }
  bool __eq__(XRepodata *xr) {
    return $self->repo == xr->repo && $self->id == xr->id;
  }
  bool __ne__(XRepodata *xr) {
    return !XRepodata___eq__($self, xr);
  }
  %newobject __repr__;
  const char *__repr__() {
    char buf[20];
    sprintf(buf, "<Repodata #%d>", $self->id);
    return solv_strdup(buf);
  }
}

