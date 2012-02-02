#
# CMakeLists.txt for tools
#

ADD_LIBRARY (toolstuff STATIC common_write.c)

# as we link with --as-needed we do not get bogus dependencies
SET (SYSTEM_LIBRARIES ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})
IF (ENABLE_RPMDB)
SET (SYSTEM_LIBRARIES ${RPMDB_LIBRARY} ${SYSTEM_LIBRARIES})
ENDIF (ENABLE_RPMDB)

SET (tools_list mergesolv dumpsolv installcheck)

IF (ENABLE_RPMDB)
ADD_EXECUTABLE (rpmdb2solv rpmdb2solv.c)
TARGET_LINK_LIBRARIES (rpmdb2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (rpms2solv rpms2solv.c)
TARGET_LINK_LIBRARIES (rpms2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (findfileconflicts findfileconflicts.c)
TARGET_LINK_LIBRARIES (findfileconflicts libsolvext libsolv ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} rpmdb2solv rpms2solv)
ENDIF (ENABLE_RPMDB)

IF (ENABLE_RPMMD)
ADD_EXECUTABLE (repomdxml2solv repomdxml2solv.c)
TARGET_LINK_LIBRARIES (repomdxml2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (rpmmd2solv rpmmd2solv.c)
TARGET_LINK_LIBRARIES (rpmmd2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (updateinfoxml2solv updateinfoxml2solv.c)
TARGET_LINK_LIBRARIES (updateinfoxml2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (deltainfoxml2solv deltainfoxml2solv.c)
TARGET_LINK_LIBRARIES (deltainfoxml2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} repomdxml2solv rpmmd2solv updateinfoxml2solv deltainfoxml2solv)
ENDIF (ENABLE_RPMMD)

IF (ENABLE_HELIXREPO)
ADD_EXECUTABLE (helix2solv helix2solv.c)
TARGET_LINK_LIBRARIES (helix2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} helix2solv)
ENDIF (ENABLE_HELIXREPO)

IF (ENABLE_SUSEREPO)
ADD_EXECUTABLE (susetags2solv susetags2solv.c)
TARGET_LINK_LIBRARIES (susetags2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} susetags2solv)
ENDIF (ENABLE_SUSEREPO)

IF (ENABLE_COMPS)
ADD_EXECUTABLE (comps2solv comps2solv.c)
TARGET_LINK_LIBRARIES (comps2solv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} comps2solv)
ENDIF (ENABLE_COMPS)

ADD_EXECUTABLE (installcheck installcheck.c)
TARGET_LINK_LIBRARIES (installcheck libsolvext libsolv ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (patchcheck patchcheck.c)
TARGET_LINK_LIBRARIES (patchcheck libsolvext libsolv ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (dumpsolv dumpsolv.c )
TARGET_LINK_LIBRARIES (dumpsolv libsolv)

ADD_EXECUTABLE (mergesolv mergesolv.c )
TARGET_LINK_LIBRARIES (mergesolv toolstuff libsolvext libsolv ${SYSTEM_LIBRARIES})

INSTALL (TARGETS ${tools_list} DESTINATION ${BIN_INSTALL_DIR})

INSTALL (PROGRAMS repo2solv.sh DESTINATION ${BIN_INSTALL_DIR})
