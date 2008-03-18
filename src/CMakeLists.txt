
SET(libsatsolver_SRCS bitmap.c  poolarch.c  poolvendor.c  poolid.c strpool.c dirpool.c
solver.c repo_solv.c repo_helix.c evr.c pool.c queue.c   repo.c  repodata.c util.c policy.c fastlz.c solvable.c)

ADD_LIBRARY(satsolver STATIC ${libsatsolver_SRCS})

SET(libsatsolver_HEADERS bitmap.h evr.h hash.h policy.h poolarch.h poolvendor.h pool.h poolid.h pooltypes.h queue.h solvable.h solver.h repo.h repodata.h repo_solv.h repo_helix.h util.h strpool.h dirpool.h knownid.h)

SET( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC" )

INSTALL(  FILES ${libsatsolver_HEADERS} DESTINATION "${CMAKE_INSTALL_PREFIX}/include/satsolver" )
INSTALL(TARGETS satsolver LIBRARY DESTINATION ${LIB_INSTALL_DIR} ARCHIVE DESTINATION ${LIB_INSTALL_DIR})
