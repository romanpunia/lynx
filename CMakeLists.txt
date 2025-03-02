cmake_minimum_required(VERSION 3.10)

#Setup toolchain file if needed
set(VI_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/vitex CACHE STRING "Vitex directory")
set(VI_BINDINGS OFF CACHE BOOL "-")
set(VI_FCONTEXT OFF CACHE BOOL "-")
set(VI_SQLITE OFF CACHE BOOL "-")
set(VI_POSTGRESQL OFF CACHE BOOL "-")
set(VI_MONGOC OFF CACHE BOOL "-")
set(VI_RAPIDJSON OFF CACHE BOOL "-")
set(VI_ANGELSCRIPT OFF CACHE BOOL "-")
set(VI_BACKWARDCPP OFF CACHE BOOL "-")
include(${VI_DIRECTORY}/deps/toolchain.cmake)

#Project's metadata
project(lynx)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

#Build directory if not defined
if (NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin)
endif()

#Project's compile options
add_executable(lynx ${CMAKE_CURRENT_SOURCE_DIR}/src/app.cpp)
set_target_properties(lynx PROPERTIES
    OUTPUT_NAME "lynx"
    CXX_STANDARD ${VI_CXX}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION})
if (MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "/ENTRY:mainCRTStartup /SUBSYSTEM:CONSOLE")
endif()

#Add Vitex as dependency
add_subdirectory(${VI_DIRECTORY} vitex)
target_link_libraries(lynx PRIVATE vitex)