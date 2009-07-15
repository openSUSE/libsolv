
SET(libsatsolver_SRCS
    bitmap.c poolarch.c poolvendor.c poolid.c strpool.c dirpool.c
    solver.c solverdebug.c repo_solv.c evr.c pool.c
    queue.c repo.c repodata.c repopage.c util.c policy.c solvable.c
    transaction.c rules.c problems.c
    chksum.c md5.c sha1.c sha2.c)

ADD_LIBRARY(satsolver STATIC ${libsatsolver_SRCS})

SET(libsatsolver_HEADERS
    bitmap.h evr.h hash.h policy.h poolarch.h poolvendor.h pool.h
    poolid.h pooltypes.h queue.h solvable.h solver.h solverdebug.h
    repo.h repodata.h repopage.h repo_solv.h util.h
    strpool.h dirpool.h knownid.h transaction.h rules.h problems.h
    chksum.h md5.h sha1.h sha2.h ${CMAKE_BINARY_DIR}/src/satversion.h)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

INSTALL(FILES ${libsatsolver_HEADERS} DESTINATION "${CMAKE_INSTALL_PREFIX}/include/satsolver")
INSTALL(TARGETS satsolver LIBRARY DESTINATION ${LIB_INSTALL_DIR} ARCHIVE DESTINATION ${LIB_INSTALL_DIR})
