lib_LTLIBRARIES= libsatsolver.la
solverincludedir = $(includedir)/satsolver

solverinclude_HEADERS = \
        pooltypes.h     \
	hash.h          \
	bitmap.h        \
	evr.h           \
	poolid.h        \
	queue.h         \
	solvable.h      \
	source.h        \
	pool.h		\
	poolarch.h	\
	solver.h	\
	util.h		\
	source_solv.h

EXTRA_DIST = poolid_private.h

# As long as we build libsatsolver.la statically only we need to
# force PIC code here, as this lib will be included in libzypp.
libsatsolver_la_CFLAGS = -fPIC $(AM_CFLAGS)
libsatsolver_la_SOURCES = \
        bitmap.c        \
	evr.c           \
	poolid.c        \
	queue.c         \
	source.c        \
	pool.c		\
	poolarch.c	\
	solver.c	\
	util.c		\
	source_solv.c
