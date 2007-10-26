
SET(rpmdb2solv_SOURCES
  rpmdb2solv.c
  source_rpmdb.h
  source_rpmdb.c
  source_write.c
)

ADD_EXECUTABLE( rpmdb2solv ${rpmdb2solv_SOURCES} )
TARGET_LINK_LIBRARIES( rpmdb2solv satsolver ${DB43_LIBRARY})

SET(rpmmd2solv_SOURCES rpmmd2solv.c source_rpmmd.h source_rpmmd.c source_write.c )
ADD_EXECUTABLE( rpmmd2solv ${rpmmd2solv_SOURCES} )
TARGET_LINK_LIBRARIES( rpmmd2solv satsolver ${EXPAT_LIBRARY})

SET(helix2solv_SOURCES helix2solv.c source_helix.h source_helix.c source_write.c )
ADD_EXECUTABLE( helix2solv ${helix2solv_SOURCES} )
TARGET_LINK_LIBRARIES( helix2solv satsolver ${EXPAT_LIBRARY})

SET(susetags2solv_SOURCES susetags2solv.c source_susetags.h source_susetags.c source_write.c)
ADD_EXECUTABLE( susetags2solv ${susetags2solv_SOURCES} )
TARGET_LINK_LIBRARIES( susetags2solv satsolver)

SET(patchxml2solv_SOURCES patchxml2solv.c source_patchxml.h source_patchxml.c source_write.c)
ADD_EXECUTABLE( patchxml2solv ${patchxml2solv_SOURCES} )
TARGET_LINK_LIBRARIES( patchxml2solv satsolver ${EXPAT_LIBRARY})


SET(content2solv_SOURCES
  content2solv.c
  source_content.h
  source_content.c
  source_write.c)
ADD_EXECUTABLE( content2solv ${content2solv_SOURCES} )
TARGET_LINK_LIBRARIES( content2solv satsolver)


SET(dumpsolv_SOURCES dumpsolv.c)
ADD_EXECUTABLE( dumpsolv ${dumpsolv_SOURCES} )
TARGET_LINK_LIBRARIES( dumpsolv satsolver)

SET(mergesolv_SOURCES mergesolv.c source_write.c)
ADD_EXECUTABLE( mergesolv ${mergesolv_SOURCES} )
TARGET_LINK_LIBRARIES( mergesolv satsolver)