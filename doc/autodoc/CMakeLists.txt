### ##################################################

FIND_PROGRAM( DOXYGEN doxygen )
IF ( NOT DOXYGEN )
  MESSAGE( FATAL_ERROR "doxygen not found: install doxygen to build the documentation." )
ELSE ( NOT DOXYGEN )
  MESSAGE( STATUS "doxygen found: ${DOXYGEN}" )
ENDIF ( NOT DOXYGEN )

FIND_PROGRAM( DOT dot )
IF ( NOT DOT )
   MESSAGE( "dot not found: you should install graphviz." )
   SET( HAVE_DOT NO )
ELSE ( NOT DOT )
   SET( HAVE_DOT YES )
ENDIF ( NOT DOT )

SET( SATSOLVER_SOURCE_DIR     ${CMAKE_SOURCE_DIR}/src )
SET( SATSOLVER_DOCINCLUDE_DIR ${CMAKE_SOURCE_DIR}/doc/autoinclude )
SET( SATSOLVER_EXAMPLE_DIR    ${CMAKE_SOURCE_DIR}/examples )

SET( DOXYGEN_INPUT    ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile )
SET( DOXYGEN_OUTPUT   ${CMAKE_CURRENT_BINARY_DIR}/html/index.html )
FILE( MAKE_DIRECTORY  ${CMAKE_CURRENT_BINARY_DIR}/html )

### ##################################################

CONFIGURE_FILE(
   ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.cmake
   ${DOXYGEN_INPUT}
   @ONLY
   )

### ##################################################

ADD_CUSTOM_COMMAND (
   OUTPUT  ${DOXYGEN_OUTPUT}
   COMMAND ${CMAKE_COMMAND} -E echo_append "Building Documentation..."
   COMMAND ${DOXYGEN} ${DOXYGEN_INPUT}
   COMMAND ${CMAKE_COMMAND} -E echo "Done."
   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
   DEPENDS ${DOXYGEN_INPUT}
)

ADD_CUSTOM_TARGET( doc ALL
   DEPENDS ${DOXYGEN_OUTPUT}
)

ADD_CUSTOM_TARGET( doc_forced
   COMMAND ${CMAKE_COMMAND} -E echo_append "Building Documentation..."
   COMMAND ${DOXYGEN} ${DOXYGEN_INPUT}
   COMMAND ${CMAKE_COMMAND} -E echo "Done."
   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
   DEPENDS ${DOXYGEN_INPUT}
)

### ##################################################

INSTALL( FILES
   ${CMAKE_CURRENT_BINARY_DIR}/satsolver.doxytag
   DESTINATION ${DOC_INSTALL_DIR}/satsolver/satsolver.doxytag
)

# We could use the thing below but it wont work with cmake older than 2.4.4
FILE(GLOB docfiles ${CMAKE_CURRENT_BINARY_DIR}/html/*)
INSTALL( FILES
   ${docfiles}
   DESTINATION ${DOC_INSTALL_DIR}/satsolver/html
)

#INSTALL( DIRECTORY
#   ${CMAKE_CURRENT_BINARY_DIR}/html
#   DESTINATION ${DOC_INSTALL_DIR}
#)
