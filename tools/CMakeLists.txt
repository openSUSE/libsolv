#
# CMakeLists.txt for sat-solver/tools
#

ADD_LIBRARY (toolstuff STATIC common_write.c)

IF (NOT DEBIAN)
ADD_EXECUTABLE (rpmdb2solv rpmdb2solv.c)
TARGET_LINK_LIBRARIES (rpmdb2solv toolstuff libsolvext libsolv ${RPMDB_LIBRARY} ${EXPAT_LIBRARY})

ADD_EXECUTABLE (rpms2solv rpms2solv.c)
TARGET_LINK_LIBRARIES (rpms2solv toolstuff libsolvext libsolv ${RPMDB_LIBRARY})

ADD_EXECUTABLE (findfileconflicts findfileconflicts.c)
TARGET_LINK_LIBRARIES (findfileconflicts libsolvext libsolv ${RPMDB_LIBRARY})

ENDIF (NOT DEBIAN)

ADD_EXECUTABLE (rpmmd2solv rpmmd2solv.c)
TARGET_LINK_LIBRARIES (rpmmd2solv toolstuff libsolvext libsolv ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

ADD_EXECUTABLE (helix2solv helix2solv.c)
TARGET_LINK_LIBRARIES (helix2solv toolstuff libsolvext libsolv ${EXPAT_LIBRARY})

ADD_EXECUTABLE (susetags2solv susetags2solv.c)
TARGET_LINK_LIBRARIES (susetags2solv toolstuff libsolvext libsolv ${ZLIB_LIBRARY})

ADD_EXECUTABLE (updateinfoxml2solv updateinfoxml2solv.c)
TARGET_LINK_LIBRARIES (updateinfoxml2solv toolstuff libsolvext libsolv ${EXPAT_LIBRARY})

ADD_EXECUTABLE (deltainfoxml2solv deltainfoxml2solv.c)
TARGET_LINK_LIBRARIES (deltainfoxml2solv toolstuff libsolvext libsolv ${EXPAT_LIBRARY})

ADD_EXECUTABLE (repomdxml2solv repomdxml2solv.c)
TARGET_LINK_LIBRARIES (repomdxml2solv toolstuff libsolvext libsolv ${EXPAT_LIBRARY})

ADD_EXECUTABLE (installcheck installcheck.c)
TARGET_LINK_LIBRARIES (installcheck libsolvext libsolv ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

ADD_EXECUTABLE (patchcheck patchcheck.c)
TARGET_LINK_LIBRARIES (patchcheck libsolvext libsolv ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

ADD_EXECUTABLE (dumpsolv dumpsolv.c )
TARGET_LINK_LIBRARIES (dumpsolv libsolv)

ADD_EXECUTABLE (mergesolv mergesolv.c )
TARGET_LINK_LIBRARIES (mergesolv toolstuff libsolvext libsolv)

INSTALL (TARGETS
    mergesolv
    dumpsolv
    susetags2solv
    helix2solv
    rpmmd2solv
    updateinfoxml2solv
    deltainfoxml2solv
    repomdxml2solv
    installcheck
    DESTINATION ${BIN_INSTALL_DIR})

IF (NOT DEBIAN)
INSTALL (TARGETS
    rpmdb2solv
    rpms2solv
    DESTINATION ${BIN_INSTALL_DIR})
ENDIF (NOT DEBIAN)

INSTALL (PROGRAMS
   repo2solv.sh
   DESTINATION ${BIN_INSTALL_DIR})
