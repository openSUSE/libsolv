
SET(rpmdb2solv_REPOS
  rpmdb2solv.c
  repo_rpmdb.h
  repo_rpmdb.c
  repo_write.c
  common_write.c
)

ADD_EXECUTABLE( rpmdb2solv ${rpmdb2solv_REPOS} )
TARGET_LINK_LIBRARIES( rpmdb2solv satsolver ${DB43_LIBRARY})

SET(rpmmd2solv_REPOS rpmmd2solv.c repo_rpmmd.h repo_rpmmd.c repo_write.c common_write.c )
ADD_EXECUTABLE( rpmmd2solv ${rpmmd2solv_REPOS} )
TARGET_LINK_LIBRARIES( rpmmd2solv satsolver ${EXPAT_LIBRARY})

SET(helix2solv_REPOS helix2solv.c repo_write.c common_write.c )
ADD_EXECUTABLE( helix2solv ${helix2solv_REPOS} )
TARGET_LINK_LIBRARIES( helix2solv satsolver ${EXPAT_LIBRARY})

SET(susetags2solv_REPOS susetags2solv.c repo_susetags.h repo_susetags.c repo_content.c repo_write.c common_write.c)
ADD_EXECUTABLE( susetags2solv ${susetags2solv_REPOS} )
TARGET_LINK_LIBRARIES( susetags2solv satsolver)

SET(patchxml2solv_REPOS patchxml2solv.c repo_patchxml.h repo_patchxml.c repo_write.c common_write.c)
ADD_EXECUTABLE( patchxml2solv ${patchxml2solv_REPOS} )
TARGET_LINK_LIBRARIES( patchxml2solv satsolver ${EXPAT_LIBRARY})


SET(dumpsolv_REPOS dumpsolv.c)
ADD_EXECUTABLE( dumpsolv ${dumpsolv_REPOS} )
TARGET_LINK_LIBRARIES( dumpsolv satsolver)

SET(mergesolv_REPOS mergesolv.c repo_write.c common_write.c)
ADD_EXECUTABLE( mergesolv ${mergesolv_REPOS} )
TARGET_LINK_LIBRARIES( mergesolv satsolver)

#SET(dumpattr_REPOS dumpattr.c)
#ADD_EXECUTABLE( dumpattr ${dumpattr_REPOS} )
#TARGET_LINK_LIBRARIES( dumpattr satsolver)

#ADD_EXECUTABLE( addstore addstore.c repo_write.c )
#TARGET_LINK_LIBRARIES( addstore satsolver )

install(TARGETS
                mergesolv 
                dumpsolv 
                patchxml2solv 
                susetags2solv
                helix2solv 
                rpmmd2solv 
                rpmdb2solv
   DESTINATION ${BIN_INSTALL_DIR} )

install(PROGRAMS repo2solv.sh DESTINATION ${BIN_INSTALL_DIR} )
