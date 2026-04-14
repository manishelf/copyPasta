cmake_minimum_required(VERSION 3.10)

project(copyPasta VERSION 1.0 LANGUAGES C CXX)


# set

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) 
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)        
set(BUILD_CLI OFF CACHE BOOL "" FORCE)        
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)   
set(USE_SSH OFF CACHE BOOL "" FORCE)           # Disable SSH (requires libssh2)
set(USE_HTTPS OFF CACHE BOOL "" FORCE)         # Disable OpenSSL/WinHTTP
set(USE_BUNDLED_ZLIB ON CACHE BOOL "" FORCE)   # Use internal zlib 
set(USE_HTTP_PARSER "builtin" CACHE STRING "" FORCE)
set(USE_GSSAPI OFF CACHE BOOL "" FORCE)        # Disable network auth
set(USE_NTLMCLIENT OFF CACHE BOOL "" FORCE)    # Disable network auth

set(USE_PCRE OFF CACHE BOOL "" FORCE) # for libgit2
set(USE_REGEX OFF CACHE BOOL "" FORCE) # for libgit2

set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE) 

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) 
set(TREE_SITTER_BUILD_TESTS OFF CACHE BOOL "" FORCE) 

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(
  #-Wall 
  #-Wextra 
  -ggdb)

# exec

add_executable(
  copyPasta 
  main.cpp
)

# deps

add_subdirectory(deps/libgit2)
add_subdirectory(deps/pcre2)
add_subdirectory(deps/tree-sitter)
add_subdirectory(deps/tree-sitter-parsers/tree-sitter-java)
add_subdirectory(deps/lua-5.5.0)


add_dependencies(copyPasta libgit2 pcre2-8  tree-sitter tree-sitter-java lua)

find_package(Threads REQUIRED)

# include

target_include_directories(copyPasta PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/deps/libgit2/include
    ${CMAKE_SOURCE_DIR}/deps/pcre2/src
    ${CMAKE_SOURCE_DIR}/deps/tree-sitter/lib/include
    ${CMAKE_SOURCE_DIR}/deps/tree-sitter-parsers/tree-sitter-java/src
    ${CMAKE_SOURCE_DIR}/deps/lua-5.5.0/src
)

# Link

target_link_libraries(copyPasta PUBLIC
  libgit2 libgit2package
  pcre2-8
  tree-sitter
  tree-sitter-java
  lua
  Threads::Threads
)

# Post

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")


