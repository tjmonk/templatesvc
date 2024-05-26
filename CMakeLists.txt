cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

project(templatesvc
	VERSION 0.1
    DESCRIPTION "Service to render templates on triggers"
)

add_executable( ${PROJECT_NAME}
	src/templatesvc.c
)

target_include_directories( ${PROJECT_NAME}
	PRIVATE inc
)

target_link_libraries( ${PROJECT_NAME}
	${CMAKE_THREAD_LIBS_INIT}
	rt
	varserver
    tjson
)

install(TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

