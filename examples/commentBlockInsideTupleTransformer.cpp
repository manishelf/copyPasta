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

int fn1() {

  string p = "example.java";

  string qf = R"(
              (method_invocation
                  (identifier)
               arguments: (argument_list
                 (lambda_expression
                   parameters: (inferred_parameters)
                   body: (block) @lamda_block)))
                          )";

  const TSLanguage *lang = tree_sitter_java();
  TSQuery* q = TSEngine::queryNew(lang, qf);
 
    FileReader r(p);
    FileWriter w(r.snapshot());
    FileEditor edt;

    TSEngine eng(lang);
    CSTTree t = eng.parse(r); 

    assert(t.getErrors().size() == 0);

    t.find(q, [&w, &edt, &r](TSQueryMatch match) mutable{ 
      for(size_t i = 0; i < match.capture_count; i++ ){
        TSNode n = match.captures[i].node;
        auto sb = ts_node_start_byte(n);
        auto eb = ts_node_end_byte(n);
        auto sp = ts_node_start_point(n);
        auto ep = ts_node_end_point(n);
        string comStart = "/*";
        string comEnd = "*/";
        edt.queue({FileEditor::OP::INSERT, {sb+1, sb+1+2}, {"", comStart}});
        edt.queue({FileEditor::OP::INSERT, {eb-1, eb-1+2}, {"", comEnd}});
        edt.queue({FileEditor::OP::PRINT_DIF, {sb-100, eb+100}, {"TO", ""}});
      }
    });

    edt.queue({FileEditor::OP::SAVE, {}, {}});

    for(auto err : edt.apply(t, w)){
      cout << "ERROR:" << err.e << edt.OP_STR[err.edit.op]
       <<"-" << err.edit.range[0] <<"," << err.edit.range[1] << endl;
    };

    edt.reset();
  
  return 0;
}

int fn2(char** argv){
  std::string path = argv[1];
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
  TSQuery* q = TSEngine::queryNew(lang, qf);
  walker.walk(pool, [lang, q](DirWalker::STATUS s, File f) {
    if(f.ext != ".java")
      return DirWalker::CONTINUE;

    FileReader r(f);
    FileWriter w(r.snapshot());
    FileEditor edt;

    TSEngine eng(lang);
    CSTTree t = eng.parse(r); 
    t.find(q, [&w, &edt, &r](TSQueryMatch match) mutable{ 
      for(size_t i = 0; i < match.capture_count; i++ ){
        TSNode n = match.captures[i].node;
        auto sb = ts_node_start_byte(n);
        auto eb = ts_node_end_byte(n);
        auto sp = ts_node_start_point(n);
        auto ep = ts_node_end_point(n);
        string comStart = "/*";
        string comEnd = "*/";
        edt.queue({FileEditor::OP::INSERT, {sb+1, sb+1+2}, {"", comStart}});
        edt.queue({FileEditor::OP::INSERT, {eb-1, eb-1+2}, {"", comEnd}});
        edt.queue({FileEditor::OP::PRINT_DIF, {sb-100, eb+100}, {"TO", ""}});
      }
    });

    edt.queue({FileEditor::OP::SAVE, {}, {}});

    for(auto err : edt.apply(t, w)){
      cout << "ERROR:" << err.e << edt.OP_STR[err.edit.op]
        << err.edit.range[0] << err.edit.range[1] << endl;
    };

    edt.reset();
  
    return DirWalker::CONTINUE;
  });

  ts_query_delete(q);

  pool.waitUntilFinished();

  return 0;
}

int main(int argc, char** argv){

  return fn1();
}

