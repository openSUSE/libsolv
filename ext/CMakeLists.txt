SET (libsolvext_SRCS
    solv_xfopen.c testcase.c repo_testcase.c)

SET (libsolvext_HEADERS
    tools_util.h solv_xfopen.h testcase.h)

IF (ENABLE_RPMDB OR ENABLE_RPMPKG)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
        pool_fileconflicts.c repo_rpmdb.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
        pool_fileconflicts.h repo_rpmdb.h)
ENDIF (ENABLE_RPMDB OR ENABLE_RPMPKG)

IF (ENABLE_PUBKEY)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_pubkey.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_pubkey.h)
ENDIF (ENABLE_PUBKEY)

IF (ENABLE_PGPVRFY)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	solv_pgpvrfy.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	solv_pgpvrfy.h)
ENDIF (ENABLE_PGPVRFY)

IF (ENABLE_RPMMD)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_repomdxml.c repo_rpmmd.c
	repo_deltainfoxml.c repo_updateinfoxml.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_repomdxml.h repo_rpmmd.h
	repo_deltainfoxml.h repo_updateinfoxml.h)
ENDIF (ENABLE_RPMMD)

IF (ENABLE_SUSEREPO)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_content.c repo_products.c repo_releasefile_products.c
	repo_susetags.c repo_zyppdb.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_content.h repo_products.h repo_releasefile_products.h
	repo_susetags.h repo_zyppdb.h)
ENDIF (ENABLE_SUSEREPO)

# old cmake does not support parenthetical expressions...
IF (ENABLE_COMPLEX_DEPS)
IF (ENABLE_SUSEREPO OR ENABLE_RPMMD OR ENABLE_RPMDB OR ENABLE_RPMPKG)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	pool_parserpmrichdep.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	pool_parserpmrichdep.h)
ENDIF (ENABLE_SUSEREPO OR ENABLE_RPMMD OR ENABLE_RPMDB OR ENABLE_RPMPKG)
ENDIF (ENABLE_COMPLEX_DEPS)

IF (SUSE)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_autopattern.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_autopattern.h)
ENDIF (SUSE)

IF (ENABLE_COMPS)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_comps.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_comps.h)
ENDIF (ENABLE_COMPS)

IF (ENABLE_DEBIAN)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_deb.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_deb.h)
ENDIF (ENABLE_DEBIAN)

IF (ENABLE_APK)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_apk.c repo_apkv3.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_apk.h)
ENDIF (ENABLE_APK)

IF (ENABLE_HELIXREPO)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_helix.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_helix.h)
ENDIF (ENABLE_HELIXREPO)

IF (ENABLE_MDKREPO)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_mdk.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_mdk.h)
ENDIF (ENABLE_MDKREPO)

IF (ENABLE_ARCHREPO)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_arch.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_arch.h)
ENDIF (ENABLE_ARCHREPO)

IF (ENABLE_CUDFREPO)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_cudf.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_cudf.h)
ENDIF (ENABLE_CUDFREPO)

IF (ENABLE_HAIKU)
    enable_language(CXX)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_haiku.cpp)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_haiku.h)
ENDIF (ENABLE_HAIKU)

IF (ENABLE_APPDATA)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_appdata.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_appdata.h)
ENDIF (ENABLE_APPDATA)

IF (ENABLE_CONDA)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repo_conda.c)
    SET (libsolvext_HEADERS ${libsolvext_HEADERS}
	repo_conda.h)
ENDIF (ENABLE_CONDA)

IF (ENABLE_RPMMD OR ENABLE_SUSEREPO)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	repodata_diskusage.c)
ENDIF (ENABLE_RPMMD OR ENABLE_SUSEREPO)

IF (ENABLE_RPMMD OR ENABLE_SUSEREPO OR ENABLE_APPDATA OR ENABLE_COMPS OR ENABLE_HELIXREPO OR ENABLE_MDKREPO)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	solv_xmlparser.c)
ENDIF (ENABLE_RPMMD OR ENABLE_SUSEREPO OR ENABLE_APPDATA OR ENABLE_COMPS OR ENABLE_HELIXREPO OR ENABLE_MDKREPO)

IF (ENABLE_CONDA)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	solv_jsonparser.c)
ENDIF (ENABLE_CONDA)

IF (ENABLE_ZCHUNK_COMPRESSION)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	solv_zchunk.c)
ENDIF (ENABLE_ZCHUNK_COMPRESSION)

IF (ENABLE_ARCHREPO OR ENABLE_APK)
    SET (libsolvext_SRCS ${libsolvext_SRCS}
	tarhead.c)
ENDIF (ENABLE_ARCHREPO OR ENABLE_APK)

IF (NOT MSVC)
SET (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
ENDIF (NOT MSVC)
IF (HAVE_LINKER_VERSION_SCRIPT)
SET (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINK_FLAGS} -Wl,--version-script=${CMAKE_SOURCE_DIR}/ext/libsolvext.ver")
ENDIF (HAVE_LINKER_VERSION_SCRIPT)

IF (DISABLE_SHARED)
ADD_LIBRARY (libsolvext STATIC ${libsolvext_SRCS})
ELSE (DISABLE_SHARED)
ADD_LIBRARY (libsolvext SHARED ${libsolvext_SRCS})
TARGET_LINK_LIBRARIES(libsolvext libsolv ${SYSTEM_LIBRARIES})
ENDIF (DISABLE_SHARED)

IF (WIN32)
  INCLUDE_DIRECTORIES (${PROJECT_SOURCE_DIR}/win32/)
ENDIF ()

SET_TARGET_PROPERTIES(libsolvext PROPERTIES OUTPUT_NAME "solvext")
if (MSVC AND DISABLE_SHARED AND ENABLE_STATIC)
    SET_TARGET_PROPERTIES(libsolvext PROPERTIES OUTPUT_NAME "solvext_static")
endif()
SET_TARGET_PROPERTIES(libsolvext PROPERTIES SOVERSION ${LIBSOLVEXT_SOVERSION})

INSTALL (FILES ${libsolvext_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/solv")
INSTALL (TARGETS libsolvext LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

IF (ENABLE_STATIC AND NOT DISABLE_SHARED)
ADD_LIBRARY (libsolvext_static STATIC ${libsolvext_SRCS})
SET_TARGET_PROPERTIES(libsolvext_static PROPERTIES OUTPUT_NAME "solvext")
SET_TARGET_PROPERTIES(libsolvext_static PROPERTIES SOVERSION ${LIBSOLVEXT_SOVERSION})
INSTALL (TARGETS libsolvext_static LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
ENDIF (ENABLE_STATIC AND NOT DISABLE_SHARED)
