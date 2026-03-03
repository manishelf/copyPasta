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

  string from = ".setTupleTransformer((tuple, alias)->{ ... })";
  string to = ".setTupleTransformer((tuple, alias)->{ /* ... */ })";

  string qf = R"(
              (method_invocation
                  (identifier) @method_name
                  (#eq? @method_name "setTupleTransformer")
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

    t.find(q, [&t, &w, &edt](TSQueryMatch match) mutable{ 
      for(size_t i = 0; i < match.capture_count; i++ ){
        // method name
        if(match.captures[i].index == 0) continue;

        TSNode n = match.captures[i].node;

        TSRange change = TSEngine::getRange(n);
        change.start_byte +=1;
        change.start_byte +=1+2;
        edt.queue({FileEditor::OP::INSERT, change, {"", "/*"}});
        change.start_byte = change.end_byte - 1;
        change.end_byte = change.end_byte - 1+1;
        edt.queue({FileEditor::OP::INSERT, change, {"", "*/"}});
        change.start_point.row -= 1;
        change.end_point.row += 1;
        edt.queue({FileEditor::OP::PRINT_CHANGE, change, {t.getText(n), ""}});
      }
    });
    //edt.queue({FileEditor::OP::SAVE});
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
  
    return DirWalker::CONTINUE;
  });

  pool.waitUntilFinished();

  return 0;
}

int main(int argc, char** argv){

  return fn2(argv);
}

