noinst_LTLIBRARIES = libsolver.la
solvdir = @TCL_EXEC_PREFIX@/lib/tcl@TCL_MAJOR_VERSION@/@TCL_VERSION@

solv_DATA = solv-@VERSION@@TCL_SHLIB_SUFFIX@ solv-@VERSION@.tm
CLEANFILES = solv-@VERSION@@TCL_SHLIB_SUFFIX@

solv-@VERSION@@TCL_SHLIB_SUFFIX@: libsolver.la
	cp ".libs/$$($(GREP) dlname "$<" | $(AWK) -F "'" '{print $$2}')" $@

libsolver_la_SOURCES = solv_tcl.c
libsolver_la_CPPFLAGS = -I$(srcdir)/../../src -I../../src -I$(srcdir)/../../ext -I../../ext $(TCL_INCLUDE_SPEC)
libsolver_la_LDFLAGS = -avoid-version -shared -rpath `pwd` $(TCL_LIB_SPEC)
libsolver_la_LIBADD = ../../src/libsolv.la ../../ext/libsolvext.la

# Create Tcl module to load libsolv.
solv-@VERSION@.tm: solv.tm.in
	sed -e "s/__VERSION__/@VERSION@/g" $< >$@

solv_tcl_SWIGFLAGS = -namespace -pkgversion @VERSION@
solv_tcl.c: ../solv.i
if HAVE_SWIG
	$(SWIG) $(solv_tcl_SWIGFLAGS) -tcl -I$(srcdir)/../../src -o $@ $<
else
	@echo "Building ${SWIG_SRCS} requires swig, which is not installed." >&2
	@echo "Please install swig and re-run configure" >&2
	@exit 1
endif
