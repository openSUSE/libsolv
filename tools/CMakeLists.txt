#
# CMakeLists.txt for tools
#

ADD_LIBRARY (toolstuff STATIC common_write.c)

IF (ENABLE_STATIC AND NOT DISABLE_SHARED AND ENABLE_STATIC_TOOLS)
    SET (LIBSOLV_TOOLS_LIBRARY libsolv_static)
    SET (LIBSOLV_TOOLS_LIBRARIES libsolvext_static libsolv_static)
ELSE (ENABLE_STATIC AND NOT DISABLE_SHARED AND ENABLE_STATIC_TOOLS)
    SET (LIBSOLV_TOOLS_LIBRARY libsolv)
    SET (LIBSOLV_TOOLS_LIBRARIES libsolvext libsolv)
ENDIF (ENABLE_STATIC AND NOT DISABLE_SHARED AND ENABLE_STATIC_TOOLS)

IF (WIN32)
    INCLUDE_DIRECTORIES (${PROJECT_SOURCE_DIR}/win32/)
ENDIF (WIN32)

SET (tools_list testsolv mergesolv dumpsolv installcheck testsolv)

ADD_EXECUTABLE (dumpsolv dumpsolv.c )
TARGET_LINK_LIBRARIES (dumpsolv ${LIBSOLV_TOOLS_LIBRARY})

IF (ENABLE_RPMDB)
ADD_EXECUTABLE (rpmdb2solv rpmdb2solv.c)
TARGET_LINK_LIBRARIES (rpmdb2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (findfileconflicts findfileconflicts.c)
TARGET_LINK_LIBRARIES (findfileconflicts ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} rpmdb2solv)
ENDIF (ENABLE_RPMDB)

IF (ENABLE_RPMPKG)
ADD_EXECUTABLE (rpms2solv rpms2solv.c)
TARGET_LINK_LIBRARIES (rpms2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})
SET (tools_list ${tools_list} rpms2solv)
ENDIF (ENABLE_RPMPKG)

IF (ENABLE_RPMMD)
ADD_EXECUTABLE (repomdxml2solv repomdxml2solv.c)
TARGET_LINK_LIBRARIES (repomdxml2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (rpmmd2solv rpmmd2solv.c)
TARGET_LINK_LIBRARIES (rpmmd2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (updateinfoxml2solv updateinfoxml2solv.c)
TARGET_LINK_LIBRARIES (updateinfoxml2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (deltainfoxml2solv deltainfoxml2solv.c)
TARGET_LINK_LIBRARIES (deltainfoxml2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} repomdxml2solv rpmmd2solv updateinfoxml2solv deltainfoxml2solv)
ENDIF (ENABLE_RPMMD)

IF (ENABLE_HELIXREPO)
ADD_EXECUTABLE (helix2solv helix2solv.c)
TARGET_LINK_LIBRARIES (helix2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} helix2solv)
ENDIF (ENABLE_HELIXREPO)

IF (ENABLE_SUSEREPO)
ADD_EXECUTABLE (susetags2solv susetags2solv.c)
TARGET_LINK_LIBRARIES (susetags2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} susetags2solv)
ENDIF (ENABLE_SUSEREPO)

IF (ENABLE_COMPS)
ADD_EXECUTABLE (comps2solv comps2solv.c)
TARGET_LINK_LIBRARIES (comps2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} comps2solv)
ENDIF (ENABLE_COMPS)

IF (ENABLE_DEBIAN)
ADD_EXECUTABLE (deb2solv deb2solv.c)
TARGET_LINK_LIBRARIES (deb2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} deb2solv)
ENDIF (ENABLE_DEBIAN)

IF (ENABLE_MDKREPO)
ADD_EXECUTABLE (mdk2solv mdk2solv.c)
TARGET_LINK_LIBRARIES (mdk2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} mdk2solv)
ENDIF (ENABLE_MDKREPO)

IF (ENABLE_ARCHREPO)
ADD_EXECUTABLE (archpkgs2solv archpkgs2solv.c)
TARGET_LINK_LIBRARIES (archpkgs2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (archrepo2solv archrepo2solv.c)
TARGET_LINK_LIBRARIES (archrepo2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} archpkgs2solv archrepo2solv)
ENDIF (ENABLE_ARCHREPO)

IF (ENABLE_APK)
ADD_EXECUTABLE (apk2solv apk2solv.c)
TARGET_LINK_LIBRARIES (apk2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} apk2solv)
ENDIF (ENABLE_APK)

IF (ENABLE_CUDFREPO)
ADD_EXECUTABLE (cudftest cudftest.c)
TARGET_LINK_LIBRARIES (cudftest ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})
ENDIF (ENABLE_CUDFREPO)

ADD_EXECUTABLE (installcheck installcheck.c)
TARGET_LINK_LIBRARIES (installcheck ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

IF (ENABLE_APPDATA)
ADD_EXECUTABLE (appdata2solv appdata2solv.c)
TARGET_LINK_LIBRARIES (appdata2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} appdata2solv)
ENDIF (ENABLE_APPDATA)

IF (ENABLE_CONDA)
ADD_EXECUTABLE (conda2solv conda2solv.c)
TARGET_LINK_LIBRARIES (conda2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

SET (tools_list ${tools_list} conda2solv)
ENDIF (ENABLE_CONDA)

IF (NOT WIN32)
ADD_EXECUTABLE (repo2solv repo2solv.c )
TARGET_LINK_LIBRARIES (repo2solv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})
SET(tools_list ${tools_list} repo2solv)
ENDIF (NOT WIN32)

ADD_EXECUTABLE (mergesolv mergesolv.c )
TARGET_LINK_LIBRARIES (mergesolv toolstuff ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

ADD_EXECUTABLE (testsolv testsolv.c)
TARGET_LINK_LIBRARIES (testsolv ${LIBSOLV_TOOLS_LIBRARIES} ${SYSTEM_LIBRARIES})

INSTALL (TARGETS ${tools_list} DESTINATION ${CMAKE_INSTALL_BINDIR})
