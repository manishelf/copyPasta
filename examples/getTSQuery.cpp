#include <cctype>
#include <git2.h>
#include <iostream>
#include <lib.hpp>
#include <string>
#include <tree_sitter/api.h>

using namespace std;

extern "C" {
  const TSLanguage *tree_sitter_java(void);
}

int main(int argc, char **argv) {
 
  string f = ".setTupleTransformer((tuple, alias)->{ \
                                                     \
   })";

  string qf = "(method_invocation(identifier) @method_name (argument_list(lambda_expression(inferred_parameters(identifier)(identifier))(block) @lamda_block)))";
  // use https://tree-sitter.github.io/tree-sitter/7-playground.html and CSTTree::asQuery() 
  
  const TSLanguage* lang = tree_sitter_java();

  FileReader r(FileSnapshot{f});
  TSEngine e(lang);

  CSTTree t = e.parse(r.get(r.bufStart, r.bufSize));

  cout << r.get(r.bufStart, r.bufSize) << endl;
  cout << t.sTree() << endl;
  cout << t.asQuery() << endl;
  return 0;
}
