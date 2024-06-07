
INCLUDE (CheckFunctionExists)
CHECK_FUNCTION_EXISTS (qsort_r HAVE_QSORT_R)
CHECK_FUNCTION_EXISTS (__qsort_r HAVE___QSORT_R)

IF (HAVE_QSORT_R)
  ADD_DEFINITIONS (-DHAVE_QSORT_R=1)
ENDIF (HAVE_QSORT_R)

IF (HAVE___QSORT_R)
  ADD_DEFINITIONS (-DHAVE___QSORT_R=1)
ENDIF (HAVE___QSORT_R)

ADD_DEFINITIONS (-DLIBSOLV_INTERNAL=1)

SET (libsolv_SRCS
    bitmap.c poolarch.c poolvendor.c poolid.c strpool.c dirpool.c
    solver.c solverdebug.c repo_solv.c repo_write.c evr.c pool.c
    queue.c repo.c repodata.c repopage.c util.c policy.c solvable.c
    transaction.c order.c rules.c problems.c linkedpkg.c cplxdeps.c
    chksum.c md5.c sha1.c sha2.c solvversion.c selection.c
    fileprovides.c diskusage.c suse.c solver_util.c cleandeps.c
    userinstalled.c filelistfilter.c decision.c)

SET (libsolv_HEADERS
    bitmap.h evr.h hash.h policy.h poolarch.h poolvendor.h pool.h
    poolid.h pooltypes.h queue.h solvable.h solver.h solverdebug.h
    repo.h repodata.h repo_solv.h repo_write.h util.h selection.h
    strpool.h dirpool.h knownid.h transaction.h rules.h problems.h
    chksum.h dataiterator.h ${CMAKE_BINARY_DIR}/src/solvversion.h)

IF (ENABLE_CONDA)
    SET (libsolv_SRCS ${libsolv_SRCS} conda.c)
    SET (libsolv_HEADERS ${libsolv_HEADERS} conda.h)
ENDIF (ENABLE_CONDA)

IF (NOT MSVC)
    SET (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
ENDIF (NOT MSVC)

IF (WIN32)
    INCLUDE (${PROJECT_SOURCE_DIR}/win32/CMakeLists.txt)
    LIST (APPEND libsolv_SRCS ${WIN32_COMPAT_SOURCES})
ENDIF (WIN32)

IF (HAVE_LINKER_VERSION_SCRIPT)
SET (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINK_FLAGS} -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/libsolv.ver")
ENDIF (HAVE_LINKER_VERSION_SCRIPT)

IF (DISABLE_SHARED)
    ADD_LIBRARY (libsolv STATIC ${libsolv_SRCS})
ELSE (DISABLE_SHARED)
    ADD_LIBRARY (libsolv SHARED ${libsolv_SRCS})
ENDIF (DISABLE_SHARED)

IF (WIN32)
    IF (DISABLE_SHARED)
        TARGET_COMPILE_DEFINITIONS(libsolv PUBLIC SOLV_STATIC_LIB)
    ELSE (DISABLE_SHARED)
        TARGET_COMPILE_DEFINITIONS(libsolv PRIVATE SOLV_EXPORTS)
    ENDIF (DISABLE_SHARED)
ENDIF (WIN32)

SET_TARGET_PROPERTIES(libsolv PROPERTIES OUTPUT_NAME "solv")
if (MSVC AND DISABLE_SHARED AND ENABLE_STATIC)
    SET_TARGET_PROPERTIES(libsolv PROPERTIES OUTPUT_NAME "solv_static")
endif()

SET_TARGET_PROPERTIES(libsolv PROPERTIES SOVERSION ${LIBSOLV_SOVERSION})

INSTALL (FILES ${libsolv_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/solv")
INSTALL (TARGETS libsolv LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

IF (ENABLE_STATIC AND NOT DISABLE_SHARED)
ADD_LIBRARY (libsolv_static STATIC ${libsolv_SRCS})
SET_TARGET_PROPERTIES(libsolv_static PROPERTIES OUTPUT_NAME "solv")
SET_TARGET_PROPERTIES(libsolv_static PROPERTIES SOVERSION ${LIBSOLV_SOVERSION})
INSTALL (TARGETS libsolv_static LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
ENDIF (ENABLE_STATIC AND NOT DISABLE_SHARED)
