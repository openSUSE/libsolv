
SET (libsolv_MANPAGES
    libsolv.3 libsolv-bindings.3 libsolv-constantids.3 libsolv-history.3
    libsolv-pool.3)

INSTALL(FILES
    ${libsolv_MANPAGES}
    DESTINATION "${MAN_INSTALL_DIR}/man3")
