#ifndef LUA_KITTY_
#define LUA_KITTY_

extern "C" {
  #include "lua.h"
  #include "lauxlib.h"
  #include "lualib.h"
}
#include "LuaBridge/LuaBridge.h"
#include <lib.hpp>
#include <loader.hpp>
#include <string>
#include <vector>
#include <unordered_set>

// TODO: remove this
using namespace luabridge;

// API

class LuaExecutor {
  
  lua_State* L;
  void bind();
  void bindFile();
  void bindReader();
  void bindWriter();
  void bindWalk();
  void bindLanguage();
  void bindTree();
  void bindEditor();
  void bindGit();
  void bindLogger();
  void bindHelpers();
  void updateLuaArgs();

  bool watcherRunning = false;
  ThreadPool pool;
  void watchAndExec(const std::string& path, int pollIntervalMs);
public:

  std::vector<std::string> args;
  void addArgs(int argc, char** argv);

  LuaExecutor();
  ~LuaExecutor();
  void exec(std::string path);

  void watchAndExecThreaded(const std::string& path, int pollIntervalMs = 1000);
  void joinWatcher();

};


// IMPL

LuaExecutor::LuaExecutor():pool(1){
  L = luaL_newstate();
  luaL_openlibs(L);
  bind();
}

LuaExecutor::~LuaExecutor() {
  watcherRunning = false;
  joinWatcher(); // wait for existing threads to complete
  lua_close(L);
}

void LuaExecutor::addArgs(int argc, char** argv){
  if(argv == nullptr) return;
  for(int i = 0; i < argc; i++){
    args.emplace_back(argv[i]);
  }
  updateLuaArgs();
}

void LuaExecutor::exec(std::string path){
  INFO("LuaExecutor exec - " << path);
  lua_getglobal(L, "debug");
  lua_getfield(L, -1, "traceback");

  int errFuncIndex = lua_gettop(L);

  // Load file
  if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
    std::string err = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error("Lua load error in " + path + ":\n" + err);
  }

  // Call with traceback
  if (lua_pcall(L, 0, LUA_MULTRET, errFuncIndex) != LUA_OK) {
    std::string err = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error("Lua runtime error in " + path + ":\n" + err);
  }

  // Clean up error handler
  lua_remove(L, errFuncIndex);
}

void LuaExecutor::watchAndExec(const std::string& path, int pollIntervalMs) {
  watcherRunning = true;
  fs::file_time_type lastWrite = fs::last_write_time(path);
  bool execDoneOnce = false;

  while (watcherRunning) {
    std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    auto nowWrite = fs::last_write_time(path);
    if (nowWrite != lastWrite || !execDoneOnce) {
      lastWrite = nowWrite;
      try {
       this->exec(path);
      } catch (const std::exception& e) {
        ERROR("[LuaExecutor] " << e.what());
      }
      execDoneOnce = true;
    }
  }
}

void LuaExecutor::watchAndExecThreaded(const std::string& path, int pollIntervalMs) {
  pool.enqueue([this, path, pollIntervalMs](){
     this->watchAndExec(path, pollIntervalMs);
  });
}

void LuaExecutor::joinWatcher() {
  pool.waitUntilFinished(); 
}

void LuaExecutor::updateLuaArgs(){
  LuaRef cmdArgs = newTable(L);
  for (size_t i = 0; i < args.size(); ++i) {
    cmdArgs[i + 1] = args[i];  // Lua is 1-indexed
  }

  setGlobal(L, cmdArgs, "cmdArgs");
}

void LuaExecutor::bind(){
  bindFile();
  bindReader();
  bindWriter();
  bindWalk();
  bindLanguage();
  bindTree();
  bindEditor();
  bindGit();
  bindLogger();
  bindHelpers();
}

namespace LKHelpers {
  
  TSRange capToRange(const luabridge::LuaRef& cap) {
    TSRange r{};
    r.start_byte = cap["startByte"].cast<uint32_t>();
    r.end_byte = cap["endByte"].cast<uint32_t>();
    r.start_point.row = cap["row"].cast<uint32_t>();
    r.start_point.column = cap["col"].cast<uint32_t>();
    r.end_point.row = cap["endRow"].cast<uint32_t>();
    r.end_point.column = cap["endCol"].cast<uint32_t>();
    return r;
  }

  LuaRef rangeToCap(lua_State* L, TSRange r){
    LuaRef match = newTable(L);
    match["startByte"] = r.start_byte;
    match["endByte"] = r.end_byte;
    match["row"] = r.start_point.row;
    match["col"] = r.start_point.column;
    match["endRow"] = r.end_point.row;
    match["endCol"] = r.end_point.column; 
    return match;
  }

  LuaRef matchToCap(lua_State* L, FileReader* r, std::vector<FileReader::MatchResult> matches){
    LuaRef table = newTable(L);
    for (size_t i = 0; i < matches.size(); ++i) {
      auto hit = matches[i];
      auto sv = r->get(hit.match.start_byte, hit.match.end_byte);
      LuaRef match  = LKHelpers::rangeToCap(L, matches[i].match);
      match["path"] = r->getFile().pathStr;
      match["text"] = std::string(sv.data(), sv.size());
      LuaRef captures = newTable(L);
      for(int i = 0; i< hit.captures.size(); i++){
        LuaRef capture = LKHelpers::rangeToCap(L, hit.captures[i]); // Lua is 1 based;
        auto sv = r->get(hit.captures[i].start_byte, hit.captures[i].end_byte);
        capture["text"] = std::string(sv.data(), sv.size());
        captures[i+1] = capture;
      }
      match["captures"] = captures;
      table[i + 1] = match; // Lua is 1 based
    }
    return table;
  }

  LuaRef makeErrorTable(lua_State* L, const std::vector<FileEditor::Error>& errs) {
    LuaRef errTable = newTable(L);
    for (size_t i = 0; i < errs.size(); ++i) {
      const auto& e = errs[i];
      LuaRef err = newTable(L);
      err["type"] = (e.e == FileEditor::CONFLICT) ? "CONFLICT"
                  : (e.e == FileEditor::CST_ERROR ? "CST_ERROR" : "CST_MISSING");
      err["startRow"] = e.range.start_point.row;
      err["startCol"] = e.range.start_point.column;
      err["endRow"] = e.range.end_point.row;
      err["endCol"] = e.range.end_point.column;
      LuaRef editTable = newTable(L);
      editTable["op"] = (int)e.edit.op;
      editTable["change"] = e.edit.change;
      editTable["context"] = e.edit.context;
      editTable["id"] = e.edit.id;
      err["edit"] = editTable;
      errTable[i + 1] = err;
    }
    return errTable;
  }

  LuaRef makeCapture(lua_State* L, TSNode node, const std::string_view source, 
                                       TSQuery* query, uint32_t index) {
    uint32_t nameLen = 0;
    const char* name = ts_query_capture_name_for_id(query, index, &nameLen);
    uint32_t sb = ts_node_start_byte(node);
    uint32_t eb = ts_node_end_byte(node);
    TSPoint sp = ts_node_start_point(node);
    TSPoint ep = ts_node_end_point(node);
    
    LuaRef cap = newTable(L);
    cap["text"] = std::string((sb <= eb && eb <= source.size()) ? source.substr(sb, eb - sb) : "");
    cap["name"] = std::string(name, nameLen);
    cap["row"] = sp.row;
    cap["col"] = sp.column;
    cap["endRow"] = ep.row;
    cap["endCol"] = ep.column;
    cap["startByte"] = sb;
    cap["endByte"] = eb;
    return cap;
  }

  std::string luaRefToString(lua_State* L, luabridge::LuaRef v) {
  
    if (v.isNil()) return "nil";
    if (v.isBool()) return v.cast<bool>() ? "true" : "false";
    if (v.isNumber()) return std::to_string(v.cast<double>());
    if (v.isString()) return v.cast<std::string>();
  
    if (v.isFunction()) return "<function>";
    if (v.isUserdata()) return "<userdata>";
    if (v.isTable()) return "<table>";
  
    return v.tostring();
  }

  void luaTableIterRecursive(
      lua_State* L,
      LuaRef t,
      const std::function<void(const std::string& keyPath, const std::string& value, int depth)>& fn,
      std::unordered_set<const void*>& visited,
      const std::string& prefix = "",
      int depth = 0
  ) {
    if (!t.isTable()) return;
  
    if (visited.count(&t)) return; // cycle guard
    visited.insert(&t);
  
    t.push(L);
    lua_pushnil(L);
  
    while (lua_next(L, -2)) {
  
      luabridge::LuaRef value = luabridge::LuaRef::fromStack(L, -1);
      lua_pop(L, 1);
  
      luabridge::LuaRef key = luabridge::LuaRef::fromStack(L, -1);
  
      std::string keyStr = key.tostring();
      std::string path = prefix.empty() ? keyStr : prefix + "." + keyStr;
  
      if (value.isTable()) {
        luaTableIterRecursive(L, value, fn, visited, path, depth + 1);
      } else {
        fn(path, luaRefToString(L, value), depth);
      }
    }
  
    lua_pop(L, 1);
  }

  static void luaTableIter(
      lua_State* L,
      luabridge::LuaRef t,
      const std::function<void(const std::string&, const std::string&, int)>& fn
      ) {
    std::unordered_set<const void*> visited;
    luaTableIterRecursive(L, t, fn, visited);
  }

  LuaRef makeLineDiff(lua_State* L, const LibGit::LineDiff& ld) {
    luabridge::LuaRef t = luabridge::newTable(L);
    t["type"] = std::string(1, (char)ld.type);
    t["oldLineNo"] = ld.oldLineNo;
    t["newLineNo"] = ld.newLineNo;
    t["fileOffset"] = ld.fileOffset;
    t["text"] = ld.cont;
    t["author"] = ld.blameAuthor;
    t["email"] = ld.blameEmail;
    t["commit"] = ld.blameCommit;
    return t;
  }
 
  LuaRef makeHunk(lua_State* L, const LibGit::Hunk& h) {
    luabridge::LuaRef t = luabridge::newTable(L);
    t["oldStart"]  = h.oldStartLine;
    t["oldLines"]  = h.oldLinesCount;
    t["newStart"]  = h.newStartLine;
    t["newLines"]  = h.newLinesCount;
    t["header"]    = h.header;

    luabridge::LuaRef lines = luabridge::newTable(L);
    for (size_t i = 0; i < h.lineDiffs.size(); ++i){
      lines[i + 1] = makeLineDiff(L, h.lineDiffs[i]);
    }
    
    t["lines"] = lines;
    return t;
  }
 
  LuaRef makeFileDiff(lua_State* L, const LibGit::FileDiff& fd) {
    luabridge::LuaRef t = luabridge::newTable(L);
    t["oldPath"] = fd.oldPath;
    t["newPath"] = fd.newPath;
    t["status"]  = (int)fd.status;
    t["flags"]   = (int)fd.flags;
    
    luabridge::LuaRef hunks = luabridge::newTable(L);
    for (size_t i = 0; i < fd.hunks.size(); ++i){
      hunks[i + 1] = makeHunk(L, fd.hunks[i]);
    }

    t["hunks"] = hunks;
    return t;
  }

}

void LuaExecutor::bindFile(){
  
  Namespace ns = getGlobalNamespace(L);
  
  ns.beginClass<File>("File")
    .addConstructor<void(*)(const std::string&)>()
      .addData("path", &File::pathStr)
      .addData("name", &File::name)
      .addData("ext", &File::ext)
      .addData("isDir", &File::isDir)
      .addData("isReg", &File::isReg)
      .addData("isValid", &File::isValid)
      .addData("size", &File::size)
      .addData("level", &File::level)
      .addFunction("sync", &File::sync)
    .endClass()
    .addFunction("deleteFile", &File::deleteFile)
    .addFunction("deleteDir", &File::deleteDir)
    .addFunction("rename", +[](File* f, const std::string& name) -> bool { 
      return File::rename(*f, name); 
    });
}

void LuaExecutor::bindReader(){

  Namespace ns = getGlobalNamespace(L);
  
  ns.beginClass<FileReader>("FileReader")
    .addConstructor<void(*)(std::string)>()

      .addFunction("isValid", &FileReader::isValid)
      .addFunction("sync", &FileReader::sync)

      .addFunction("get", +[](FileReader* r) {
        auto sv = r->get();
        return std::string(sv.data(), sv.size());
      })
      .addFunction("getRange", +[](FileReader* r, size_t from, size_t to) {
        auto sv = r->get(from, to);
        return std::string(sv.data(), sv.size());
      })
      .addFunction("getLine", +[](FileReader* r, size_t row) {
        auto sv = r->getLine(row);
        return std::string(sv.data(), sv.size());
      })
      .addFunction("getRowStart", +[](FileReader* r, size_t row) {
        return r->getRowOffsets()[row];
      })
       .addFunction("getIndent", +[](FileReader* r, size_t row) {
        return r->getIndent(row);
      })

      .addFunction("find", +[](FileReader* r, const std::string& pattern, bool regex, lua_State* L) {
        auto results = r->find(pattern, regex);
        return LKHelpers::matchToCap(L, r, results);
      })
    .endClass()
    .addFunction("read", +[](const std::string& path) {
       return FileReader(path);
    })
    .addFunction("readSnap", +[](const std::string& cont) {
       FileSnapshot s;
       s.cont = cont;
       return FileReader(cont);
    });
}

void LuaExecutor::bindWriter(){

  Namespace ns = getGlobalNamespace(L);

  ns.beginClass<FileWriter>("FileWriter")
     .addConstructor<void(*)(std::string)>()
     .addFunction("isValid", &FileWriter::isValid)

     .addFunction("save", &FileWriter::save)
     .addFunction("backup", +[](FileWriter* w, const std::string suffix){
        w->backup(suffix);
     })
     .addFunction("writeTo", &FileWriter::writeTo)
     
     .addFunction("append", &FileWriter::append)
     .addFunction("insert", &FileWriter::insert)
     .addFunction("insertRowBefore", &FileWriter::insertRowBefore)
     .addFunction("insertRowAfter", &FileWriter::insertRowAfter)
     
     .addFunction("deleteRow", &FileWriter::deleteRow)
     .addFunction("deleteCont", &FileWriter::deleteCont)
     
     .addFunction("writeAt", +[](FileWriter* w, size_t offset, std::string str) {
       w->write(offset, str); 
     })
    
     .addFunction("replace", &FileWriter::replace)
     .addFunction("replaceAll", &FileWriter::replaceAll)
   .endClass()
   .addFunction("write", +[](const std::string& path){
     return FileWriter(path);
   });
}

void LuaExecutor::bindWalk(){

  Namespace ns = getGlobalNamespace(L);

  ns.addFunction("walk", +[](const std::string& path, LuaRef opts, LuaRef callback) {
    DirWalker walker(path);
    walker.recursive = opts["recursive"].cast<bool>();
    walker.inverted = opts["inverted"].cast<bool>();
    walker.filesOnly = opts["filesOnly"].cast<bool>();
    walker.obeyGitIgnore = !opts["doNotObeyGitIgnore"].cast<bool>();

    
    // TODO: thread pool
    bool usePool = opts["usePool"].cast<bool>();
    
    if (opts["ext"].isTable()) {
      for (auto it : pairs(opts["ext"])) {
        walker.matchExt.insert(it.second.cast<std::string>());
      }
    }
    
    if (opts["ignore"].isTable()) {
      for (auto it : pairs(opts["ignore"])) {
        walker.ignore.insert(it.second.cast<std::string>());
      }
    }

    walker.walk([&callback](DirWalker::STATUS status, File file, LibGit& git) {
      LuaRef result = callback(&file, &git);
      if (result.isNumber()) {
        int rv = result.cast<int>();
        if (rv == (int)DirWalker::STOP) return DirWalker::STOP;
        if (rv == (int)DirWalker::ABORT) return DirWalker::ABORT;
        if (rv == (int)DirWalker::SKIP) return DirWalker::SKIP;
      }
      return DirWalker::CONTINUE;
    });
  })
  .addFunction("findInFiles", +[](const std::string& path, const std::string& pattern, 
                                  LuaRef opts, lua_State* L) -> LuaRef {
    DirWalker walker(path);
    walker.recursive = true;
    walker.filesOnly = true;
    
    if (opts.isTable()) {
      if (opts["ext"].isTable()) {
        for (auto it : pairs(opts["ext"])) {
          walker.matchExt.insert(it.second.cast<std::string>());
        }
      }
    }

    
    std::vector<LuaRef> hits;
    std::mutex hitMutex;
    ThreadPool pool;

    walker.walk(pool, [L, &hitMutex, &hits, &pattern](DirWalker::STATUS status, File file, LibGit&) {
      if (status == DirWalker::QUEUING) return DirWalker::CONTINUE;
      FileReader reader(file);
      auto results = reader.find(pattern, true, PCRE2_MULTILINE);
      if (!results.empty()) {
        std::lock_guard<std::mutex> lock(hitMutex);
        auto res = LKHelpers::matchToCap(L, &reader, results);
        hits.push_back(res);
      }

      return DirWalker::CONTINUE;
    });
    
    pool.waitUntilFinished();

    LuaRef results = newTable(L);
    
    for (size_t i = 0; i < hits.size(); ++i) {
      results[i + 1] = hits[i];
    }
    return results;
  });
}

void LuaExecutor::bindLanguage(){

  Namespace ns = getGlobalNamespace(L);

  ns.beginClass<TSLangWrapper>("Language")
      .addFunction("parse", +[](TSLangWrapper* lang, const std::string& input){
        std::string source;
        assert(!input.empty());
        return TSEnginePool::global().get(lang->getLang()->getRaw())->parse(input);
      })
      .addFunction("parseFile", +[](TSLangWrapper* lang, const std::string& path){
        std::string source;
        assert(!path.empty());
        FileReader reader(path);
        return TSEnginePool::global().get(lang->getLang()->getRaw())->parse(reader);
      })
      .addFunction("getNodeTypes", +[](TSLangWrapper* lang){
          auto eng = TSEnginePool::global().get(lang->getLang()->getRaw());
          return eng->getAvailableNodeTypes();
      })
    .endClass()
    .addFunction("loadLanguage", +[](const std::string& langName) {
      TSLoader loader;
      TSLangWrapper tsLang = loader.get(langName);
      return tsLang;
    });
}

void LuaExecutor::bindTree(){

  Namespace ns = getGlobalNamespace(L);

  ns.beginClass<CSTTree>("Tree")
    .addFunction("sexp", &CSTTree:: asSexpr)
    .addFunction("asQuery", &CSTTree::asQuery)
    .addFunction("getErrors", +[](CSTTree* t, lua_State* L){
      auto errors = t->getErrors();
      LuaRef res = newTable(L);
      for(int i = 0; i < errors.size(); i++){
        res[i+1] = LKHelpers::rangeToCap(L, errors[i]);
      } 
      return res;
    })
    .addFunction("query", +[](CSTTree* t, const std::string& expr, LuaRef callback) {
      lua_State* L = callback.state();
      auto parent = t->getParent();
      TSQuery* query = TSQueryCache::global().get(parent, expr);
      t->find(query, [t, query, callback, L](TSQueryMatch match){
        LuaRef captures = newTable(L);
        for (uint32_t i = 0; i < match.capture_count; ++i) {
          TSNode node = match.captures[i].node;
          uint32_t nameLen = 0;
          const char* name = ts_query_capture_name_for_id(query, match.captures[i].index, &nameLen);
          LuaRef cap = LKHelpers::makeCapture(L, node, t->getSource(), query, match.captures[i].index);
          captures[std::string(name, nameLen)] = cap;
        }
        callback(captures);
      });
    })
  .endClass();
}

void LuaExecutor::bindEditor(){
  
  Namespace ns = getGlobalNamespace(L);

  ns.beginClass<FileEditor>("Editor")
    .addConstructor<void(*)()>()
    .addFunction("insertBefore", +[](FileEditor* ed, LuaRef cap, const std::string& text) {
      TSRange r = LKHelpers::capToRange(cap);
      r.end_byte = r.start_byte;
      r.end_point = r.start_point;
      std::string context = cap["context"].cast<std::string>();
      ed->queue({ FileEditor::OP::INSERT, r, text, context });
    })
    .addFunction("insertAfter", +[](FileEditor* ed, LuaRef cap, const std::string& text) {
      TSRange r = LKHelpers::capToRange(cap);
      r.start_byte = r.end_byte;
      r.start_point = r.end_point;
      std::string context = cap["context"].cast<std::string>();
      ed->queue({ FileEditor::OP::INSERT, r, text, context });
    })
    .addFunction("insertRowBefore", +[](FileEditor* ed, luabridge::LuaRef cap) {
      TSRange r    = LKHelpers::capToRange(cap);
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>(); 
      ed->queue({ FileEditor::OP::INSERT_ROW_BEFORE, r, change, context });
    })
    .addFunction("insertRowAfter", +[](FileEditor* ed, luabridge::LuaRef cap) {
      TSRange r    = LKHelpers::capToRange(cap);
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>(); 
      ed->queue({ FileEditor::OP::INSERT_ROW_AFTER, r, change, context });
    })
    .addFunction("write", +[](FileEditor* ed, LuaRef cap, const std::string& text) {
      TSRange r = LKHelpers::capToRange(cap);
      std::string context = cap["context"].cast<std::string>();
      ed->queue({ FileEditor::OP::WRITE, r, text, context });
    })
    .addFunction("replace", +[](FileEditor* ed, LuaRef cap, 
                                const std::string& pattern, const std::string& tpl) {
      TSRange r = LKHelpers::capToRange(cap);
      ed->queue({ FileEditor::OP::REPLACE, r, tpl, pattern });
    })
    .addFunction("delete", +[](FileEditor* ed, LuaRef cap) {
      TSRange r = LKHelpers::capToRange(cap);
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>();
      ed->queue({ FileEditor::OP::DELETE, r, change, context });
    })
    .addFunction("deleteWithPad", +[](FileEditor* ed, LuaRef cap, uint32_t pad) {
      TSRange r = LKHelpers::capToRange(cap);
      r.start_byte = r.start_byte > pad ? r.start_byte - pad : 0;
      r.end_byte += pad;
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>();
      ed->queue({ FileEditor::OP::DELETE, r, change , context });
    })
    .addFunction("printBefore", +[](FileEditor* ed, LuaRef cap) {
      TSRange r = LKHelpers::capToRange(cap);
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>(); 
      ed->queue({ FileEditor::OP::PRINT_CHANGE_BEFORE, r, change, context });
    })
    .addFunction("printAfter", +[](FileEditor* ed, LuaRef cap) {
      TSRange r = LKHelpers::capToRange(cap);
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>(); 
      ed->queue({ FileEditor::OP::PRINT_CHANGE_AFTER, r, change, context });
    })
    .addFunction("mark", +[](FileEditor* ed, luabridge::LuaRef cap) {
      TSRange r = LKHelpers::capToRange(cap);
      std::string text = cap["change"].cast<std::string>();
      std::string info = cap["context"].cast<std::string>(); 
      ed->queue({ FileEditor::OP::MARK, r, text, info });
    })
    .addFunction("validate", +[](FileEditor* ed, LuaRef cap) {
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>(); 
      ed->queue({ FileEditor::OP::VALIDATE_CST , {}, change, context});
    })
    .addFunction("printErrors", +[](FileEditor* ed, LuaRef cap) {
      std::string change = cap["change"].cast<std::string>();
      std::string context = cap["context"].cast<std::string>(); 
      ed->queue({ FileEditor::OP::PRINT_ERRORS, {}, change, context });
    })
    .addFunction("backup", +[](FileEditor* ed, LuaRef cap, const std::string& suffix) {
      ed->queue({ FileEditor::OP::BACKUP, {}, suffix });
    })
    .addFunction("writeTo", +[](FileEditor* ed, const std::string& path) {
      ed->queue({ FileEditor::OP::WRITE_TO, TSRange{}, path, "" });
    })
    .addFunction("queueSave", +[](FileEditor* ed, const std::string& path) {
      ed->queue({ FileEditor::OP::SAVE });
    })
    .addFunction("getErrors", +[](FileEditor* ed, lua_State* L) -> luabridge::LuaRef {
      auto errs = ed->getErrors();
      return LKHelpers::makeErrorTable(L, errs);
    })
    .addFunction("getConflicts", +[](FileEditor* ed, lua_State* L) -> luabridge::LuaRef {
      auto errs = ed->getConflictErrors();
      return LKHelpers::makeErrorTable(L, errs);
    })
    .addFunction("cancel", &FileEditor::delEdit)
    .addFunction("reset", &FileEditor::reset)
    .addFunction("apply", +[](FileEditor* ed, CSTTree* t, FileWriter* w, lua_State* L) -> LuaRef {
      auto errs = ed->apply((*t), (*w));
      return LKHelpers::makeErrorTable(L, errs);
    })
    .addFunction("applyAndSave", +[](FileEditor* ed, CSTTree* t, FileWriter* w, lua_State* L) -> LuaRef {
      ed->queue({ FileEditor::OP::SAVE });
      auto errs = ed->apply((*t), (*w));
      return LKHelpers::makeErrorTable(L, errs);
    })
    .addFunction("applyAndSaveValidOnly", +[](FileEditor* ed, CSTTree* t, FileWriter* w, lua_State* L) -> LuaRef {
      ed->queue({ FileEditor::OP::SAVE_VALID_ONLY });
      auto errs = ed->apply((*t), (*w));
      return LKHelpers::makeErrorTable(L, errs);
    })
    .addFunction("applySaveAndMarkErrors", +[](FileEditor* ed, CSTTree* t, FileWriter* w, lua_State* L) -> LuaRef { 
      auto errs = ed->applySaveAndMarkErrors((*t), (*w));
      return LKHelpers::makeErrorTable(L, errs);
    })
    .addFunction("step", +[](FileEditor* ed, CSTTree* t, FileWriter* w, lua_State* L) -> luabridge::LuaRef {
      auto errs = ed->step(*t, *w);
      return LKHelpers::makeErrorTable(L, errs);
    })
    .endClass();
}

void LuaExecutor::bindGit(){
  Namespace ns = getGlobalNamespace(L);

  ns.beginClass<LibGit>("Git")
      .addFunction("isIgnored", +[](LibGit* g, const std::string& pathStr){
        return g->isPathIgnored(pathStr);
      })
      .addFunction("addIgnore", &LibGit::addIgnoreRule)
      .addFunction("add", +[](LibGit* g, const std::string& pathStr) {
        g->add(pathStr);
      })
      .addFunction("addAll", &LibGit::addAll)
      .addFunction("commit", &LibGit::commit)
      .addFunction("resetHead", +[](LibGit* g){
        g->resetHead(); // default value causes binding issue
      })
      .addFunction("setSignature", &LibGit::setSignature)
      .addFunction("branchExists", &LibGit::branchExists)

      .addFunction("branchCreate", &LibGit::branchCreate)
      .addFunction("branchCreate", &LibGit::branchCreate)
      .addFunction("checkout", +[](LibGit* g, const std::string blobId){
        g->checkout(blobId);
      })
      .addFunction("diff", +[](LibGit* g, lua_State* L) {
        auto diffs = g->diff();
        luabridge::LuaRef result = luabridge::newTable(L);
        for (size_t i = 0; i < diffs.size(); ++i)
          result[i + 1] = LKHelpers::makeFileDiff(L, diffs[i]);
        return result;
      })
      .addFunction("diffFromTo", +[](LibGit* g, const std::string& from,
                                     const std::string& to, lua_State* L) {
        auto diffs = g->diff(from, to);
        luabridge::LuaRef result = luabridge::newTable(L);
        for (size_t i = 0; i < diffs.size(); ++i)
          result[i + 1] = LKHelpers::makeFileDiff(L, diffs[i]);
        return result;
      })
    .endClass()
    .addFunction("gitClone", &LibGit::clone)
    .addFunction("gitOpen", &LibGit::open)
    .addFunction("gitOpenOrInit", &LibGit::openOrInit);
}

void LuaExecutor::bindLogger(){

  Namespace ns = getGlobalNamespace(L);

  ns.beginNamespace("Logger")
    .addVariable("level", &LOGGER_LEVEL)
  .endNamespace();
  // TODO: add logger methods
}

void LuaExecutor::bindHelpers() {

  Namespace ns = getGlobalNamespace(L);

  ns.beginNamespace("Helper")

    .beginNamespace("Table")
      .addFunction("print", +[](LuaRef t, lua_State* L) {
        if (!t.isTable()) {
          ERROR("Not a table");
          return;
        }

        LKHelpers::luaTableIter(L, t, [](const std::string& path, const std::string& value,
                                              int depth) {
          std::cout << path << " = " << value << "\n";
        });
      })

      .addFunction("keys", +[](lua_State* L, luabridge::LuaRef t) {
        luabridge::LuaRef result = luabridge::LuaRef::newTable(L);

        if (!t.isTable()) return result;

        int i = 1;
        LKHelpers::luaTableIter(L, t, [&i, &result](const std::string& path, const std::string& value,
                                              int depth) {
          if(depth = 0)
          result[i++] = path;
        });

        return result;
      })

      .addFunction("values", +[](lua_State* L, luabridge::LuaRef t) {
        luabridge::LuaRef result = luabridge::LuaRef::newTable(L);

        if (!t.isTable()) return result;

        int i = 1;
        LKHelpers::luaTableIter(L, t, [&i, &result](const std::string& path, const std::string& value,
                                              int depth) {

          if(depth = 0)
          result[i++] = value;
        });

        return result;
      })

    .endNamespace() // Table

    .beginNamespace("String")
    .addFunction("startsWith", +[](const std::string& s, const std::string& prefix) {
      return s.rfind(prefix, 0) == 0;
    })

    .addFunction("endsWith", +[](const std::string& s, const std::string& suffix) {
      if (s.length() < suffix.length()) return false;
      return s.compare(s.length() - suffix.length(), suffix.length(), suffix) == 0;
    })

    .addFunction("contains", +[](const std::string& s, const std::string& sub) {
      return s.find(sub) != std::string::npos;
    })

    .addFunction("trim", +[](const std::string& s) {
      size_t start = s.find_first_not_of(" \t\n\r");
      if (start == std::string::npos) return std::string("");
    
      size_t end = s.find_last_not_of(" \t\n\r");
      return s.substr(start, end - start + 1);
    })
    
    .addFunction("ltrim", +[](const std::string& s) {
      size_t start = s.find_first_not_of(" \t\n\r");
      return (start == std::string::npos) ? "" : s.substr(start);
    })
    
    .addFunction("rtrim", +[](const std::string& s) {
      size_t end = s.find_last_not_of(" \t\n\r");
      return (end == std::string::npos) ? "" : s.substr(0, end + 1);
    })
    
    .addFunction("split", +[](lua_State* L, const std::string& s, const std::string& delim) {
      luabridge::LuaRef result = luabridge::LuaRef::newTable(L);
    
      size_t start = 0, end;
      int i = 1;
    
      while ((end = s.find(delim, start)) != std::string::npos) {
        result[i++] = s.substr(start, end - start);
        start = end + delim.length();
      }
    
      result[i++] = s.substr(start);
      return result;
    })
    
    .addFunction("repeat", +[](const std::string& s, int n) {
      if (n <= 0) return std::string("");
    
      std::string out;
      out.reserve(s.size() * n);
    
      for (int i = 0; i < n; ++i) {
        out += s;
      }
    
      return out;
    })
    
    .addFunction("padLeft", +[](const std::string& s, int totalWidth, const std::string& pad) {
      if ((int)s.length() >= totalWidth) return s;
    
      std::string p = pad.empty() ? " " : pad;
      std::string out;
    
      while ((int)(out.length() + s.length()) < totalWidth) {
        out += p;
      }
    
      return out.substr(0, totalWidth - s.length()) + s;
    })
    
    .addFunction("padRight", +[](const std::string& s, int totalWidth, const std::string& pad) {
      if ((int)s.length() >= totalWidth) return s;
    
      std::string p = pad.empty() ? " " : pad;
      std::string out = s;
    
      while ((int)out.length() < totalWidth) {
        out += p;
      }
    
      return out.substr(0, totalWidth);
    })
    
    .addFunction("count", +[](const std::string& s, const std::string& sub) {
      if (sub.empty()) return 0;
    
      int count = 0;
      size_t pos = 0;
    
      while ((pos = s.find(sub, pos)) != std::string::npos) {
        count++;
        pos += sub.length();
      }
    
      return count;
    })
    
    .addFunction("isEmpty", +[](const std::string& s) {
      return s.empty();
    })
    
    .addFunction("isBlank", +[](const std::string& s) {
      return s.find_first_not_of(" \t\n\r") == std::string::npos;
    })
    
    .addFunction("reverse", +[](const std::string& s) {
      return std::string(s.rbegin(), s.rend());
    })
    .endNamespace() // String
  .endNamespace(); // Helper
}

#endif
