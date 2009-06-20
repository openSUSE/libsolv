#
# CMakeLists.txt for sat-solver/tools
#

# Let's not compile the same files ten times; this library is not installed
ADD_LIBRARY( toolstuff STATIC repo_write.c common_write.c )

SET(rpmdb2solv_REPOS
  rpmdb2solv.c
  repo_rpmdb.c
  repo_products.c
  repo_zyppdb.c
  repo_releasefile_products.c
)

ADD_EXECUTABLE( rpmdb2solv ${rpmdb2solv_REPOS} )
TARGET_LINK_LIBRARIES( rpmdb2solv satsolver toolstuff ${RPMDB_LIBRARY} ${EXPAT_LIBRARY})

SET(rpms2solv_REPOS rpms2solv.c repo_rpmdb.c )
ADD_EXECUTABLE( rpms2solv ${rpms2solv_REPOS} )
TARGET_LINK_LIBRARIES( rpms2solv satsolver toolstuff ${RPMDB_LIBRARY})

SET(rpmmd2solv_REPOS rpmmd2solv.c repo_rpmmd.c )
ADD_EXECUTABLE( rpmmd2solv ${rpmmd2solv_REPOS} )
TARGET_LINK_LIBRARIES( rpmmd2solv satsolver toolstuff ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

SET(helix2solv_REPOS helix2solv.c )
ADD_EXECUTABLE( helix2solv ${helix2solv_REPOS} )
TARGET_LINK_LIBRARIES( helix2solv satsolver toolstuff ${EXPAT_LIBRARY})

SET(susetags2solv_REPOS susetags2solv.c repo_susetags.c repo_content.c )
ADD_EXECUTABLE( susetags2solv ${susetags2solv_REPOS} )
TARGET_LINK_LIBRARIES( susetags2solv satsolver toolstuff ${ZLIB_LIBRARY})

SET(updateinfoxml2solv_REPOS updateinfoxml2solv.c repo_updateinfoxml.c)
ADD_EXECUTABLE( updateinfoxml2solv ${updateinfoxml2solv_REPOS} )
TARGET_LINK_LIBRARIES( updateinfoxml2solv satsolver toolstuff ${EXPAT_LIBRARY})

SET(deltainfoxml2solv_REPOS deltainfoxml2solv.c repo_deltainfoxml.c)
ADD_EXECUTABLE( deltainfoxml2solv ${deltainfoxml2solv_REPOS} )
TARGET_LINK_LIBRARIES( deltainfoxml2solv satsolver toolstuff ${EXPAT_LIBRARY})

SET(repomdxml2solv_REPOS repomdxml2solv.c repo_repomdxml.c)
ADD_EXECUTABLE( repomdxml2solv ${repomdxml2solv_REPOS} )
TARGET_LINK_LIBRARIES( repomdxml2solv satsolver toolstuff ${EXPAT_LIBRARY})

SET(installcheck_SOURCES installcheck.c repo_rpmmd.c repo_susetags.c)
ADD_EXECUTABLE(installcheck ${installcheck_SOURCES})
TARGET_LINK_LIBRARIES(installcheck satsolver ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

SET(patchcheck_SOURCES patchcheck.c repo_rpmmd.c repo_susetags.c repo_updateinfoxml.c)
ADD_EXECUTABLE(patchcheck ${patchcheck_SOURCES})
TARGET_LINK_LIBRARIES(patchcheck satsolver ${EXPAT_LIBRARY} ${ZLIB_LIBRARY})

ADD_EXECUTABLE( dumpsolv dumpsolv.c )
TARGET_LINK_LIBRARIES( dumpsolv satsolver)

ADD_EXECUTABLE( mergesolv mergesolv.c )
TARGET_LINK_LIBRARIES( mergesolv satsolver toolstuff)

SET(findfileconflicts_SOURCES findfileconflicts.c pool_fileconflicts.c repo_rpmdb.c )
ADD_EXECUTABLE( findfileconflicts ${findfileconflicts_SOURCES} )
TARGET_LINK_LIBRARIES( findfileconflicts satsolver ${RPMDB_LIBRARY} ${EXPAT_LIBRARY} )

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
   DESTINATION ${BIN_INSTALL_DIR} )

install(PROGRAMS repo2solv.sh DESTINATION ${BIN_INSTALL_DIR} )
