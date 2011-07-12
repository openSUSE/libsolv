SET (libsolvext_SRCS
    repo_content.c repo_deltainfoxml.c repo_helix.c repo_products.c
    repo_releasefile_products.c repo_repomdxml.c repo_rpmmd.c
    repo_susetags.c repo_updateinfoxml.c repo_write.c repo_zyppdb.c
    repo_deb.c solv_xfopen.c)
IF (NOT DEBIAN)
SET (libsolvext_SRCS ${libsolvext_SRCS}
    pool_fileconflicts.c repo_rpmdb.c)
ENDIF (NOT DEBIAN)

SET (libsolvext_HEADERS
    pool_fileconflicts.h repo_content.h repo_deltainfoxml.h repo_helix.h repo_products.h
    repo_releasefile_products.h repo_repomdxml.h repo_rpmdb.h repo_rpmmd.h
    repo_susetags.h repo_updateinfoxml.h repo_write.h repo_zyppdb.h
    tools_util.h repo_deb.h solv_xfopen.h)

SET (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
SET (CMAKE_SHARED_LINKER_FLAGS "${LINK_FLAGS} -Wl,--version-script=${CMAKE_SOURCE_DIR}/ext/libsolvext.ver")

IF (DISABLE_SHARED)
ADD_LIBRARY (libsolvext STATIC ${libsolvext_SRCS})
ELSE (DISABLE_SHARED)
ADD_LIBRARY (libsolvext SHARED ${libsolvext_SRCS})
ENDIF (DISABLE_SHARED)

SET_TARGET_PROPERTIES(libsolvext PROPERTIES OUTPUT_NAME "solvext")
SET_TARGET_PROPERTIES(libsolvext PROPERTIES SOVERSION ${LIBSOLVEXT_SOVERSION})

INSTALL (FILES ${libsolvext_HEADERS} DESTINATION "${CMAKE_INSTALL_PREFIX}/include/solv")
INSTALL (TARGETS libsolvext LIBRARY DESTINATION ${LIB_INSTALL_DIR} ARCHIVE DESTINATION ${LIB_INSTALL_DIR})

IF (ENABLE_STATIC AND NOT DISABLE_SHARED)
ADD_LIBRARY (libsolvext_static STATIC ${libsolvext_SRCS})
SET_TARGET_PROPERTIES(libsolvext_static PROPERTIES OUTPUT_NAME "solvext")
SET_TARGET_PROPERTIES(libsolvext_static PROPERTIES SOVERSION ${LIBSOLVEXT_SOVERSION})
INSTALL (TARGETS libsolvext_static LIBRARY DESTINATION ${LIB_INSTALL_DIR} ARCHIVE DESTINATION ${LIB_INSTALL_DIR})
ENDIF (ENABLE_STATIC AND NOT DISABLE_SHARED)
