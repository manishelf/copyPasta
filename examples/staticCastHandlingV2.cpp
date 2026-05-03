#include <iostream>
#include <lib.hpp>
#include <loader.hpp>
#include <string>
#include <tree_sitter/api.h>
#include <assert.h>

using namespace std;

// -------------------- Queries --------------------

string q_createNativeQuery = R"(
(method_invocation
  (identifier) @method_name
  ; eq dont work for some reason
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

string q_tupleTransformer = R"(
(method_invocation
    (identifier) @method_name
    ; eq dont work for some reason
    (#eq? @method_name "setTupleTransformer")
 arguments: 
   (argument_list
     (lambda_expression
       parameters: (inferred_parameters)
       body: (block) @lamda_block
     )
    )
)
)";

string q_castExpr = R"(
  	(cast_expression
        type: (type_identifier) @cast_type
        value: (array_access) @cast_value
      ) @cast 
)";

// -------------------- Helpers --------------------

string getWrapperForType(string_view type) {
  if (type == "Character" || type == "String") {
    return "StringUtilities.valueOf(";
  } 
  else if (type == "Long") {
    return "NumberUtilities.toLong(";
  } 
  else if (type == "Integer") {
    return "NumberUtilities.toInteger(";
  }
  return "";
}

bool isWithinContext(const TSRange& loc0, const TSRange& locHit, int searchContext) {
  return !((loc0.start_byte < locHit.start_byte || loc0.end_byte < locHit.end_byte)
           && (loc0.start_byte > locHit.start_byte + searchContext));
}

TSRange normalizeToRow(const TSRange& loc, int row, FileReader& r) {
  TSRange out = loc;

  out.start_point.row = row;
  out.end_point.row = row;

  int start = r.getRowOffsets()[row] + out.start_point.column;
  int len   = out.end_point.column - out.start_point.column;

  out.start_byte = start;
  out.end_byte   = start + len;

  return out;
}

// -------------------- Core Logic --------------------

void processCastExpressions(
    FileReader& r,
    FileEditor& edt,
    CSTTree& t,
    TSRange& locHit
) {
  TSQuery* tq_castExpr = TSQueryCache::global().get(t.getParent(), q_castExpr);

  t.find(tq_castExpr, [&](TSQueryMatch match_inner) {
    assert(match_inner.capture_count == 3);

    TSNode n0 = match_inner.captures[0].node;
    TSRange loc0 = TSEngine::getRange(n0);

    int searchContext = 100;
    if (!isWithinContext(loc0, locHit, searchContext)) return;

    int row = loc0.start_point.row;
    loc0 = normalizeToRow(loc0, row, r);

    // type
    TSNode n1 = match_inner.captures[1].node;
    TSRange loc1 = normalizeToRow(TSEngine::getRange(n1), row, r);
    string_view type = r.get(loc1.start_byte, loc1.end_byte);

    // value
    TSNode n2 = match_inner.captures[2].node;
    TSRange loc2 = normalizeToRow(TSEngine::getRange(n2), row, r);
    string_view value = r.get(loc2.start_byte, loc2.end_byte);

    string wrapper = getWrapperForType(type);
    if (wrapper.empty()) return;

    string newExpr = wrapper + string(value) + ")";

    edt.queue({ FileEditor::OP::WRITE, loc0, newExpr, "cast" });
  });
}

void processTupleTransformerMatches(
    FileReader& r,
    CSTTree& t,
    FileEditor& edt
) {
  TSQuery* tq_tupleTransformer = TSQueryCache::global().get(t.getParent(), q_tupleTransformer);
  t.find(tq_tupleTransformer, [&](TSQueryMatch match) {

    for (size_t i = 0; i < match.capture_count; i++) {
      TSNode n = match.captures[i].node;
      if (match.captures[i].index == 0) {
        if (t.getText(n) != "setTupleTransformer") break;
        continue;
      }
      if (match.captures[i].index == 1) {
        TSRange locHit = TSEngine::getRange(n);
        processCastExpressions(r, edt, t, locHit);
      }
    }
  });
}

void processCreateNativeQueryMatches(
    FileReader& r,
    CSTTree& t,
    FileEditor& edt
) {
  TSQuery* tq_createNativeQuery = TSQueryCache::global().get(t.getParent(), q_createNativeQuery);

  t.find(tq_createNativeQuery, [&](TSQueryMatch match) {
    for (size_t i = 0; i < match.capture_count; i++) {
      TSNode n = match.captures[i].node;

      if (match.captures[i].index == 0) {
        if (t.getText(n) != "createNativeQuery") break;
        continue;
      }

      if (match.captures[i].index == 1) continue;

      if (match.captures[i].index == 2) {
        if (t.getText(n) != "Object[].class") break;
        TSRange locHit = TSEngine::getRange(n);
        processCastExpressions(r, edt, t, locHit);
      }
    }
  });
}

void processFile(TSLangWrapper& lang, File f) {
  FileWriter w(f);
  FileReader r(f);
  FileEditor edt;

  TSEngine eng(lang.getLang()->getRaw());
  CSTTree t = eng.parse(r);

  processCreateNativeQueryMatches(r, t, edt);
  processTupleTransformerMatches(r, t, edt);

  //edt.queue({FileEditor::OP::PRINT_PATH});
  edt.queue({FileEditor::OP::VALIDATE_CST});
  //edt.queue({FileEditor::OP::PRINT_ERRORS});
  //edt.queue({FileEditor::OP::SAVE_VALID_ONLY});

  edt.applySaveAndMarkErrors(t, w);
  edt.reset();
}

int main(int argc, char** argv) {

  string path = argv[1];

  ThreadPool pool;
  DirWalker walker(path);
  walker.recursive = true;

  TSLoader l;
  TSLangWrapper lang = l.get("java");

  LibGit repo = LibGit::openOrInit(walker.path);
  string branch = "test-b-1";

  repo.resetHead();
  if (!repo.branchExists(branch)) {
    repo.branchCreate(branch);
  }
  repo.checkout(branch);

  walker.ignore.insert("**/hibernate/**");
  walker.ignore.insert("**/dep-jars/**");
  walker.ignore.insert("**/api/**");
  walker.ignore.insert("**/datatype/**");
  walker.matchExt.insert(".java");

  walker.walk(pool, [&lang](DirWalker::STATUS s, File f) {
    if (s == DirWalker::QUEUING) return DirWalker::CONTINUE;

    processFile(lang, f);
    return DirWalker::CONTINUE;
  });

  pool.waitUntilFinished();

  for (auto fdiff : repo.diff()) {
    cout << fdiff.newPath << endl;
    for (auto hunk : fdiff.hunks) {
      cout << hunk.header << endl;
      for (auto line : hunk.lineDiffs) {
        cout << (char)line.type << " ";
        cout << line.cont;
      }
    }
  }

  return 0;
}
