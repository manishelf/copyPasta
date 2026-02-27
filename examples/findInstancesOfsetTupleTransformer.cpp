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
  std::string path = argv[1];
  ThreadPool pool;
  DirWalker walker(path);
  walker.recursive = true;

  if (git_libgit2_init() < 0) {
    cerr << "unable to intialize libgit2" << endl;
  }

  string f = ".setTupleTransformer((tuple, alias)->{ })";

  string qf = "(method_invocation(identifier) @method_name                      \
               (argument_list(lambda_expression(inferred_parameters(identifier) \
               (identifier))(block) @lamda_block))) ";
  // use https://tree-sitter.github.io/tree-sitter/7-playground.html and
  // CSTTree::asQuery()

  const TSLanguage *lang = tree_sitter_java();

  TSQuery* q = TSEngine::queryNew(lang, qf); 

  walker.walk([q, lang](DirWalker::STATUS s, File f, void *p) {
    if(f.ext != ".java") return DirWalker::CONTINUE;

    FileReader r(f);
    TSEngine e(lang);
    CSTTree t = e.parse(r);
    for(auto match : t.find(q)){
      for(size_t i = 0; i < match.capture_count; i++){
        TSNode node = match.captures[i].node;
        if(ts_node_is_null(node)) continue;

        auto start = ts_node_start_point(node);
         auto end = ts_node_end_point(node);
         cout << f.pathStr << ":" << start.row <<":"<< start.column << "\n";
         cout << "----" << "\n";
         cout << r.get(ts_node_start_byte(node), ts_node_end_byte(node))<< "\n";
         cout << "----" << "\n";
      }
    };
    return DirWalker::CONTINUE;
  });

  ts_query_delete(q);
  pool.waitUntilFinished();
  git_libgit2_shutdown();

  return 0;
}
