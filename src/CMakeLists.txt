
SET(libsatsolver_SRCS bitmap.c  poolarch.c  poolid.c  solver.c  source_solv.c
evr.c pool.c queue.c   source.c  util.c)

ADD_LIBRARY(satsolver STATIC ${libsatsolver_SRCS})