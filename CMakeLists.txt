PROJECT(satsolver)

cmake_minimum_required(VERSION 2.4)

#IF(${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERISION} GREATER 2.4)
#ENDIF(${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERISION} GREATER 2.4)

if(COMMAND cmake_policy)
  # escape preprocessor, see -DVERSION below
  cmake_policy(SET CMP0005 OLD)
endif(COMMAND cmake_policy)

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
FIND_PACKAGE(Check REQUIRED)
FIND_PACKAGE(ZLIB REQUIRED)

IF ( FEDORA )
MESSAGE(STATUS "Building for Fedora")
ADD_DEFINITIONS( -DFEDORA )
ENDIF ( FEDORA)

IF ( DEBIAN )
MESSAGE(STATUS "Building for Debian")
ADD_DEFINITIONS( -DDEBIAN -DDEBIAN_SEMANTICS)
ENDIF ( DEBIAN )

IF ( MULTI_SEMANTICS )
MESSAGE(STATUS "Enabling multi dist support")
ADD_DEFINITIONS( -DMULTI_SEMANTICS)
ENDIF ( MULTI_SEMANTICS )

IF ( RPM5 )
MESSAGE(STATUS "Enabling RPM 5 support")
ADD_DEFINITIONS( -DRPM5)
FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(RPM REQUIRED rpm)
INCLUDE_DIRECTORIES( ${RPM_INCLUDE_DIRS} )
ENDIF (RPM5 )


IF ( RPM5 OR NOT DEBIAN )
FIND_LIBRARY(RPMDB_LIBRARY NAMES rpmdb)

IF ( NOT RPMDB_LIBRARY )
FIND_LIBRARY(RPMDB_LIBRARY NAMES rpm)
ENDIF( NOT RPMDB_LIBRARY )

FIND_LIBRARY(RPMIO_LIBRARY NAMES rpmio)
IF ( RPMIO_LIBRARY )
SET( RPMDB_LIBRARY ${RPMIO_LIBRARY} ${RPMDB_LIBRARY} )
ENDIF ( RPMIO_LIBRARY )

IF ( RPM5 )
FIND_LIBRARY(RPMMISC_LIBRARY NAMES rpmmisc)
IF ( RPMMISC_LIBRARY )
SET ( RPMDB_LIBRARY ${RPMMISC_LIBRARY} ${RPMDB_LIBRARY} )
ENDIF ( RPMMISC_LIBRARY )
ENDIF ( RPM5 )

IF ( RPM5 OR FEDORA )
FIND_LIBRARY(DB_LIBRARY NAMES db)
IF ( DB_LIBRARY )
SET( RPMDB_LIBRARY ${DB_LIBRARY} ${RPMDB_LIBRARY} )
ENDIF ( DB_LIBRARY )
ENDIF ( RPM5 OR FEDORA )

ENDIF ( RPM5 OR NOT DEBIAN )

INCLUDE ( CheckFunctionExists )
INCLUDE ( TestBigEndian )

CHECK_FUNCTION_EXISTS( strchrnul HAVE_STRCHRNUL )
CHECK_FUNCTION_EXISTS( fopencookie HAVE_FOPENCOOKIE )
CHECK_FUNCTION_EXISTS( funopen HAVE_FUNOPEN )
TEST_BIG_ENDIAN( WORDS_BIGENDIAN )

# should create config.h with #cmakedefine instead...
FOREACH( VAR HAVE_STRCHRNUL HAVE_FOPENCOOKIE HAVE_FUNOPEN WORDS_BIGENDIAN )
  IF( ${VAR} )
    ADD_DEFINITIONS( -D${VAR}=1 )
  ENDIF( ${VAR} )
ENDFOREACH( VAR )

SET( PACKAGE "satsolver" )
SET( VERSION "${LIBSATSOLVER_MAJOR}.${LIBSATSOLVER_MINOR}.${LIBSATSOLVER_PATCH}" )

ADD_DEFINITIONS( -D_FILE_OFFSET_BITS=64 )
ADD_DEFINITIONS( -DVERSION=\\\"${VERSION}\\\" )
CONFIGURE_FILE( src/satversion.h.in src/satversion.h )

SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SAT Solver library")
SET(CPACK_PACKAGE_VENDOR "Novell Inc.")
SET(CPACK_PACKAGE_VERSION_MAJOR ${LIBSATSOLVER_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${LIBSATSOLVER_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${LIBSATSOLVER_PATCH})
SET(CPACK_GENERATOR "TBZ2")
SET(CPACK_SOURCE_GENERATOR "TBZ2")
SET(CPACK_SOURCE_PACKAGE_FILE_NAME "${PACKAGE}-${VERSION}")
SET(CPACK_SOURCE_TOPLEVEL_TAG "Linux-Source:")
SET(CPACK_TOPLEVEL_TAG "Linux-Source:")

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
"Makefile$"
"Makefile\\\\.in$"
# cmake cache files
"DartConfiguration.tcl$"
"CMakeCache.txt"
"CMakeFiles"
"cmake_install.cmake$"
"CMakeLists.txt.auto$"
"CTestTestfile.cmake"
"CPackConfig.cmake$"
"CPackSourceConfig.cmake$"
"libsatsolver.spec$"
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
INCLUDE_DIRECTORIES( ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/ext ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_BINARY_DIR}/src SYSTEM )

####################################################################

MESSAGE(STATUS "Looking modules in ${CMAKE_MODULE_PATH}")

set ( CMAKE_C_FLAGS     "${CMAKE_C_FLAGS} -Werror -Wall" )
set ( CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS} -g -O3" )
set ( CMAKE_C_FLAGS_DEBUG     "${CMAKE_C_FLAGS} -g3 -O0" )

ADD_SUBDIRECTORY(src)
ADD_SUBDIRECTORY(ext)
ADD_SUBDIRECTORY(tools)
ADD_SUBDIRECTORY(tests)
ADD_SUBDIRECTORY(examples)
ADD_SUBDIRECTORY(doc)

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
  COMMAND mkdir -p _CPack_Packages/${CPACK_TOPLEVEL_TAG}
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

INCLUDE(CTest)
ENABLE_TESTING()


