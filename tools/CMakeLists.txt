#
# CMakeLists.txt for sat-solver/tools
#

ADD_LIBRARY( toolstuff STATIC common_write.c )

ADD_EXECUTABLE(rpmdb2solv rpmdb2solv.c)
TARGET_LINK_LIBRARIES(rpmdb2solv toolstuff satsolverext satsolver ${RPMDB_LIBRARY} ${EXPAT_LIBRARY})

ADD_EXECUTABLE(rpms2solv rpms2solv.c)
TARGET_LINK_LIBRARIES(rpms2solv toolstuff satsolverext satsolver ${RPMDB_LIBRARY})

ADD_EXECUTABLE(rpmmd2solv rpmmd2solv.c)
TARGET_LINK_LIBRARIES(rpmmd2solv toolstuff satsolverext satsolver ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

ADD_EXECUTABLE(helix2solv helix2solv.c ${helix2solv_REPOS} )
TARGET_LINK_LIBRARIES(helix2solv toolstuff satsolverext satsolver ${EXPAT_LIBRARY})

ADD_EXECUTABLE(susetags2solv susetags2solv.c)
TARGET_LINK_LIBRARIES(susetags2solv toolstuff satsolverext satsolver ${ZLIB_LIBRARY})

ADD_EXECUTABLE(updateinfoxml2solv updateinfoxml2solv.c)
TARGET_LINK_LIBRARIES(updateinfoxml2solv toolstuff satsolverext satsolver ${EXPAT_LIBRARY})

ADD_EXECUTABLE(deltainfoxml2solv deltainfoxml2solv.c)
TARGET_LINK_LIBRARIES(deltainfoxml2solv toolstuff satsolverext satsolver ${EXPAT_LIBRARY})

ADD_EXECUTABLE(repomdxml2solv repomdxml2solv.c)
TARGET_LINK_LIBRARIES(repomdxml2solv toolstuff satsolverext satsolver ${EXPAT_LIBRARY})

ADD_EXECUTABLE(installcheck installcheck.c)
TARGET_LINK_LIBRARIES(installcheck satsolverext satsolver ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

ADD_EXECUTABLE(patchcheck patchcheck.c)
TARGET_LINK_LIBRARIES(patchcheck satsolverext satsolver ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

ADD_EXECUTABLE(dumpsolv dumpsolv.c )
TARGET_LINK_LIBRARIES(dumpsolv satsolver)

ADD_EXECUTABLE(mergesolv mergesolv.c )
TARGET_LINK_LIBRARIES(mergesolv toolstuff satsolverext satsolver)

ADD_EXECUTABLE(findfileconflicts findfileconflicts.c)
TARGET_LINK_LIBRARIES(findfileconflicts satsolverext satsolver ${RPMDB_LIBRARY})

install(TARGETS
    mergesolv
    dumpsolv
    susetags2solv
    helix2solv
    rpmmd2solv
    rpmdb2solv
    rpms2solv
    updateinfoxml2solv
    deltainfoxml2solv
    repomdxml2solv
    DESTINATION ${BIN_INSTALL_DIR})

install(PROGRAMS
   repo2solv.sh
   DESTINATION ${BIN_INSTALL_DIR})
