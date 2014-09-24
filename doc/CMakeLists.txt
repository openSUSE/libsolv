
SET (libsolv_MANPAGES3
    libsolv.3 libsolv-bindings.3 libsolv-constantids.3 libsolv-history.3
    libsolv-pool.3)

SET (libsolv_MANPAGES1
    mergesolv.1 dumpsolv.1 installcheck.1 testsolv.1)

IF (ENABLE_RPMDB)
SET (libsolv_MANPAGES1 ${libsolv_MANPAGES1} rpmdb2solv.1 rpms2solv.1)
ENDIF (ENABLE_RPMDB)

IF (ENABLE_RPMMD)
SET (libsolv_MANPAGES1 ${libsolv_MANPAGES1} repomdxml2solv.1 rpmmd2solv.1 updateinfoxml2solv.1 deltainfoxml2solv.1)
ENDIF (ENABLE_RPMMD)

INSTALL(FILES
    ${libsolv_MANPAGES3}
    DESTINATION "${MAN_INSTALL_DIR}/man3")

INSTALL(FILES
    ${libsolv_MANPAGES1}
    DESTINATION "${MAN_INSTALL_DIR}/man1")
