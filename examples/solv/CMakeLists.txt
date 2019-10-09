
ADD_EXECUTABLE (solv solv.c
checksig.c
deltarpm.c
fastestmirror.c
fileconflicts.c
fileprovides.c
mirror.c
patchjobs.c
repoinfo.c
repoinfo_cache.c
repoinfo_config_debian.c
repoinfo_config_yum.c
repoinfo_config_urpmi.c
repoinfo_download.c
repoinfo_system_debian.c
repoinfo_system_rpm.c
repoinfo_type_debian.c
repoinfo_type_mdk.c
repoinfo_type_rpmmd.c
repoinfo_type_susetags.c
)

TARGET_LINK_LIBRARIES (solv libsolvext libsolv ${SYSTEM_LIBRARIES})

cpack_add_component("examples")

INSTALL(TARGETS
    solv
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT "examples")

set("CPACK_DEBIAN_EXAMPLES_PACKAGE_DEPENDS" "${CPACK_DEBIAN_LIBSOLV_PACKAGE_NAME}, ${CPACK_DEBIAN_LIBSOLV_PACKAGE_NAME}-dev")

pass_through_cpack_vars()