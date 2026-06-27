#include <LuaKitty.hpp>

using namespace std;
using namespace copypasta;

int main(int argc, char** argv){
  if(argc < 2) {
    cout << "Please provide a lua script to execute" << endl;
    return 1;
  }
  LuaExecutor exec;
  exec.addArgs(argc, argv);
  if (strcmp(argv[1], "--watch") == 0 && argc > 2) {
      exec.watchAndExecThreaded(argv[2]);
      exec.joinWatcher();
  }
  else {
      exec.exec(argv[1], true);
  }
  return 0;
}

