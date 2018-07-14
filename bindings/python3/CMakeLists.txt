#
# used for building both python2 and python3 bindings
# do not use if you want to build just one flavor, use the
# standard python bindings file in that case.
#
# we cannot use FIND_PACKAGE PythonLibs here, as this would
# clash with the python variables.
#
IF (NOT DEFINED PYTHON3_EXECUTABLE)
SET (PYTHON3_EXECUTABLE "/usr/bin/python3")
ENDIF (NOT DEFINED PYTHON3_EXECUTABLE)

EXECUTE_PROCESS(COMMAND ${PYTHON3_EXECUTABLE} -c "from sys import stdout; from distutils import sysconfig; stdout.write(sysconfig.get_python_lib(True))" OUTPUT_VARIABLE PYTHON3_INSTALL_DIR)
EXECUTE_PROCESS(COMMAND ${PYTHON3_EXECUTABLE} -c "from sys import stdout; from distutils import sysconfig; stdout.write(sysconfig.get_python_inc())" OUTPUT_VARIABLE PYTHON3_INCLUDE_DIR)

SET (SWIG_PY3_FLAGS -DPYTHON3=1)
SET (SWIG_PY3_FLAGS ${SWIG_PY3_FLAGS} -DSWIG_PYTHON_LEGACY_BOOL=1)

MESSAGE (STATUS "Python3 executable: ${PYTHON3_EXECUTABLE}")
MESSAGE (STATUS "Python3 installation dir: ${PYTHON3_INSTALL_DIR}")
MESSAGE (STATUS "Python3 include path: ${PYTHON3_INCLUDE_DIR}")

ADD_CUSTOM_COMMAND (
    OUTPUT solv_python.c
    COMMAND ${SWIG_EXECUTABLE} ${SWIG_FLAGS} -python ${SWIG_PY3_FLAGS} -I${CMAKE_SOURCE_DIR}/src -o solv_python.c ${CMAKE_SOURCE_DIR}/bindings/solv.i
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_SOURCE_DIR}/bindings/solv.i
)

ADD_DEFINITIONS(-Wno-unused)
INCLUDE_DIRECTORIES (${PYTHON3_INCLUDE_DIR})

ADD_LIBRARY (bindings_python3 SHARED solv_python.c)
SET_TARGET_PROPERTIES (bindings_python3 PROPERTIES PREFIX "" OUTPUT_NAME "_solv")
TARGET_LINK_LIBRARIES (bindings_python3 libsolvext libsolv ${SYSTEM_LIBRARIES})

INSTALL (TARGETS bindings_python3 LIBRARY DESTINATION ${PYTHON3_INSTALL_DIR})
INSTALL (FILES ${CMAKE_CURRENT_BINARY_DIR}/solv.py DESTINATION ${PYTHON3_INSTALL_DIR})
