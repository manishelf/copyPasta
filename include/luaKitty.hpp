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


// Forward declarations of wrapper types used in bindings
struct LuaLanguage {
  TSLang tsLang;
  std::shared_ptr<TSEngine> engine;
};

struct LuaTree {
  TSLang tsLang;
  std::shared_ptr<TSEngine> engine;
  TSTree* tree = nullptr;
  std::string source;
  ~LuaTree() { if (tree) ts_tree_delete(tree); }
};

struct LuaQuery {
  TSLang tsLang;
  TSQuery* query = nullptr;
  std::string expr;
  ~LuaQuery() { if (query) ts_query_delete(query); }
};

struct LuaEditor {
  FileEditor ed;
};

// Helper functions (must be outside bind() to avoid captures)
namespace LuaKittyHelpers {
  
  inline TSRange capToRange(const luabridge::LuaRef& cap) {
    TSRange r{};
    r.start_byte = cap["startByte"].cast<uint32_t>();
    r.end_byte = cap["endByte"].cast<uint32_t>();
    r.start_point.row = cap["row"].cast<uint32_t>();
    r.start_point.column = cap["col"].cast<uint32_t>();
    r.end_point.row = cap["endRow"].cast<uint32_t>();
    r.end_point.column = cap["endCol"].cast<uint32_t>();
    return r;
  }

  inline luabridge::LuaRef makeErrorTable(lua_State* L, const std::vector<FileEditor::Error>& errs) {
    using namespace luabridge;
    LuaRef errTable = newTable(L);
    for (size_t i = 0; i < errs.size(); ++i) {
      const auto& e = errs[i];
      LuaRef err = newTable(L);
      err["type"] = e.e == FileEditor::CONFLICT ? "CONFLICT"
                  : e.e == FileEditor::CST_ERROR ? "CST_ERROR" : "CST_MISSING";
      err["startRow"] = (int)e.range.start_point.row;
      err["startCol"] = (int)e.range.start_point.column;
      err["endRow"] = (int)e.range.end_point.row;
      err["endCol"] = (int)e.range.end_point.column;
      LuaRef editTable = newTable(L);
      editTable["op"] = (int)e.edit.op;
      editTable["change"] = e.edit.change;
      err["edit"] = editTable;
      errTable[i + 1] = err;
    }
    return errTable;
  }

  inline luabridge::LuaRef makeCapture(lua_State* L, TSNode node, const std::string& source, 
                                       TSQuery* query, uint32_t index) {
    using namespace luabridge;
    uint32_t nameLen = 0;
    const char* name = ts_query_capture_name_for_id(query, index, &nameLen);
    uint32_t sb = ts_node_start_byte(node);
    uint32_t eb = ts_node_end_byte(node);
    TSPoint sp = ts_node_start_point(node);
    TSPoint ep = ts_node_end_point(node);
    
    LuaRef cap = newTable(L);
    cap["text"] = (sb <= eb && eb <= source.size()) ? source.substr(sb, eb - sb) : "";
    cap["row"] = (int)sp.row;
    cap["col"] = (int)sp.column;
    cap["endRow"] = (int)ep.row;
    cap["endCol"] = (int)ep.column;
    cap["startByte"] = (int)sb;
    cap["endByte"] = (int)eb;
    
    return cap;
  }
}

// API

class LuaExecutor {
  lua_State* L;
  void bind();
  std::thread watcherThread;
  bool watcherRunning = false;
public:
  LuaExecutor();
  ~LuaExecutor();
  void exec(std::string path);
  void watchAndExecThreaded(const std::string& path, int pollIntervalMs = 1000);
  void joinWatcher();
  void yieldWatcher();
private:
  void watchAndExec(const std::string& path, int pollIntervalMs);
};

// IMPL
void LuaExecutor::watchAndExec(const std::string& path, int pollIntervalMs) {
  namespace fs = std::filesystem;
  watcherRunning = true;
  fs::file_time_type lastWrite = fs::last_write_time(path);
  while (watcherRunning) {
    std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    auto nowWrite = fs::last_write_time(path);
    if (nowWrite != lastWrite) {
      lastWrite = nowWrite;
      try {
        exec(path);
        std::cout << "[LuaExecutor] Re-executed script: " << path << std::endl;
      } catch (const std::exception& e) {
        std::cerr << "[LuaExecutor] Error: " << e.what() << std::endl;
      }
    }
  }
}

void LuaExecutor::watchAndExecThreaded(const std::string& path, int pollIntervalMs) {
  if (watcherThread.joinable()) {
    watcherRunning = false;
    watcherThread.join();
  }
  watcherThread = std::thread(&LuaExecutor::watchAndExec, this, path, pollIntervalMs);
}

void LuaExecutor::joinWatcher() {
  if (watcherThread.joinable()) {
    watcherThread.join();
  }
}

void LuaExecutor::yieldWatcher() {
  std::this_thread::yield();
}

LuaExecutor::~LuaExecutor() {
  watcherRunning = false;
  if (watcherThread.joinable()) {
    watcherThread.join();
  }
  lua_close(L);
}

LuaExecutor::LuaExecutor(){
  L = luaL_newstate();
  luaL_openlibs(L);
  bind();
}

void LuaExecutor::bind(){
  using namespace luabridge;

  getGlobalNamespace(L)
    // ---- LuaLanguage ----
    .beginClass<LuaLanguage>("Language")
      .addFunction("parse", +[](LuaLanguage* lang, const std::string& input, bool isFile) -> LuaTree* {
        std::string source;
        if (isFile) {
          FileReader reader(input);
          auto blk = reader.sync();
          if (!blk.cont) throw std::runtime_error("Cannot read file: " + input);
          source = std::string(blk.cont, blk.size);
        } else {
          source = input;
        }
        TSParser* parser = ts_parser_new();
        ts_parser_set_language(parser, lang->tsLang.lang);
        TSTree* tree = ts_parser_parse_string(parser, nullptr, source.data(), (uint32_t)source.size());
        ts_parser_delete(parser);
        if (!tree) throw std::runtime_error("Parse failed");
        auto* lt = new LuaTree();
        lt->engine = lang->engine;
        lt->tree = tree;
        lt->source = std::move(source);
        return lt;
      })
      .addFunction("queryNew", +[](LuaLanguage* lang, const std::string& expr) -> LuaQuery* {
        uint32_t errOff = 0;
        TSQueryError errType;
        TSQuery* q = ts_query_new(lang->tsLang.lang, expr.c_str(), (uint32_t)expr.size(), &errOff, &errType);
        if (!q || errType != TSQueryErrorNone)
          throw std::runtime_error("Query error at offset " + std::to_string(errOff));
        auto* lq = new LuaQuery();
        lq->query = q;
        lq->expr = expr;
        return lq;
      })
    .endClass()

    // ---- LuaTree ----
    .beginClass<LuaTree>("Tree")
      .addFunction("sexp", +[](LuaTree* t) -> std::string {
        char* raw = ts_node_string(ts_tree_root_node(t->tree));
        std::string result(raw);
        free(raw);
        return result;
      })
      .addFunction("asQuery", +[](LuaTree* t) -> std::string {
        CSTTree cst(t->tree, std::string_view(t->source), *t->engine);
        return cst.asQuery();
      })
      .addFunction("query", +[](LuaTree* t, const std::string& expr, LuaRef callback) {
        lua_State* LS = callback.state();
        TSQuery* query = QueryCache::global().get(t->engine.get(), expr);
        TSNode root = ts_tree_root_node(t->tree);
        TSQueryCursor* cursor = ts_query_cursor_new();
        ts_query_cursor_exec(cursor, query, root);
        
        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match)) {
          LuaRef captures = newTable(LS);
          for (uint32_t i = 0; i < match.capture_count; ++i) {
            TSNode node = match.captures[i].node;
            uint32_t nameLen = 0;
            const char* name = ts_query_capture_name_for_id(query, match.captures[i].index, &nameLen);
            LuaRef cap = LuaKittyHelpers::makeCapture(LS, node, t->source, query, match.captures[i].index);
            captures[std::string(name, nameLen)] = cap;
          }
          callback(captures);
        }
        ts_query_cursor_delete(cursor);
      })
      .addFunction("queryWith", +[](LuaTree* t, LuaQuery* q, LuaRef callback) {
        lua_State* LS = callback.state();
        TSNode root = ts_tree_root_node(t->tree);
        TSQueryCursor* cursor = ts_query_cursor_new();
        ts_query_cursor_exec(cursor, q->query, root);
        
        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match)) {
          LuaRef captures = newTable(LS);
          for (uint32_t i = 0; i < match.capture_count; ++i) {
            TSNode node = match.captures[i].node;
            uint32_t nameLen = 0;
            const char* name = ts_query_capture_name_for_id(q->query, match.captures[i].index, &nameLen);
            LuaRef cap = LuaKittyHelpers::makeCapture(LS, node, t->source, q->query, match.captures[i].index);
            captures[std::string(name, nameLen)] = cap;
          }
          callback(captures);
        }
        ts_query_cursor_delete(cursor);
      })
      // Note: tree:matches() would return LuaRef but we need lua_State* which we don't have
      // Users should use tree:query() with callback instead
    .endClass()

    // ---- LuaQuery ----  
    .beginClass<LuaQuery>("Query")
      // Note: query:matches() has same issue - use tree:queryWith() instead
    .endClass()

    // ---- loadLanguage ----
    .addFunction("loadLanguage", +[](const std::string& langName) -> LuaLanguage* {
      Loader loader;
      TSLang tsLang = loader.get(langName);
      if (!tsLang.lang) throw std::runtime_error("Failed to load language: " + langName);
      auto* ll = new LuaLanguage();
      ll->tsLang = std::move(tsLang);
      ll->engine = TSEnginePool::global().get(ll->tsLang.lang);
      return ll;
    })

    // ---- FileReader ----
    .beginClass<FileReader>("FileReader")
      .addConstructor<void(*)(std::string)>()
      .addFunction("isValid", &FileReader::isValid)
      .addFunction("sync", +[](FileReader* r, lua_State* L) -> LuaRef {
        auto blk = r->sync();
        LuaRef result = newTable(L);
        result["cont"] = blk.cont ? std::string(blk.cont, blk.size) : "";
        return result;
      })
      .addFunction("get", +[](FileReader* r) -> std::string {
        auto sv = r->get();
        return std::string(sv.data(), sv.size());
      })
      .addFunction("getRange", +[](FileReader* r, size_t from, size_t to) -> std::string {
        auto sv = r->get(from, to);
        return std::string(sv.data(), sv.size());
      })
      .addFunction("getLine", +[](FileReader* r, size_t row) -> std::string {
        auto sv = r->getLine(row);
        return std::string(sv.data(), sv.size());
      })
      .addFunction("find", +[](FileReader* r, const std::string& pattern, bool regex, lua_State* L) -> LuaRef {
        auto results = r->find(pattern, regex);
        LuaRef table = newTable(L);
        for (size_t i = 0; i < results.size(); ++i) {
          LuaRef match = newTable(L);
          match["startByte"] = (int)results[i].match.start_byte;
          match["endByte"] = (int)results[i].match.end_byte;
          match["row"] = (int)results[i].match.start_point.row;
          match["col"] = (int)results[i].match.start_point.column;
          auto sv = r->get(results[i].match.start_byte, results[i].match.end_byte);
          match["text"] = std::string(sv.data(), sv.size());
          table[i + 1] = match;
        }
        return table;
      })
    .endClass()
    .addFunction("read", +[](const std::string& path) -> FileReader* {
      return new FileReader(path);
    })

    // ---- FileWriter ----
    .beginClass<FileWriter>("FileWriter")
      .addConstructor<void(*)(std::string)>()
      .addFunction("isValid", &FileWriter::isValid)
      .addFunction("save", &FileWriter::save)
      .addFunction("backup", +[](FileWriter* w, const std::string& suffix) -> bool {
        return w->backup(suffix);
      })
      .addFunction("flush", +[](FileWriter* w, std::string path) -> bool {
        return w->flush(path);
      })
      .addFunction("append", +[](FileWriter* w, std::string str) {
        w->append(str);
      })
      .addFunction("insert", +[](FileWriter* w, size_t offset, std::string str) {
        w->insert(offset, str);
      })
      .addFunction("insertRowBefore", +[](FileWriter* w, size_t row, const std::string& str) {
        w->insertRowBefore(row, str);
      })
      .addFunction("insertRowAfter", +[](FileWriter* w, size_t row, const std::string& str) {
        w->insertRowAfter(row, str);
      })
      .addFunction("deleteRow", +[](FileWriter* w, size_t row) {
        w->deleteRow(row);
      })
      .addFunction("deleteCont", +[](FileWriter* w, size_t from, size_t to) {
        w->deleteCont(from, to);
      })
      .addFunction("writeAt", +[](FileWriter* w, size_t offset, std::string str) {
        w->write(offset, str);
      })
      .addFunction("replace", +[](FileWriter* w, const std::string& pattern, const std::string& tpl, size_t nth) {
        w->replace(pattern, tpl, nth);
      })
      .addFunction("replaceAll", +[](FileWriter* w, const std::string& pattern, const std::string& tpl) {
        w->replaceAll(pattern, tpl);
      })
    .endClass()
    .addFunction("write", +[](const std::string& path) -> FileWriter* {
      return new FileWriter(path);
    })

    // ---- Editor ----
    .beginClass<LuaEditor>("Editor")
      .addConstructor<void(*)()>()
      .addFunction("delete", +[](LuaEditor* ed, LuaRef cap) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        ed->ed.queue({ FileEditor::OP::DELETE, r, "", "" });
      })
      .addFunction("deleteWithPad", +[](LuaEditor* ed, LuaRef cap, uint32_t pad) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        r.start_byte = r.start_byte > pad ? r.start_byte - pad : 0;
        r.end_byte += pad;
        ed->ed.queue({ FileEditor::OP::DELETE, r, "", "" });
      })
      .addFunction("insert", +[](LuaEditor* ed, LuaRef cap, const std::string& text) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        r.end_byte = r.start_byte;
        r.end_point = r.start_point;
        ed->ed.queue({ FileEditor::OP::INSERT, r, text, "" });
      })
      .addFunction("insertAfter", +[](LuaEditor* ed, LuaRef cap, const std::string& text) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        r.start_byte = r.end_byte;
        r.start_point = r.end_point;
        ed->ed.queue({ FileEditor::OP::INSERT, r, text, "" });
      })
      .addFunction("write", +[](LuaEditor* ed, LuaRef cap, const std::string& text) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        ed->ed.queue({ FileEditor::OP::WRITE, r, text, "" });
      })
      .addFunction("replace", +[](LuaEditor* ed, LuaRef cap, const std::string& pattern, const std::string& tpl) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        ed->ed.queue({ FileEditor::OP::REPLACE, r, tpl, pattern });
      })
      .addFunction("printBefore", +[](LuaEditor* ed, LuaRef cap) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        ed->ed.queue({ FileEditor::OP::PRINT_CHANGE_BEFORE, r, "", "" });
      })
      .addFunction("printAfter", +[](LuaEditor* ed, LuaRef cap) {
        TSRange r = LuaKittyHelpers::capToRange(cap);
        ed->ed.queue({ FileEditor::OP::PRINT_CHANGE_AFTER, r, "", "" });
      })
      .addFunction("validate", +[](LuaEditor* ed, File f) {
        TSRange r{};
        ed->ed.queue({ FileEditor::OP::VALIDATE_CST, r, "", "" });
      })
      .addFunction("apply", +[](LuaEditor* ed, LuaTree* lt, FileWriter* w, lua_State* L) -> LuaRef {
        CSTTree cst(lt->tree, std::string_view(lt->source), *lt->engine);
        auto errs = ed->ed.apply(cst, *w);
        lt->source = w->snapshot().cont;
        return LuaKittyHelpers::makeErrorTable(L, errs);
      })
      .addFunction("applyAndSave", +[](LuaEditor* ed, LuaTree* lt, FileWriter* w, lua_State* L) -> LuaRef {
        CSTTree cst(lt->tree, std::string_view(lt->source), *lt->engine);
        auto errs = ed->ed.apply(cst, *w);
        lt->source = w->snapshot().cont;
        if (errs.empty()) w->save();
        return LuaKittyHelpers::makeErrorTable(L, errs);
      })
    .endClass()

    // ---- LibGit ----
    .beginClass<LibGit>("Git")
      .addFunction("isIgnored", (bool(LibGit::*)(std::string)) &LibGit::isPathIgnored)
      .addFunction("addIgnore", &LibGit::addIgnoreRule)
      .addFunction("add", +[](LibGit* g, const std::string& pathStr) {
        fs::path p(pathStr);
        g->add(p);
      })
    .endClass()
    .addFunction("gitClone", +[](const std::string& url, const std::string& path, bool shallow) -> LibGit* {
      return new LibGit(LibGit::clone(url, path, shallow));
    })
    .addFunction("gitOpen", +[](const std::string& path) -> LibGit* {
      return new LibGit(LibGit::open(path));
    })

    // ---- File ----
    .beginClass<File>("File")
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
    .addFunction("deleteFile", +[](File* f) -> int { return File::deleteFile(*f); })
    .addFunction("deleteDir", +[](File* f) -> int { return File::deleteDir(*f); })
    .addFunction("renameFileOrDir", +[](File* f, const std::string& name) -> bool { 
      return File::rename(*f, name); 
    })

    // ---- walk ----
    .addFunction("walk", +[](const std::string& path, LuaRef opts, LuaRef callback) {
      DirWalker walker(path);

      walker.recursive = opts["recursive"].cast<bool>();
      walker.filesOnly = opts["filesOnly"].cast<bool>();
      
      if (opts["ext"].isTable()) {
        for (auto it : pairs(opts["ext"])) {
          walker.matchExt.insert(it.second.cast<std::string>());
        }
      }
      
      walker.walk([callback](DirWalker::STATUS status, File file, LibGit& git) -> DirWalker::ACTION {
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

    // ---- findInFiles ----
    .addFunction("findInFiles", +[](const std::string& path, const std::string& pattern, 
                                    LuaRef opts, lua_State* L) -> LuaRef {
      DirWalker walker(path);
      walker.recursive = true;
      walker.filesOnly = true;
      
      if (opts.isTable()) {
        walker.recursive = opts["recursive"].cast<bool>();
        if (opts["ext"].isTable()) {
          for (auto it : pairs(opts["ext"])) {
            walker.matchExt.insert(it.second.cast<std::string>());
          }
        }
      }
      
      struct Hit { std::string path; uint32_t row, col; std::string text; };
      std::vector<Hit> hits;
      std::mutex hitsMtx;
      
      ThreadPool pool;
      walker.walk(pool, [&hits, &hitsMtx, &pattern](DirWalker::STATUS status, File file, LibGit&) -> DirWalker::ACTION {
        if (status == DirWalker::QUEUING) return DirWalker::CONTINUE;
        FileReader reader(file);
        if (!reader.isValid()) return DirWalker::CONTINUE;
        reader.sync();
        auto results = reader.find(pattern, false);
        if (!results.empty()) {
          std::lock_guard<std::mutex> lk(hitsMtx);
          for (auto& r : results) {
            auto sv = reader.get(r.match.start_byte, r.match.end_byte);
            hits.push_back({ file.pathStr, r.match.start_point.row, r.match.start_point.column, std::string(sv) });
          }
        }
        return DirWalker::CONTINUE;
      });
      pool.waitUntilFinished();
      
      LuaRef results = newTable(L);
      for (size_t i = 0; i < hits.size(); ++i) {
        LuaRef hit = newTable(L);
        hit["path"] = hits[i].path;
        hit["row"] = (int)hits[i].row;
        hit["col"] = (int)hits[i].col;
        hit["text"] = hits[i].text;
        results[i + 1] = hit;
      }
      return results;
    });
}

void LuaExecutor::exec(std::string path){
  if (luaL_dofile(L, path.c_str()) != LUA_OK) {
    throw std::runtime_error("Error executing " + path + " : " + lua_tostring(L, -1));
  }
}

#endif
