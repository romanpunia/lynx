cmake_minimum_required(VERSION 3.6)

#Setup toolchain file if needed
set(VI_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../mavi CACHE STRING "Mavi directory")
set(VI_USE_BINDINGS OFF CACHE BOOL "Script bindings are not used")
set(VI_USE_FAST_MEMORY ON CACHE BOOL "Fast memory is preferred for long running apps")
set(VI_USE_ASSIMP OFF CACHE BOOL "Assimp is not used")
set(VI_USE_FREETYPE OFF CACHE BOOL "FreeType is not used")
set(VI_USE_GLEW OFF CACHE BOOL "GLEW is not used")
set(VI_USE_OPENAL OFF CACHE BOOL "OpenAL is not used")
set(VI_USE_OPENGL OFF CACHE BOOL "OpenGL is not used")
set(VI_USE_SDL2 OFF CACHE BOOL "SDL2 is not used")
set(VI_USE_POSTGRESQL OFF CACHE BOOL "PostgreSQL is not used")
set(VI_USE_MONGOC OFF CACHE BOOL "MongoDB is not used")
set(VI_USE_SPIRV OFF CACHE BOOL "SPIR-V and Glslang are not used")
set(VI_USE_BULLET3 OFF CACHE BOOL "Bullet3 is not used")
set(VI_USE_RMLUI OFF CACHE BOOL "RmlUi is not used")
set(VI_USE_SHADERS OFF CACHE BOOL "Shaders are not used")
include(${VI_DIRECTORY}/lib/toolchain.cmake)

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

#Add Mavi as dependency
add_subdirectory(${VI_DIRECTORY} mavi)
link_directories(${VI_DIRECTORY})
target_include_directories(lynx PRIVATE ${VI_DIRECTORY})
target_link_libraries(lynx PRIVATE mavi)