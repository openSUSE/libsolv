PROJECT(satsolver)

# Library
IF ( DEFINED LIB )
  SET ( LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/${LIB}" )
ELSE ( DEFINED  LIB )
  IF (CMAKE_SIZEOF_VOID_P MATCHES "8")
    SET( LIB_SUFFIX "64" )
  ENDIF(CMAKE_SIZEOF_VOID_P MATCHES "8")
  SET ( LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}" )
ENDIF ( DEFINED  LIB )
MESSAGE(STATUS "Libraries will be installed in ${LIB_INSTALL_DIR}" )
SET( BIN_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/bin" )

####################################################################
# CONFIGURATION                                                    #
####################################################################

# where to look first for cmake modules, before ${CMAKE_ROOT}/Modules/ is checked
SET( CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules )

INCLUDE( ${CMAKE_SOURCE_DIR}/VERSION.cmake )

FIND_PACKAGE(EXPAT REQUIRED)
FIND_LIBRARY(RPMDB_LIBRARY NAMES rpmdb)
FIND_LIBRARY(ZLIB_LIBRARY NAMES z)

SET( PACKAGE "satsolver" )
SET( VERSION "${LIBSATSOLVER_MAJOR}.${LIBSATSOLVER_MINOR}.${LIBSATSOLVER_PATCH}" )

ADD_DEFINITIONS( -D_FILE_OFFSET_BITS=64 )
ADD_DEFINITIONS( -DVERSION=\\\"${VERSION}\\\" )

SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "mls solver")
SET(CPACK_PACKAGE_VENDOR "Novell Inc.")
SET(CPACK_PACKAGE_VERSION_MAJOR ${LIBSATSOLVER_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${LIBSATSOLVER_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${LIBSATSOLVER_PATCH})
SET( CPACK_GENERATOR "TBZ2")
SET( CPACK_SOURCE_GENERATOR "TBZ2")
SET( CPACK_SOURCE_PACKAGE_FILE_NAME "${PACKAGE}-${VERSION}" )

# The following components are regex's to match anywhere (unless anchored)
# in absolute path + filename to find files or directories to be excluded
# from source tarball.
SET (CPACK_SOURCE_IGNORE_FILES
#svn files
"\\\\.svn/"
"\\\\.cvsignore$"
# temporary files
"\\\\.swp$"
# backup files
"~$"
# eclipse files
"\\\\.cdtproject$"
"\\\\.cproject$"
"\\\\.project$"
"\\\\.settings/"
# others
"\\\\.#"
"/#"
"/build/"
"/_build/"
"/\\\\.git/"
# used before
"/CVS/"
"/\\\\.libs/"
"/\\\\.deps/"
"\\\\.o$"
"\\\\.lo$"
"\\\\.la$"
"Makefile\\\\.in$"
"testsuite/data.libzypp/kernel-tests/kernel-hardware"
"testsuite/data.libzypp/yast-tests/kernel3-hardware"
)

INCLUDE(CPack)

####################################################################

SET( DOC_INSTALL_DIR
   "${CMAKE_INSTALL_PREFIX}/share/doc/packages/${PACKAGE}"
   CACHE PATH "The install dir for documentation (default prefix/share/doc/packages/${PACKAGE})"
   FORCE
)

####################################################################
# INCLUDES                                                         #
####################################################################

#SET (CMAKE_INCLUDE_DIRECTORIES_BEFORE ON)
INCLUDE_DIRECTORIES( ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR} SYSTEM )

####################################################################

MESSAGE(STATUS "Looking modules in ${CMAKE_MODULE_PATH}")
FIND_PROGRAM(SWIG_EXECUTABLE
  NAMES swig-1.3 swig
  PATHS ${SWIG_DIR} ${SWIG_DIR}/.. ${SWIG_DIR}/../../bin /usr/bin /usr/local/bin ${CMAKE_INSTALL_PREFIX}/bin
)

set ( CMAKE_C_FLAGS     "${CMAKE_C_FLAGS} -Werror -Wall" )
set ( CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS} -g -O3" )
set ( CMAKE_C_FLAGS_DEBUG     "${CMAKE_C_FLAGS} -g3 -O0" )

ADD_SUBDIRECTORY(src)
ADD_SUBDIRECTORY(tools)
ADD_SUBDIRECTORY(applayer)
ADD_SUBDIRECTORY(testsuite)

IF ( NOT SWIG_EXECUTABLE )
  MESSAGE( FATAL "SWIG not found." )
ELSE ( NOT SWIG_EXECUTABLE )
  MESSAGE( STATUS "SWIG found at ${SWIG_EXECUTABLE}" )
  ADD_SUBDIRECTORY(bindings)
ENDIF ( NOT SWIG_EXECUTABLE )

MESSAGE(STATUS "version: ${VERSION}")

####################################################################
# RPM SPEC                                                         #
####################################################################

MACRO(SPECFILE)
  MESSAGE(STATUS "Writing spec file...")
  CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/package/libsatsolver.spec.in ${CMAKE_BINARY_DIR}/package/libsatsolver.spec @ONLY)
  #MESSAGE(STATUS "I hate you rpm-lint...!!!")
  #CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/package/libsatsolver-rpmlint.cmake ${CMAKE_BINARY_DIR}/package/libsatsolver-rpmlintrc @ONLY)
ENDMACRO(SPECFILE)

SPECFILE()

ADD_CUSTOM_TARGET( svncheck
  COMMAND cd $(CMAKE_SOURCE_DIR) && ! LC_ALL=C svn status --show-updates --quiet | grep -v '^Status against revision'
)

SET( AUTOBUILD_COMMAND
  COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/package/*.tar.bz2
  COMMAND ${CMAKE_MAKE_PROGRAM} package_source
  COMMAND ${CMAKE_COMMAND} -E copy ${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.bz2 ${CMAKE_BINARY_DIR}/package
  COMMAND ${CMAKE_COMMAND} -E remove ${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.bz2
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/package/libsatsolver.changes" "${CMAKE_BINARY_DIR}/package/libsatsolver.changes"
)

ADD_CUSTOM_TARGET( srcpackage_local
  ${AUTOBUILD_COMMAND}
)

ADD_CUSTOM_TARGET( srcpackage
  COMMAND ${CMAKE_MAKE_PROGRAM} svncheck
  ${AUTOBUILD_COMMAND}
)
