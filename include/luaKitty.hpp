#ifndef LUA_KITTY_
#define LUA_KITTY_

extern "C" {
  #include "lua.h"
  #include "lauxlib.h"
  #include "lualib.h"
}
#include "LuaBridge/LuaBridge.h"
#include <lib.hpp>
#include <loader.hpp>
#include <string>


// API

class LuaExecutor {
  lua_State* L;
  void bind();
  public:
  LuaExecutor();
  ~LuaExecutor();

  void exec(std::string path);
};

// IMPL

LuaExecutor::LuaExecutor(){
  L = luaL_newstate();
  luaL_openlibs(L);
  bind();
}

LuaExecutor::~LuaExecutor(){
  lua_close(L);
}

void LuaExecutor::bind(){

  auto ns = luabridge::getGlobalNamespace(L);

  // LibGit
    ns.beginClass<LibGit>("Git")
        .addFunction("isIgnored", (bool(LibGit::*)(std::string)) &LibGit::isPathIgnored)
        .addFunction("addIgnore", &LibGit::addIgnoreRule)
        .addFunction("add", &LibGit::add)
    .endClass()
    .addFunction("gitClone", &LibGit::clone)
    .addFunction("gitOpen", &LibGit::open);

  // File
    ns.beginClass<File>("File")
        .addConstructor<void(*)(const std::string&)>()
        .addData("path", &File::pathStr)
        .addData("name", &File::name)
        .addData("ext", &File::ext)
        .addData("isDir", &File::isDir)
        .addData("isReg", &File::isReg)
        .addData("isValid", &File::isValid)
        .addData("size", &File::size)
        .addData("level", &File::level)
        //.addData(
        //   path
        //   status
        //   dir_entry
        //)
        //.addData("REPO")
        .addFunction("sync", &File::sync)
    .endClass()
    .addFunction("deleteFile", &File::deleteFile)
    .addFunction("deleteDir", &File::deleteDir)
    .addFunction("renameFileOrDir", &File::rename);
  // 
}

void LuaExecutor::exec(std::string path){
  if (luaL_dofile(L, path.c_str()) != LUA_OK) {
    throw std::runtime_error("Error executing " + path + " : " + + lua_tostring(L, -1));
  }
}

#endif
