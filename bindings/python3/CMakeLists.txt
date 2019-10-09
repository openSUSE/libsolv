#
# used for building both python2 and python3 bindings
# do not use if you want to build just one flavor, use the
# standard python bindings file in that case.
#
# we cannot use FIND_PACKAGE PythonLibs here, as this would
# clash with the python variables.
#
find_package (Python3 COMPONENTS Interpreter Development)

SET (SWIG_PY3_FLAGS -DPYTHON3=1)
SET (SWIG_PY3_FLAGS ${SWIG_PY3_FLAGS} -DSWIG_PYTHON_LEGACY_BOOL=1)

MESSAGE (STATUS "Python3 executable: ${PYTHON_EXECUTABLE}")
MESSAGE (STATUS "Python3 installation dir: ${Python3_STDARCH}")
MESSAGE (STATUS "Python3 include path: ${Python3_INCLUDE_DIRS}")

ADD_CUSTOM_COMMAND (
    OUTPUT solv_python.c
    COMMAND ${SWIG_EXECUTABLE} ${SWIG_FLAGS} -python ${SWIG_PY3_FLAGS} -I${CMAKE_SOURCE_DIR}/src -o solv_python.c ${CMAKE_SOURCE_DIR}/bindings/solv.i
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_SOURCE_DIR}/bindings/solv.i
)

ADD_DEFINITIONS(-Wno-unused)
INCLUDE_DIRECTORIES (${Python3_INCLUDE_DIRS})

ADD_LIBRARY (bindings_python3 SHARED solv_python.c)
SET_TARGET_PROPERTIES (bindings_python3 PROPERTIES PREFIX "" OUTPUT_NAME "_solv")
TARGET_LINK_LIBRARIES (bindings_python3 libsolvext libsolv ${SYSTEM_LIBRARIES})

cpack_add_component("python3"
    DEPENDS "libsolv"
)
set("CPACK_DEBIAN_PYTHON3_PACKAGE_NAME" "python3-${PACKAGE}")
set ("CPACK_DEBIAN_PYTHON3_PACKAGE_PROVIDES" "python3-${PACKAGE}")

INSTALL (TARGETS bindings_python3 LIBRARY DESTINATION ${Python3_STDARCH} COMPONENT "python3" )
INSTALL (FILES ${CMAKE_CURRENT_BINARY_DIR}/solv.py DESTINATION ${Python3_STDARCH} COMPONENT "python3" )

set ("CPACK_DEBIAN_PYTHON3_PACKAGE_DEPENDS" "libpython${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR} (>=${Python3_VERSION}~), python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR} (>=${Python3_VERSION}~), ${CPACK_DEBIAN_LIBSOLV_PACKAGE_NAME}")
pass_through_cpack_vars()