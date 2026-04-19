#include <lib.hpp>
#include <loader.hpp>
#include <tree_sitter/api.h>

using namespace std;

DECLARE_TS_LANG(java)

extern "C" {
  #include "lua.h"
  #include "lauxlib.h"
  #include "lualib.h"
}


int main(int argc, char** argv){

  lua_State* L = luaL_newstate();
  luaL_openlibs(L);

  //lua_setglobal(L, "copy_pasta");

  Loader loader;
  loader.get("java");
  
  if(argc > 1){
    if (luaL_dofile(L, argv[1]) != LUA_OK) {
        std::cerr << "Error: " << lua_tostring(L, -1) << std::endl;
    }

  }else{
    cout << "no file" << endl;
  }
  lua_close(L);
  
  return 0;
}

