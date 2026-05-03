#include <lib.hpp>
#include <loader.hpp>
#include <luaKitty.hpp>
#include <tree_sitter/api.h>

using namespace std;

int main(int argc, char** argv){
  LuaExecutor exec;
  exec.addArgs(argc, argv);
  exec.watchAndExecThreaded(argv[1]);
  exec.joinWatcher();
  return 0;
}

