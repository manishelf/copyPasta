#include "git2/signature.h"
#include <cctype>
#include <git2.h>
#include <iostream>
#include <lib.hpp>
#include <string>
#include <tree_sitter/api.h>
#include <assert.h>

using namespace std;

extern "C" {
const TSLanguage *tree_sitter_java(void);
}

int fn2(char** argv){
  
  string path = argv[1];
  ThreadPool pool;
  DirWalker walker(path);
  walker.recursive = true;
  LibGit git; // handle lib_git_init;

  string from = ".setTupleTransformer((tuple, alias)->{ ... })";
  string to = ".setTupleTransformer((tuple, alias)->{ /* ... */ })";

  string qf = R"(
              (method_invocation
                  (identifier) 
               arguments: (argument_list
                 (lambda_expression
                   parameters: (inferred_parameters)
                   body: (block) @lamda_block)))
                          )";

  const TSLanguage *lang = tree_sitter_java();

  walker.walk([lang, &qf](DirWalker::STATUS s, File f) {

    if(s ==DirWalker::QUEUING) return DirWalker::CONTINUE;

    if(f.ext != ".java")
      return DirWalker::CONTINUE;

    FileWriter w(f);
    FileEditor edt; 
    TSEngine eng(lang);

    // needs to be local to thread for cursors to work correctly
    // mightbe a bug as TSQuery is immutable according to docs 
    thread_local TSQuery* q = eng.queryNew(qf);

    CSTTree t = eng.parse(w); 

    t.find(q, [&w, &edt](TSQueryMatch match) mutable{ 
      for(size_t i = 0; i < match.capture_count; i++ ){
        TSNode n = match.captures[i].node;
        auto sb = ts_node_start_byte(n);
        auto eb = ts_node_end_byte(n);
        auto sp = ts_node_start_point(n);
        auto ep = ts_node_end_point(n);

        edt.queue({FileEditor::OP::INSERT, {sb+1, sb+1+2}, {"", "/*"}});
        edt.queue({FileEditor::OP::INSERT, {eb-1, eb-1+2}, {"", "*/"}});
        edt.queue({FileEditor::OP::PRINT_DIF, {w.rowOffsets[sp.row-1], w.rowOffsets[ep.row+1]}, {"TO", ""}});
      }
    });
    edt.queue({FileEditor::OP::SAVE});
    edt.queue({FileEditor::OP::VALIDATE_CST, {},{f.pathStr}});

    auto errors = edt.apply(t, w);

    for(auto err : errors ){
      size_t row , col;
      row = err.range.start_point.row;
      col = err.range.start_point.column;
      cout << "ERROR:" << edt.ERROR_STR[err.e] << endl;
      cout << f.pathStr <<":" << row << ":"<< col << endl;
    }

    edt.reset();
  
    return DirWalker::STOP;
  });

  pool.waitUntilFinished();

  return 0;
}

int main(int argc, char** argv){

  return fn2(argv);
}

