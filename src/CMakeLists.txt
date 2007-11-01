
SET(libsatsolver_SRCS bitmap.c  poolarch.c  poolvendor.c  poolid.c
solver.c  repo_solv.c evr.c pool.c queue.c   repo.c  util.c
policy-ruby.c)

ADD_LIBRARY(satsolver STATIC ${libsatsolver_SRCS})

SET(libsatsolver_HEADERS bitmap.h evr.h hash.h policy.h poolarch.h poolvendor.h pool.h poolid.h pooltypes.h queue.h solvable.h solver.h repo.h repo_solv.h util.h )

SET( CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -g -O3 -Wall -I${RUBY_INCLUDE_PATH}" )

INSTALL(  FILES ${libsatsolver_HEADERS} DESTINATION "${CMAKE_INSTALL_PREFIX}/include/satsolver" )
INSTALL(TARGETS satsolver LIBRARY DESTINATION ${LIB_INSTALL_DIR} ARCHIVE DESTINATION ${LIB_INSTALL_DIR})
