#include <cctype>
#include <iostream>
#include <lib.hpp>
#include <loader.hpp>
#include <string>
#include <tree_sitter/api.h>
#include <assert.h>

using namespace std;

DECLARE_TS_LANG(java)

int main(int argc, char** argv){
  
  string path = argv[1];
  ThreadPool pool;
  DirWalker walker(path);
  walker.recursive = true;

  string from = R"(
       result.equip_uid = (Integer) tuple[0];
       result.access_no = (String) tuple[1];
       result.tariff_code = (String) tuple[2];
       result.service_cd = (String) tuple[3];
       result.to_equip_uid = (Integer) tuple[4];
)";

  string to = R"(
       import com.cerillion.NumberUtilities;
       import com.cerillion.StringUtilities;
       ...
       result.equip_uid                 =  NumberUtilities.toInteger(tuple[0]);
       result.access_no                 =  StringUtilities.valueOf(tuple[1]);
       result.tariff_code               =  StringUtilities.valueOf(tuple[2]);
       result.service_cd                =  StringUtilities.valueOf(tuple[3]);
       result.to_equip_uid              =  StringUtilities.valueOf(tuple[4]);
  )";

  //xx.createNativeQuery("Query", Object[].class)
  string qf_2 = R"(
  (method_invocation
    (identifier) @method_name
    ; equals does not work for some reason
    (#eq? @method_name "createNativeQuery")
    arguments: (argument_list
      ([
        (string_literal)
        (binary_expression)
      ]) @first_arg
      (class_literal) @second_arg
      (#eq? @second_arg "Object[].class") 
    )
  )
  )";

  //(Type) row[0];
  string qf_3 = R"(
    	(cast_expression
          type: (type_identifier) @cast_type
          value: (array_access) @cast_value
        ) @cast 
  )";

  TSLoader l;

  TSLangWrapper lang = l.get("java");

  walker.matchExt.insert(".java");
  walker.walk([&lang, &qf_2, &qf_3](DirWalker::STATUS s, File f) {

    if(s == DirWalker::QUEUING) return DirWalker::CONTINUE;

    FileWriter w(f);
    FileReader r(f);
    FileEditor edt; 
    TSEngine eng(lang.getLang()->getRaw());

    thread_local TSQuery* q_2 = eng.queryNew(qf_2);

    CSTTree t = eng.parse(w); 


    t.find(q_2, [&eng, &qf_3, &t, &w, &r, &edt](TSQueryMatch match) mutable{ 
      for(size_t i = 0; i < match.capture_count; i++ ){
        TSNode n = match.captures[i].node;
        if(match.captures[i].index == 0){
          if(t.getText(n) != "createNativeQuery"){
            break;
          }
          continue;
        }
        if(match.captures[i].index == 1) continue;
        if(match.captures[i].index == 2) {
           if(t.getText(n) != "Object[].class"){
            break;
          }
          
          TSRange change = TSEngine::getRange(n);
          //edt.queue({FileEditor::OP::PRINT_CHANGE, change, {t.getText(n), ""}});
          thread_local TSQuery* q_3 = eng.queryNew(qf_3); 

          int context = 200; 
          // LEARNIGS: always assert when a assumption is made 
          // in this case the file is assumed to have more lines available when + context is done
          // this cause  a OOM error where os killed the process
          // this was due to parsing invalid string view pointing to data outside of array_access
          // for investigation of such issues use HTOP , gdb and add static counters and if to add breakpoints to debug
          // dont use pool when debugging

          for(int j = 1; j < context; j++){
            int row = change.start_point.row + j;
            string_view line = r.getLine(row);
            // TODO: there is a better way to do this by using query on the original tree
            // that is the recomended thing to do as what I am doing here is VERY inefficient
            // not that slow in practice though
            if(w.getFile().name == "ItemisedUsage.java"){
              int t = 0;
            }
            CSTTree tl = eng.parse(line);
            tl.find(q_3, [&row, &line, &r, &tl, &edt](TSQueryMatch match_inner){
                assert(match_inner.capture_count == 3);

                // full cast expression/
                TSNode n0 = match_inner.captures[0].node;
                TSRange loc0 = TSEngine::getRange(n0);
                loc0.start_point.row = row;
                loc0.end_point.row = row;

                int start0 = r.getRowOffsets()[row] + loc0.start_point.column;
                int len0   = loc0.end_point.column - loc0.start_point.column;

                loc0.start_byte = start0;
                loc0.end_byte = start0 + len0;

                // type_identifier
                TSNode n1 = match_inner.captures[1].node;
                TSRange loc1 = TSEngine::getRange(n1);
                loc1.start_point.row = row;
                loc1.end_point.row = row;

                int start1 = r.getRowOffsets()[row] + loc1.start_point.column;
                int len1   = loc1.end_point.column - loc1.start_point.column;

                loc1.start_byte = start1;
                loc1.end_byte = start1 + len1;

                std::string type(r.get(start1, start1 + len1));

                // value
                TSNode n2 = match_inner.captures[2].node;
                TSRange loc2 = TSEngine::getRange(n2);
                loc2.start_point.row = row;
                loc2.end_point.row = row;

                int start2 = r.getRowOffsets()[row] + loc2.start_point.column;
                int len2   = loc2.end_point.column - loc2.start_point.column;

                loc2.start_byte = start2;
                loc2.end_byte = start2 + len2;

                std::string_view text = r.get(start2, start2 + len2);

                // new version
                std::string wrapper;

                
                if (type == "Character") {
                  wrapper = "StringUtilities.valueOf(";
                }
                else 
                if(type == "String") {
                  wrapper = "StringUtilities.valueOf(";
                } 
                else
                if (type == "Long") {
                  wrapper = "NumberUtilities.toLong(";
                } 
                else 
                if (type == "Integer") {
                  wrapper = "NumberUtilities.toInteger(";
                }
                else 
                {
                  return;
                }

                string newWrapper;
                newWrapper.append(wrapper);
                newWrapper.append(text);
                newWrapper.append(")");

                //edt.queue({FileEditor::OP::PRINT_CHANGE_BEFORE, loc0, newWrapper, "cast"});
                //edt.queue({FileEditor::OP::WRITE, loc0, newWrapper, "cast"});
                edt.queue({FileEditor::OP::WRITE, loc0, newWrapper, "cast"});
                //edt.queue({FileEditor::OP::PRINT_CHANGE_AFTER, loc0, newWrapper, "cast"});

            });
          }
        }
      }
    });
//    edt.queue({FileEditor::OP::VALIDATE_CST, {},{f.pathStr}});
//    edt.queue({FileEditor::OP::PRINT_ERRORS});
    edt.queue({FileEditor::OP::SAVE});

    edt.apply(t, w);
    edt.reset();
  
    return DirWalker::CONTINUE;
  });

    LibGit repo = LibGit::open(walker.path);
  
  for(auto fdiff : repo.diff()){
    cout << fdiff.newPath << endl;
    for(auto hunk : fdiff.hunks){
      cout << hunk.header << endl;
      for(auto line: hunk.lineDiffs){
        cout << (char)line.type << " ";
        cout << line.cont;
      }
    }
  }


  pool.waitUntilFinished();

  return 0;
}

