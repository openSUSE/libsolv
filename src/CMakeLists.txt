
SET(libsatsolver_SRCS bitmap.c  poolarch.c  poolid.c  solver.c  repo_solv.c
evr.c pool.c queue.c   repo.c  util.c)

ADD_LIBRARY(satsolver STATIC ${libsatsolver_SRCS})

SET(libsatsolver_HEADERS bitmap.h evr.h hash.h poolarch.h pool.h poolid.h pooltypes.h queue.h solvable.h solver.h repo.h repo_solv.h util.h )

INSTALL(  FILES ${libsatsolver_HEADERS} DESTINATION "${CMAKE_INSTALL_PREFIX}/include/satsolver" )
INSTALL(TARGETS satsolver LIBRARY DESTINATION ${LIB_INSTALL_DIR} ARCHIVE DESTINATION ${LIB_INSTALL_DIR})