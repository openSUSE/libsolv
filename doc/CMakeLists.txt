
SET (libsolv_MANPAGES3
    libsolv.3 libsolv-bindings.3 libsolv-constantids.3 libsolv-history.3
    libsolv-pool.3)

SET (libsolv_MANPAGES1
    mergesolv.1 dumpsolv.1 installcheck.1 testsolv.1)

INSTALL(FILES
    ${libsolv_MANPAGES3}
    DESTINATION "${MAN_INSTALL_DIR}/man3")

INSTALL(FILES
    ${libsolv_MANPAGES1}
    DESTINATION "${MAN_INSTALL_DIR}/man1")
