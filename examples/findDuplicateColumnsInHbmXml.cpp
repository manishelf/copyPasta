#include <cctype>
#include <git2.h>
#include <iostream>
#include <lib.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

int main(int argc, char **argv) {
  std::string path = argv[1];
  DirWalker walker(path);
  walker.recursive = true;
  walker.inverted = false;
  vector<string_view> ignore;
  if (git_libgit2_init() < 0) {
    cerr << "unable to intialize libgit2" << endl;
  }

  walker.walk([](DirWalker::STATUS status, File file, void *pay) {
    if (file.name.rfind(".hbm.xml") == string::npos)
      return DirWalker::CONTINUE;

    FileReader reader(file);
    auto cdataStart = "<![CDATA[";
    auto cdataEnd = "]]>";
    auto start = reader.find(cdataStart);
    auto end = reader.find(cdataEnd);

    if (start.size() == 0)
      return DirWalker::CONTINUE;
    for (int i = 0; i < start.size(); i++) {
      auto startPos = start[i];
      auto endPos = end[i];
      set<string_view> lines;
      for (int i = startPos.match.start_point.row;
           i <= endPos.match.end_point.row; i++) {
        
        auto line = reader.getLine(i);

        if(line.find("select") != string::npos ||
            line.find("SELECT") != string::npos) lines.clear();

        auto it = lines.find(line);
        if (it != lines.end()) {
          size_t pos = line.size();
          // trim from right
          while (pos > 0 &&
                 std::isspace(static_cast<unsigned char>(line[pos - 1]))) {
            --pos;
          }

          // only lines that end with a ',' i.e columns
          if (it != lines.end() && pos > 0 && line[pos - 1] == ',') {
            cout << file.pathStr << ":" << (i + 1) << ":1: DUPLICATE: " << line
                 << endl;
          }
        }
        lines.insert(line);
      }
    }
    return DirWalker::CONTINUE;
  });

  git_libgit2_shutdown();

  return 0;
}
