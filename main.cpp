#include <filesystem>
#include <iostream>
#include <lib.hpp>
#include <git2.h>
using namespace std;

int main(int argc, char **argv) {
  std::string path = ".";
  DirWalker walker(path);
  walker.recursive = true;
  walker.inverted = false;
  vector<string_view> ignore;

	if (git_libgit2_init() < 0) {
    cerr << "unable to intialize libgit2" << endl;
	}
	
  walker.walk([](DirWalker::STATUS status, File file, void *pay) {
    cout << file.path << endl; 
    return DirWalker::CONTINUE;
  });
  
	git_libgit2_shutdown();

  return 0;
}
