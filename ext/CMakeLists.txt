SET(libsatsolverext_SRCS
    pool_fileconflicts.c repo_content.c repo_deltainfoxml.c repo_helix.c repo_products.c
    repo_releasefile_products.c repo_repomdxml.c repo_rpmdb.c repo_rpmmd.c
    repo_susetags.c repo_updateinfoxml.c repo_write.c repo_zyppdb.c
    repo_deb.c)

ADD_LIBRARY(satsolverext STATIC ${libsatsolverext_SRCS})

SET(libsatsolver_HEADERS
    pool_fileconflicts.h repo_content.h repo_deltainfoxml.h repo_helix.h repo_products.h
    repo_releasefile_products.h repo_repomdxml.h repo_rpmdb.h repo_rpmmd.h
    repo_susetags.h repo_updateinfoxml.h repo_write.h repo_zyppdb.h
    tools_util.h repo_deb.h)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

INSTALL(FILES ${libsatsolverext_HEADERS} DESTINATION "${CMAKE_INSTALL_PREFIX}/include/satsolver")
INSTALL(TARGETS satsolverext LIBRARY DESTINATION ${LIB_INSTALL_DIR} ARCHIVE DESTINATION ${LIB_INSTALL_DIR})
