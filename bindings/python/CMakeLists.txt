find_package (Python2 COMPONENTS Interpreter Development)

SET (SWIG_PY_FLAGS ${SWIG_PY_FLAGS} -DSWIG_PYTHON_LEGACY_BOOL=1)

MESSAGE (STATUS "Python executable: ${PYTHON_EXECUTABLE}")
MESSAGE (STATUS "Python installation dir: ${Python2_STDARCH}")
MESSAGE (STATUS "Python include path: ${Python2_INCLUDE_DIRS}")

ADD_CUSTOM_COMMAND (
    OUTPUT solv_python.c
    COMMAND ${SWIG_EXECUTABLE} ${SWIG_FLAGS} -python ${SWIG_PY_FLAGS} -I${CMAKE_SOURCE_DIR}/src -o solv_python.c ${CMAKE_SOURCE_DIR}/bindings/solv.i
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${CMAKE_SOURCE_DIR}/bindings/solv.i
)

ADD_DEFINITIONS(-Wno-unused)
INCLUDE_DIRECTORIES (${Python2_INCLUDE_DIRS})

ADD_LIBRARY (bindings_python MODULE solv_python.c)
SET_TARGET_PROPERTIES (bindings_python PROPERTIES PREFIX "" OUTPUT_NAME "_solv")
TARGET_LINK_LIBRARIES (bindings_python libsolvext libsolv ${SYSTEM_LIBRARIES})

INSTALL (TARGETS bindings_python LIBRARY DESTINATION ${Python2_STDARCH})
INSTALL (FILES ${CMAKE_CURRENT_BINARY_DIR}/solv.py DESTINATION ${Python2_STDARCH})
