cmake_minimum_required(VERSION 3.10)

project(copyPasta VERSION 1.0 LANGUAGES C CXX)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# for windows /MD instead of /MT 
set(STATIC_CRT OFF CACHE BOOL "" FORCE) #libgit2
cmake_policy(SET CMP0091 NEW)

if(MSVC)
  # force /MD
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
      CACHE STRING "" FORCE)

  # disable MD warnings for depricated things
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
  set(USE_WINHTTP ON CACHE BOOL "" FORCE) #libgit2
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) 
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)        
set(BUILD_CLI OFF CACHE BOOL "" FORCE)        
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)   
set(USE_SSH OFF CACHE BOOL "" FORCE)           # SSH (requires libssh2) for libgit2
set(USE_HTTPS ON CACHE BOOL "" FORCE)         # OpenSSL/WinHTTP for libgit2
set(USE_BUNDLED_ZLIB ON CACHE BOOL "" FORCE)   # Use internal zlib 
set(USE_HTTP_PARSER "builtin" CACHE STRING "" FORCE)
set(USE_GSSAPI OFF CACHE BOOL "" FORCE)        # Disable network auth
set(USE_NTLMCLIENT OFF CACHE BOOL "" FORCE)    # Disable network auth

set(USE_PCRE OFF CACHE BOOL "" FORCE) # for libgit2
set(USE_REGEX OFF CACHE BOOL "" FORCE) # for libgit2

set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE) 

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) 
set(TREE_SITTER_BUILD_TESTS OFF CACHE BOOL "" FORCE) 

set(LINK_STATIC_TS_PARSER_LIBS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(
  #-Wall 
  #-Wextra 
  )

if(NOT MSVC)
  add_compile_options(-ggdb)
endif()

# exec

add_executable(
  copyPasta 
  main.cpp
)

# deps

add_subdirectory(deps/libgit2)
add_subdirectory(deps/pcre2)
add_subdirectory(deps/tree-sitter)
add_subdirectory(deps/lua-5.5.0)
add_subdirectory(deps/LuaBridge)

add_dependencies(
    copyPasta
    libgit2
    pcre2-8
    tree-sitter
    lua
)

if(LINK_STATIC_TS_PARSER_LIBS)
  add_library(TS_PARSER_LIB STATIC IMPORTED)
  # get the binaries from https://github.com/casualjim/breeze-tree-sitter-parsers
  if(WIN32)
      set_target_properties(TS_PARSER_LIB PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/deps/libtree-sitter-parsers-all-windows-x86_64/libtree-sitter-parsers-all-windows-x86_64.a
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
      )
  else()
      # this requires the entire app binary to be compiled with glibc clang/clang++
      set_target_properties(TS_PARSER_LIB PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/deps/libtree-sitter-parsers-all-linux-x86_64-glibc/libtree-sitter-parsers-all-linux-x86_64-glibc.a
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
      )
  endif()
endif()

find_package(Threads REQUIRED)

# include

target_include_directories(copyPasta PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/deps/libgit2/include
    ${CMAKE_SOURCE_DIR}/deps/pcre2/src
    ${CMAKE_SOURCE_DIR}/deps/tree-sitter/lib/include
    ${CMAKE_SOURCE_DIR}/deps/lua-5.5.0/src
    ${CMAKE_SOURCE_DIR}/deps/LuaBridge/Source
)

# Link

target_link_libraries(
  copyPasta 
  PUBLIC
  libgit2 libgit2package
  pcre2-8
  tree-sitter
  lua
  Threads::Threads
)

if(LINK_STATIC_TS_PARSER_LIBS)
    if(MSVC)
      target_link_libraries(copyPasta
            PUBLIC 
            "/WHOLEARCHIVE:$<TARGET_FILE:TS_PARSER_LIB>"
      )
    else()
      target_link_libraries(copyPasta
            PUBLIC
            "-Wl,--whole-archive"
            $<TARGET_FILE:TS_PARSER_LIB>
            "-Wl,--no-whole-archive" 
      )
    endif()
endif()

if(WIN32)
    target_link_libraries(copyPasta PUBLIC
        winhttp
        rpcrt4
        crypt32
        ole32
        ws2_32
    )
endif()

# Post

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
if(MSVC)
  message(STATUS "MSVC runtime: ${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

# build for windows - 
# sudo apt install mingw-w64
# cmake .. \
#   -DCMAKE_SYSTEM_NAME=Windows \
#   -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
#   -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
# prefer cmake .. -DCMAKE_TOOLCHAIN_FILE=../mingw-toolchain.cmake
