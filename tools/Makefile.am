noinst_PROGRAMS = rpmdb2solv rpmmd2solv helix2solv susetags2solv patchxml2solv dumpsolv mergesolv content2solv

INCLUDES =  \
	-I$(top_srcdir)/src

LDADD = \
	$(top_builddir)/src/libsatsolver.la

noinst_HEADERS = source_write.h

rpmdb2solv_SOURCES = \
	rpmdb2solv.c	\
	source_rpmdb.h	\
	source_rpmdb.c	\
	source_write.c

rpmdb2solv_LDADD = -ldb-4.3 $(LDADD)

rpmmd2solv_SOURCES = \
	rpmmd2solv.c	\
	source_rpmmd.h	\
	source_rpmmd.c	\
	source_write.c

rpmmd2solv_LDADD = -lexpat $(LDADD)

helix2solv_SOURCES = \
	helix2solv.c	\
	source_helix.h	\
	source_helix.c	\
	source_write.c

helix2solv_LDADD = -lexpat $(LDADD)

susetags2solv_SOURCES = \
	susetags2solv.c		\
	source_susetags.h	\
	source_susetags.c	\
	source_write.c

patchxml2solv_SOURCES = \
	patchxml2solv.c		\
	source_patchxml.h	\
	source_patchxml.c	\
	source_write.c

patchxml2solv_LDADD = -lexpat $(LDADD)

content2solv_SOURCES = \
	content2solv.c		\
	source_content.h	\
	source_content.c	\
	source_write.c

dumpsolv_SOURCES = \
	dumpsolv.c

mergesolv_SOURCES = \
	mergesolv.c \
	source_write.c
