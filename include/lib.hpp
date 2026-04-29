#include "git2/checkout.h"
#include "git2/deprecated.h"
#include "git2/diff.h"
#include "git2/patch.h"
#include "git2/signature.h"
#include "git2/types.h"
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <git2.h>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <tree_sitter/api.h>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace fs = std::filesystem;

// ----------------------------------------------------------
// API
// ----------------------------------------------------------
#ifndef LIB_H_
#define LIB_H_

class File {
public:
  std::string pathStr;
  std::string name;
  std::string ext;
  bool isDir;
  bool isReg;
  bool isValid;
  size_t size;
  size_t level;
  fs::path path;
  fs::file_status status;
  fs::directory_entry dir_entry;

  // TODO: hotspot
  File(std::string path);
  File(fs::directory_entry entry);
  File();
  // hotspot
  ~File();

  void loadFromEntry();

  void sync();

  static int deleteFile(File &target); // deletes entry file and commits
  static int deleteDir(File &target);   // deletes entry dir and commits returns
                                        // number of children deleted
  static bool rename(File &target, std::string name); // moves or renames
};

struct FileSnapshot {
  std::string cont;
  File file;
  size_t lastModified;
  bool dirty;
};

class FileReader {
  File file;
  void readFileMetadata();
  bool _isValid = false;
  size_t pos;

  static const char *tsRead(void *payload, uint32_t byte_index, TSPoint point,
                            uint32_t *bytes_read);

  std::vector<char> buf;

  bool rowOffsetsValid = false;
  std::vector<size_t> rowOffsets;

public:
  size_t level = 0;


  size_t bufStart = 0;
  size_t bufSize = 0;
  static constexpr size_t defaultBlockSize = 1024 * 1024;
  size_t blockSize = defaultBlockSize;
  bool readReverse;
  bool snapshotMode = false; // disables fresh load and sync

  FileReader(File file, size_t blockSize = defaultBlockSize);
  FileReader(std::string filePath, size_t blockSize = defaultBlockSize);
  FileReader(const FileSnapshot snap, size_t blockSize = defaultBlockSize);
  FileReader(const FileReader &copy);
  FileReader() {};
  ~FileReader();

  bool isValid() { return _isValid; };
  File getFile() { return file; };
  const std::vector<size_t>& getRowOffsets(); 

  typedef struct {
    char *cont;
    size_t size;
  } block;

  block sync();
  block load(size_t from, size_t to);
  std::string_view get();
  std::string_view get(size_t from, size_t to);
  std::string_view getLine(size_t row);
  std::string_view getIndent(size_t row);

  void reset();
  block readBlockAt(size_t pos);
  block next();
  block prev();

  TSInput asTsInput();
  typedef struct {
    TSRange match;
    std::vector<TSRange> captures;
  } MatchResult;

  std::vector<MatchResult> find(std::string pattern, 
                                bool regex = false,// aparently literal search is faster than regex 
                                uint32_t opt_compile = PCRE2_CASELESS); 
  
  static std::vector<MatchResult> findIn(const std::string &text, std::string pattern,
                                  bool regex = false, 
                                  uint32_t opt_compile = PCRE2_CASELESS);
 
  std::vector<MatchResult> findWith(pcre2_code *re,
                                    uint32_t opt_match = PCRE2_NO_UTF_CHECK); // some compile 
                                                                              // options allowed 
  TSPoint getP(size_t byteOffset);

  FileSnapshot snapshot();

  class iterator {
  public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = block;
    using difference_type = std::ptrdiff_t;
    using pointer = block *;
    using reference = block &;

    iterator(FileReader *reader, size_t pos) : reader(reader), pos(pos) {}

    value_type operator*() { return reader->readBlockAt(pos); }

    iterator &operator++() {
      pos += reader->defaultBlockSize;
      if (pos >= reader->file.size)
        pos = reader->file.size;
      return *this;
    }

    iterator &operator--() {
      if (pos == 0)
        return *this;
      if (pos >= reader->defaultBlockSize)
        pos -= reader->defaultBlockSize;
      else
        pos = 0;
      return *this;
    }

    bool operator==(const iterator &other) const {
      return reader == other.reader && pos == other.pos;
    }

    bool operator!=(const iterator &other) const { return !(*this == other); }

  private:
    FileReader *reader;
    size_t pos;
  };

  iterator begin() { return iterator(this, 0); }

  iterator end() { return iterator(this, file.size); }

  std::reverse_iterator<iterator> rbegin() {
    return std::reverse_iterator<iterator>(end());
  }

  std::reverse_iterator<iterator> rend() {
    return std::reverse_iterator<iterator>(begin());
  }
};

#define STORE_ITER_INFO               \
  size_t currPos = pos;               \
  bool currReadReverse = readReverse; \
  readReverse = false;                \
  pos = 0                             

#define RESTORE_ITER_INFO             \
  pos = currPos;                      \
  readReverse = currReadReverse       \


TSPoint _getP(size_t byteOffset, const std::vector<size_t>& rowOffsets);
TSRange _makeRange(size_t start, size_t end, const std::vector<size_t>& rowOffsets);

class FileWriter {
  File file;
  bool _isValid;
  std::ofstream oFileStream;
  FileSnapshot snap;
  bool rowOffsetsValid = false;
  std::vector<size_t> rowOffsets;

public:
  FileWriter(const FileSnapshot snap);
  FileWriter(std::string path);
  FileWriter(File f);
  FileWriter(const FileWriter &copy);
  ~FileWriter();

  bool isValid() { return _isValid; };
  File getFile() { return file; };
  const FileSnapshot snapshot() const { return snap; };
  const std::vector<size_t>& getRowOffsets(); 

  TSPoint getP(size_t byteOffset);

  bool save(); // save buf to underling file
  bool backup(const std::string &suffix = ".bak"); // create a backup in same folder
  bool flush(std::string &path); // create if non existing , will over write existing

  FileWriter &copy(std::string &path); // load file cont to buf

  // overwrite
  FileWriter &write(const std::string &content); // replace entire buf content
  FileWriter &write(size_t offset, char *newCont, size_t newContLen);
  FileWriter &write(size_t offset, std::string &cont);
  FileWriter &write(size_t from, size_t to, std::string &cont);

  FileWriter &append(std::string &cont);
  FileWriter &insert(size_t offset, std::string &newCont);
  FileWriter &insertRowBefore(size_t row, const std::string &line, bool preserveIndent = false);
  FileWriter &insertRowAfter(size_t row, const std::string &line, bool preserveIndent = false);
  FileWriter &deleteRow(size_t row);
  FileWriter &deleteCont(size_t from, size_t to);

  FileWriter &replace(std::string pattern, std::string templateOrResult,
                      size_t nth_occ = 0, // 0 for first 1 for second and
                                          //-1 for last, -2 for last second and so on
                      uint32_t opt = PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED);
  FileWriter &replaceAll(std::string pattern, std::string templateOrResult,
                         uint32_t opt = PCRE2_SUBSTITUTE_GLOBAL |
                                      PCRE2_SUBSTITUTE_EXTENDED);
};

class LibGit {

  using RepoPtr = std::shared_ptr<git_repository>;
  static RepoPtr make_repo(git_repository* repo);
 
  RepoPtr repo;
  std::string root;
  std::mutex gitMutex;
  
  std::string username = "copyPasta";
  std::string email = "manishelf@proton.me";
  git_signature* signature;

  static std::once_flag lib_git_init;
  static void init();

  git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
  git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
	
public:
  LibGit(git_repository *repo);
  ~LibGit();

  static LibGit clone(std::string url, std::string path = ".", 
                      bool shallow = false, git_clone_options opts = GIT_CLONE_OPTIONS_INIT);
  static LibGit open(std::string path = ".");
 
  bool isPathIgnored(fs::path path);
  bool isPathIgnored(std::string path);
  
  void addIgnoreRule(std::string rule);

  void add(const fs::path &path);
  void addAll();

  void checkout(std::string blobId, git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT);
  void branchCreate(std::string name); 
  void setSignature(std::string username, std::string email);
  void commit(std::string message);

  void resetHead(git_reset_t opt = GIT_RESET_HARD);

  bool branchExists(std::string name);

  struct LineDiff {
    git_diff_line_t type = GIT_DIFF_LINE_ADDITION;
    int oldLineNo = 0; // -1 if not in old file i.e add
    int newLineNo = 0; // -1 if not in new file i.e del
    size_t fileOffset = 0; // offset in the original file to the content
    std::string cont;
    std::string blameAuthor;
    std::string blameCommit;
  };

  struct Hunk{
    int oldStartLine; 
    int oldLinesCount; 
    int newStartLine; 
    int newLinesCount; 

    // eg @@ -20,1 +20, 3 @@  at start of a hunk 
    std::string header; 

    std::vector<LineDiff> lineDiffs;
  };

  struct FileDiff {
    std::vector<Hunk> hunks;
    std::string oldPath;
    std::string newPath;
    git_delta_t status;
    git_diff_flag_t flags;
  };

  std::vector<FileDiff> diff();
  std::vector<LibGit::FileDiff> diff(std::string fromBlobId, std::string toBlobId,
                                     git_diff_options opts = GIT_DIFF_OPTIONS_INIT); 
};


// FileEditor

// order based on increasing precedence 
#define FOREACH_OP(OP)                                                         \
  OP(FLUSH)                                                                    \
  OP(SAVE)                                                                     \
  OP(PRINT_PATH)                                                               \
  OP(PRINT_ERRORS)                                                             \
  OP(VALIDATE_CST)                                                             \
  OP(PRINT_CHANGE_AFTER)                                                       \
  OP(MARK)                                                                     \
  OP(WRITE)                                                                    \
  OP(INSERT)                                                                   \
  OP(REPLACE)                                                                  \
  OP(DELETE)                                                                   \
  OP(PRINT_CHANGE_BEFORE)                                                       

#define NOT_CONFLICTING_OP(op)              \
  (   op == OP::PRINT_CHANGE_BEFORE         \
   || op == OP::PRINT_CHANGE_AFTER          \
   || op == OP::PRINT_PATH                  \
   || op == OP::PRINT_ERRORS                \
  )   


#define FOREACH_ERROR(ERR)                                                     \
  ERR(CONFLICT)                                                                \
  ERR(CST_ERROR)                                                               \
  ERR(CST_MISSING)

class TSEngine;
class CSTTree;
class FileEditor {
public:
  enum OP {
#define GENERATE_ENUM(ENUM) ENUM,
    FOREACH_OP(GENERATE_ENUM)
#undef GENERATE_ENUM
  };

  std::map<OP, std::string> OP_STR;

  enum ERROR {
#define GENERATE_ENUM(ENUM) ENUM,
    FOREACH_ERROR(GENERATE_ENUM)
#undef GENERATE_ENUM
  };

  std::map<ERROR, std::string> ERROR_STR;

  struct Edit {
    int id = 0;
    OP op;
    TSRange range;
    std::string change;
    std::string context;
    std::vector<int> relatedEdits;
  };
  struct Error {
    ERROR e;
    TSRange range;
    Edit edit;
  };
 
  FileEditor();

  Edit queue(Edit e); // unordered
  bool delEdit(int id);
  void reset();
  std::vector<Error> getConflictErrors();
  void sortOperations(); // sorts the operation to have proper edits from bottom to top
  std::vector<Error> step(CSTTree &tree, FileWriter &writer);
  std::vector<Error> apply(CSTTree &tree, FileWriter &writer);

private:
  std::vector<Edit> operations;
  int edditIdCounter = 0;
  int currStep = 0;
  std::vector<Error> errors;

  static TSPoint getNewEndPoint(const Edit& edit);
};

class ThreadPool;
class DirWalker {
  bool _isValid;

public:
  std::string path;
  size_t level = 0;
  bool recursive = false;
  bool inverted = false;
  bool includeDotDir = false;
  bool obeyGitIgnore = true;
  bool filesOnly = true;
  std::set<std::string> ignore;
  std::set<std::string> matchExt;

  enum STATUS {
    QUEUING, // file queued for processing; may be skipped based on action result
    OPENED,  // file is opened for processing
    STOPPED, // Stoped the walk for current dir
    ABORTED, // Stoped the walk altogether
    FAILED,  // Failed to open file or dir
    DONE
  };
  enum ACTION {
    STOP = -2,    // stop walk in current dir
    ABORT = -1,   // stop the walk altogether
    CONTINUE = 0, // continue walk
    SKIP = 1,     // skip entering child dir
  };

  DirWalker(std::string dir);

  void copyConfig(DirWalker* from);

  bool isValid() { return _isValid; }

  std::vector<File> allChildren();

  ~DirWalker();


  // using WalkAction_t = std::function<ACTION(STATUS, File, void *payload)>;

  template <typename Payload, typename Action>
  static ACTION callAction(Action&& action, STATUS status, File file, LibGit& repo, Payload& payload){
    ACTION actRes;
    if constexpr (std::is_invocable_v<Action, STATUS, File, LibGit&, Payload &>) {
      actRes = action(status, file, repo, payload);
    } else if constexpr (std::is_invocable_v<Action, STATUS, File, LibGit&>) {
      actRes = action(status, file, repo);
    } else if constexpr (std::is_invocable_v<Action, STATUS, File>) {
      actRes = action(status, file);
    } else{
      throw std::invalid_argument("Invalid signature for walk action, check DirWalker::callAction for valid signatures");
    }
    return actRes;
  }

  template <typename Action> // Action is any callable
  STATUS walk(Action &&action);

  template <typename Payload, typename Action>
  STATUS walk(Action &&action, Payload &payload = NULL);

  // will give two calls per entry to Action 1 QUEUING, 2 OPENED
  template <typename Action> void walk(ThreadPool &pool, Action &&action);

  template <typename Payload, typename Action>
  void walk(ThreadPool &pool, Action &&action, Payload &payload = NULL);

private:
  template <typename Payload, typename Action>
  STATUS walk(LibGit& repo, Action &&action, Payload &payload = NULL);

  using AbortSignal = std::shared_ptr<std::atomic<bool>>;
  template <typename Payload, typename Action>
  void walk(LibGit& repo, ThreadPool &pool, Action &&action,
            AbortSignal globalAbort, Payload &payload);
};

#define DECLARE_TS_LANG(name) extern "C" {      \
    const TSLanguage *tree_sitter_##name(void); \
}

class TSEngine;

class CSTTree {
private:
  std::unique_ptr<TSTree> tree;
  std::string_view source;
  std::string _source;
  TSEngine &parent;
  
  struct TSTreeDeleter {
    void operator()(TSTree* t) const {
      ts_tree_delete(t); // deletes once after last owner is deleted , previous owner gets null value
    }
  };

  std::unique_ptr<TSTree, TSTreeDeleter> makeTreePtr(TSTree* tree);
public:
  friend TSEngine;

  CSTTree(TSTree *tree, std::string_view source, TSEngine &parent);
  ~CSTTree();

  std::string sTree();
  std::string asQuery();
  void getQueryForNode(TSNode node, std::string &query, size_t level = 0);
  std::string getText(TSNode n);

  template <typename cb> void find(TSQuery *query, cb handle);

  bool validate(TSInputEdit edit, size_t insertL = 0, size_t delL = 0);
  void edit(TSInputEdit edit, const std::string &source);

  std::vector<TSRange> getErrors();

  // Returns a non-owning pointer to the underlying TSTree.
  // Use ts_tree_copy() if you need an independent lifetime.
  TSTree *getRawTree() const { return tree.get(); }

  void sync();
};

class TSEngine {
  const TSLanguage *lang;
  TSParser *parser;

public:
  TSEngine(const TSLanguage *lang);
  ~TSEngine();
  const CSTTree parse(std::string_view source);
  const CSTTree parse(const CSTTree &old, std::string_view modSource);
  const CSTTree parse(FileReader &reader);
  const CSTTree parse(FileWriter &writer);

  static TSRange getRange(TSNode n);

  TSQuery *queryNew(std::string &queryExpr);

  std::map<std::string , std::string> getAvailableNodeTypes();

};

struct ReaderWriterEngineTree {
  FileReader fr;
  FileWriter fw;
  FileEditor edt;
  TSEngine eng;
  CSTTree t;
};

// ReaderWriterEngineTree getReaderWriterEngineTree(const File& file, const TSLanguage* lang);
// Use ReaderWriterEngineTree macro instead

class ThreadPool {
  std::vector<std::thread> workers;
  std::queue<std::function<void()>> task;
  std::mutex queueMutex;
  std::condition_variable enqueueCondition;
  std::mutex finishMutex;
  std::condition_variable finishCondition;

  bool stop;
  size_t maxCount;
  std::atomic<size_t> activeTasks{0}; // Tracks pending + running tasks

public:
  ThreadPool(size_t maxCount = std::thread::hardware_concurrency());
  ~ThreadPool();

  // pass in a anonymous class and the action in the constructor will be
  // performed
  template <class F> void enqueue(F &&f);

  bool isBusy() { return activeTasks > 0; }

  // helper to block until all tasks are finished
  // main thread will yield until all threads are done
  void waitUntilFinished() {
    // while (activeTasks > 0) {
    // std::this_thread::yield();
    //}
    std::unique_lock<std::mutex> lock(finishMutex);
    finishCondition.wait(lock, [this] { return activeTasks.load() == 0; });
  }
};

// Thread-safe cache for compiled PCRE2 patterns.
// Patterns are keyed by (pattern_string, compile_options).
// The compiled pcre2_code* is owned by the cache for its lifetime.
// Callers must NOT call pcre2_code_free on pointers returned by get().
class PcreCache {
  struct Key {
    std::string pattern;
    uint32_t    opts;
    bool operator<(const Key &o) const {
      return pattern < o.pattern || (pattern == o.pattern && opts < o.opts);
    }
  };
  std::map<Key, pcre2_code *> cache;
  mutable std::mutex mtx;

public:
  PcreCache(){}

  ~PcreCache() {
    for (auto &[k, re] : cache)
      pcre2_code_free(re);
    cache.clear();
  }

  pcre2_code *get(const std::string &pattern, uint32_t opt_compile = PCRE2_CASELESS);

  // thread safe
  static PcreCache &global() {
    static PcreCache instance;
    return instance;
  }
};

// Thread-safe pool for persistent TSEngine instances per language
class TSEnginePool {
  std::mutex mtx;
  std::map<const TSLanguage*, std::shared_ptr<TSEngine>> engines;
public:
  std::shared_ptr<TSEngine> get(const TSLanguage* lang);
  // thread safe
  static TSEnginePool &global() {
    static TSEnginePool instance;
    return instance;
  }
};

// Thread-safe query cache for reusing TSQuery* per engine and pattern
class QueryCache {
  std::mutex mtx;
  std::map<std::pair<const TSEngine*, std::string>, TSQuery*> cache;
public:
  TSQuery* get(TSEngine* engine, const std::string& pattern); 
  // thread safe
  static QueryCache &global() {
    static QueryCache instance;
    return instance;
  }
};

#define LOG_LEVEL_DEBUG_FULL -1
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3
#define LOG_LEVEL_NONE  4

// Set active level
#define LOGGER_LEVEL LOG_LEVEL_NONE

std::string inline currentTime(){                                                    
    using namespace std::chrono;                                                
    auto now = system_clock::now();                                             
    auto secs = time_point_cast<std::chrono::seconds>(now);                     
    auto micros = duration_cast<std::chrono::microseconds>(now - secs).count(); 
    auto millis = micros / 1000;                                                
    auto micros_rem = micros % 1000;                                            
                                                                                
    std::time_t t = system_clock::to_time_t(secs);                              
    std::tm tm{};                                                               
    localtime_r(&t, &tm);                                                       
                                                                                
    std::ostringstream oss;                                                     
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");                             
    oss << '.' << std::setw(3) << std::setfill('0') << millis;                  
    oss << std::setw(3) << std::setfill('0') << micros_rem;                     
    return oss.str();                                                           
};

// Core log macro
#define LOG(level, label, msg)                                        \
    do {                                                              \
        if (level >= LOGGER_LEVEL) {                                  \
            std::cout << "[" << currentTime() << "] "                 \
                      << label                                        \
                      << " "                                          \
                      << msg                                          \
                      << std::endl;                                   \
        }                                                             \
    } while (0)
#define DEBUG_FULL(msg) LOG(LOG_LEVEL_DEBUG_FULL, "[DEBUG_FULL]", msg)
#define DEBUG(msg)      LOG(LOG_LEVEL_DEBUG,           "[DEBUG]", msg)
#define INFO(msg)       LOG(LOG_LEVEL_INFO,            "[INFO ]", msg)
#define WARN(msg)       LOG(LOG_LEVEL_WARN,            "[WARN ]", msg)
#define ERROR(msg)      LOG(LOG_LEVEL_ERROR,           "[ERROR]", msg)

#endif // LIB_H_

// ----------------------------------------------------------
// IMPL
// ----------------------------------------------------------

#ifndef LIB_IMPLEMENTATION
#define LIB_IMPLEMENTATION

void File::loadFromEntry(){
  if (dir_entry.exists()) {
    DEBUG_FULL("File loadFromEntry");
    path = fs::absolute(dir_entry.path().lexically_normal());
    name = path.filename();
    ext = path.extension();
    status = dir_entry.status();
    isDir = fs::is_directory(status);
    isReg = fs::is_regular_file(status);
    if (isReg) {
      size = dir_entry.file_size();
    } else {
      size = 0;
    }
    isValid = true;
  } else {
    ERROR("File is invalid - " << dir_entry.path());
    isValid = false;
  }
}

File::File(std::string path): dir_entry(path), pathStr(path){
  DEBUG_FULL("File ctor - " << path);
  level = 0;
  loadFromEntry();
};

File::File(fs::directory_entry entry): dir_entry(entry), pathStr(entry.path()) {
  DEBUG_FULL("File ctor - " << pathStr);
  level = 0;
  loadFromEntry();
};

File::File() {
  size = 0;
  isValid = false;
  level = 0;
}

void File::sync() {
  dir_entry.refresh();
  status = dir_entry.status();
  size = dir_entry.file_size();
  isValid = dir_entry.exists();
  isReg = dir_entry.is_regular_file();
  path = dir_entry.path();
  DEBUG("File sync - " << pathStr);
};

int File::deleteFile(File &target) {
  if (target.isDir)
    return -1;
  INFO("File delete file - " << target.pathStr);
  return fs::remove(target.path);
};

int File::deleteDir(File &target) {
  if (!target.isDir)
    return false;
  INFO("File delete dir" << target.pathStr);
  return fs::remove_all(target.path);
};

bool File::rename(File &file, std::string name) {
  fs::rename(file.pathStr, name);
  file.path = fs::path(name);
  file.pathStr = name;
  file.dir_entry = fs::directory_entry(file.path);
  file.sync();
  INFO("File rename from - " << file.name << " to - " << name);
  return true;
};

File::~File() {
  DEBUG_FULL("File destroyed");
};

// FileReader

#define UPDATE_ROW_OFFSETS(data, len)                                            \
  if(!rowOffsetsValid){                                                          \
    DEBUG_FULL("Updating row offsets - " << len);                                \
    rowOffsets.clear();                                                          \
    rowOffsets.push_back(0);                                                     \
    for (size_t i = 0; i < (len); ++i) {                                         \
      if ((data)[i] == '\n') {                                                   \
        rowOffsets.push_back(i + 1);                                             \
      }                                                                          \
    }                                                                            \
    rowOffsetsValid = true;                                                      \
  }

const std::vector<size_t>& FileReader::getRowOffsets(){
  UPDATE_ROW_OFFSETS(buf, bufSize);
  return rowOffsets;
}

FileReader::FileReader(File file, size_t blockSize){
  DEBUG_FULL("FileReader ctor");
  this->file = file;
  if(file.isValid){
    this->blockSize = blockSize;
    _isValid = !file.isDir;
    readFileMetadata();
  }
};

FileReader::FileReader(std::string filePath, size_t blockSize){
  DEBUG_FULL("FileReader ctor");
  file = File(filePath);
  if (file.isValid) {
    _isValid = !file.isDir;
    this->blockSize = blockSize;
    readFileMetadata();
  }
};

FileReader::FileReader(const FileSnapshot snap, size_t blockSize) {
  DEBUG_FULL("FileReader ctor with snap");
  snapshotMode = true;
  buf.reserve(snap.cont.length());
  std::copy(snap.cont.begin(), snap.cont.end(), buf.begin());
  bufStart = 0;
  bufSize = snap.cont.length();
  file = snap.file;
  this->blockSize = blockSize;
  _isValid = true;
  rowOffsetsValid = false;
};

FileReader::FileReader(const FileReader &copy) {
  DEBUG_FULL("FileReader copy");
  file = copy.file;
  _isValid = copy._isValid;
  pos = copy.pos;
  buf = copy.buf; 
  blockSize = copy.blockSize;
  level = copy.level;
  rowOffsets = copy.rowOffsets;
  bufStart = copy.bufStart;
  bufSize = copy.bufSize;
  readReverse = copy.readReverse;
  snapshotMode = copy.snapshotMode;
}

void FileReader::readFileMetadata() {
  if (file.isValid && file.size != 0) {

    DEBUG_FULL("FileReader readFileMetadata");

    bufStart = 0;
    rowOffsets.reserve(file.size / 50);

    size_t blockSize = std::min(this->blockSize, file.size);
    DEBUG_FULL("FileReader readFileMetadata block size - " << blockSize); 

    load(0, blockSize);
  } else {
    ERROR("FileReader readFileMetadata failed");
    bufSize = 0;
    bufStart = 0;
    _isValid = false;
  }
};

FileReader::block FileReader::sync() {
  if (!_isValid)
    return {nullptr, 0};

  if (snapshotMode)
    return {buf.data(), bufSize};

  DEBUG("FileReader sync");

  file.sync();

  buf.clear();
  bufSize = 0;
  bufStart = 0;
  return load(0, file.size);
};

std::string_view FileReader::get() { return get(bufStart, bufSize); }

std::string_view FileReader::get(size_t from, size_t to) {
  if (!_isValid)
    return {};

  if (file.isValid && (from > file.size || to > file.size))
    return {};

  DEBUG_FULL("FileReader get");

  auto block = load(from, to);
  if (block.cont == nullptr)
    return {};

  return std::string_view(block.cont, block.size);
};

std::string_view FileReader::getLine(size_t row) {
  
  DEBUG_FULL("FileReader getLine " << row);

  UPDATE_ROW_OFFSETS(buf, bufSize);
  // this has caused OOM due to unbounded access over the array :)
  if(row + 1 == rowOffsets.size()){
    return get(rowOffsets[row], this->file.size);
  }else if (row >= rowOffsets.size()){
    return "";
  }

  return get(rowOffsets[row], rowOffsets[row + 1]);
}

std::string_view FileReader::getIndent(size_t row){
  std::string_view line = getLine(row);
  int end = 0;
  for(int i = 0; i < line.length(); i++){
    auto ch = line[i];
    if(!(ch == ' ' || ch == '\t')){
      end = i;
      break;
    }
  }
  return line.substr(0, end);
}

FileReader::block FileReader::load(size_t from, size_t to) {
  if (!_isValid)
    return {nullptr, 0};

  size_t fileEnd = snapshotMode ? bufSize : file.size;

  if (from > fileEnd || to > fileEnd || to == 0)
    return {nullptr, 0};

  size_t length = to - from;
  
  if (from >= bufStart && to <= bufStart + bufSize) {
    return {&buf.data()[from - bufStart], length};
  }

  DEBUG("FileReader load from - " << from  << " to - " << to);

  buf.resize(to);

  std::ifstream iFileStream(file.path.c_str(), std::ios::binary | std::ios::ate); 

  iFileStream.seekg(bufSize, std::ios::beg);
  iFileStream.read(buf.data(), to - bufSize);
  size_t bytesRead = iFileStream.gcount();

  iFileStream.close();
  bufSize = to;

  if (bytesRead == 0)
    return {nullptr, 0};

  return {&buf.data()[from], length};
};

FileReader::block FileReader::readBlockAt(size_t pos) {
  if (!_isValid)
    return {nullptr, 0};
  if (pos >= file.size)
    return {nullptr, 0};

  DEBUG_FULL("FileReader readBlockAt - " << pos);

  size_t size = std::min(FileReader::defaultBlockSize, file.size - pos);

  return load(pos, pos + size);
}

TSInput FileReader::asTsInput() {
  TSInput input;
  input.payload = this;
  input.read = &FileReader::tsRead;
  input.encoding = TSInputEncodingUTF8;
  DEBUG_FULL("FileReader asTsInput");
  return input;
};

const char *FileReader::tsRead(void *payload, uint32_t byte_index,
                               TSPoint position, uint32_t *bytes_read) {
  auto *reader = static_cast<FileReader *>(payload);

  if (byte_index >= reader->file.size) {
    DEBUG_FULL("FileReader asTsInput finished");
    *bytes_read = 0;
    return nullptr;
  }

  DEBUG_FULL("FileReader asTsInput read upto - " << byte_index);
  size_t blockSize =
      std::min(reader->blockSize, reader->file.size - byte_index);

  // Ensure buffer covers requested range
  if (reader->buf.empty() || byte_index < reader->bufStart ||
      byte_index + blockSize > reader->bufStart + reader->bufSize) {

    reader->load(byte_index, byte_index + blockSize);
    reader->bufStart = byte_index;
  }

  *bytes_read = static_cast<uint32_t>(blockSize);
  return &reader->buf.data()[byte_index - reader->bufStart];
}

TSPoint _getP(size_t byteOffset, const std::vector<size_t>& rowOffsets) {

  if (rowOffsets.empty())
    return {0, static_cast<uint32_t>(byteOffset)};

  DEBUG_FULL("_getP called - " << rowOffsets.size());
  auto begin = rowOffsets.data();
  auto end   = begin + rowOffsets.size();
  // Find the first row offset that is GREATER than our byte
  auto it = std::upper_bound(begin, end, byteOffset);

  if (it == begin) {
    return {0, static_cast<uint32_t>(byteOffset)};
  }

  // The row number is the index of the element before 'it'
  uint32_t row = static_cast<uint32_t>((it - begin) - 1);

  // The column is the difference between our offset and the rows start offset
  uint32_t col = byteOffset - rowOffsets[row];

  return {row, col};
}

TSPoint FileReader::getP(size_t byteOffset) {
  UPDATE_ROW_OFFSETS(buf, bufSize);
  // :: is needed to scope it from outside
  return ::_getP(byteOffset, rowOffsets);
}


TSRange _makeRange(size_t start, size_t end, const std::vector<size_t>& rowOffsets){
  TSRange r;
  r.start_byte  = static_cast<uint32_t>(start);
  r.end_byte    = static_cast<uint32_t>(end);
  r.start_point = _getP(start, rowOffsets);
  r.end_point   = _getP(end,   rowOffsets);
  return r;
}

std::vector<FileReader::MatchResult> FileReader::find(std::string pattern,
                                                      bool regex, uint32_t opt_compile) {

  
  std::vector<MatchResult> matches;
  
    
  DEBUG("FileReader find called with - " + pattern);
  if (regex) {
    pcre2_code *re = PcreCache::global().get(pattern, opt_compile);

    return findWith(re);
  } else {
    STORE_ITER_INFO;
    for(auto block = next(); block.cont && block.size != 0; block = next()){
      std::string_view searchSpace(block.cont, block.size);

      size_t foundPos = 0;
      size_t offset = 0;
      while ((foundPos = searchSpace.find(pattern, offset)) !=
          std::string_view::npos) {

        size_t matchStart = foundPos;
        size_t matchEnd = matchStart + pattern.size();

        MatchResult match;
        match.match = _makeRange(matchStart, matchEnd, rowOffsets);
        matches.push_back(match);

        offset = matchEnd;
      }
    }
    RESTORE_ITER_INFO;
    DEBUG("FileReader find done for - " + pattern);
    return matches;
  }
};

std::vector<FileReader::MatchResult> FileReader::findIn(const std::string &text,
                                                        std::string pattern,
                                                        bool regex,
                                                        uint32_t opt_compile) {

  DEBUG("FileReader findIn  for - " << pattern << " over size - " << text.length() );
  FileReader fr({text});

  auto matches = fr.find(pattern, regex, opt_compile);
  
  DEBUG("FileReader findIn done  for - " << pattern << " over size - " << text.length() );
  return matches;
};

std::vector<FileReader::MatchResult> FileReader::findWith(pcre2_code *re,
                                                          uint32_t opt_match) {

  DEBUG("FileReader findWith");
  std::vector<MatchResult> matches;
  STORE_ITER_INFO; 
  for(auto block = next(); block.cont && block.size != 0; block = next()){

    PCRE2_SPTR subject = (PCRE2_SPTR)block.cont;
    PCRE2_SIZE subject_length = block.size;
    PCRE2_SIZE *ovector;

    pcre2_match_data *match_data;
    match_data = pcre2_match_data_create_from_pattern(re, NULL);

    int rc = 0;
    PCRE2_SIZE startOffset = 0;
    while (true) {
      rc = pcre2_match(re, subject, subject_length, startOffset, opt_match, match_data,
          NULL);

      if (rc == PCRE2_ERROR_NOMATCH)
        break;

      if (rc < 0) {
        PCRE2_UCHAR buffer[256];
        int len = pcre2_get_error_message(rc, buffer, sizeof(buffer));

        if (len > 0) {
          std::cerr << "PCRE2 error: " << buffer << "\n";
        } else {
          std::cerr << "Unknown PCRE2 error: " << rc << "\n";
        }
        pcre2_match_data_free(match_data);
        throw std::runtime_error("PCRE2 match error");
      }
      ovector = pcre2_get_ovector_pointer(match_data);

      MatchResult match;
      TSRange range;

      range.start_byte = static_cast<uint32_t>(ovector[0]);
      range.end_byte = static_cast<uint32_t>(ovector[1]);

      range.start_point = getP(range.start_byte);
      range.end_point = getP(range.end_byte);

      match.match = range;

      for (int i = 1; i < rc; i++) {
        PCRE2_SIZE start = ovector[2 * i];
        PCRE2_SIZE end = ovector[2 * i + 1];

        if (start == PCRE2_UNSET || end == PCRE2_UNSET)
          continue;

        TSRange capture = _makeRange(start, end, rowOffsets);
        match.captures.push_back(capture);
      }

      startOffset = ovector[1];
      if (ovector[0] == ovector[1]) { // 0 length matches can exist
        if (startOffset < subject_length)
          startOffset++;
        else
          break;
      }

      if (startOffset >= subject_length)
        break;

      matches.push_back(match);
    };

    pcre2_match_data_free(match_data);
  }
  RESTORE_ITER_INFO;

  DEBUG("FileReader findWith done");
  return matches;
};

FileSnapshot FileReader::snapshot() {

  DEBUG("FileReader snapshot");
  FileSnapshot snap = {};

  snap.dirty = false;
  snap.file = file;
  if (file.isValid) {
    snap.file.sync(); 
    fs::file_time_type mtimCurr = snap.file.dir_entry.last_write_time();
    snap.lastModified = mtimCurr.time_since_epoch().count();

    fs::file_time_type mtimOld = file.dir_entry.last_write_time();
    size_t selfLastModified = mtimOld.time_since_epoch().count();

    if(selfLastModified < snap.lastModified){
      sync();
    }
  }

  STORE_ITER_INFO;
  while(next().cont != nullptr){
    // load all remaining blocks
    DEBUG_FULL("FileReader snapshot Loaded block");
  }
  RESTORE_ITER_INFO;

  snap.cont = std::string(buf.data(), bufSize);
  return snap;
}

FileReader::block FileReader::next() {
  size_t fileEnd = snapshotMode ? bufSize : file.size;

  if (pos >= fileEnd || fileEnd == 0) {
    return {nullptr, 0};
  }

  DEBUG_FULL("FileReader next - " << file.pathStr);

  size_t currentBlockSize = 0;

  if (fileEnd - pos < defaultBlockSize) {
    currentBlockSize = fileEnd - pos;
  } else {
    currentBlockSize = defaultBlockSize;
  }

  if (pos < bufStart || pos + currentBlockSize > bufStart + bufSize) {
    load(pos, pos + currentBlockSize);
    bufStart = pos;
  }

  char *currPtr = &buf.data()[pos];

  if (readReverse) {
    pos = (pos >= currentBlockSize) ? pos - currentBlockSize : 0;
  } else {
    pos += currentBlockSize;
  }

  return {currPtr, currentBlockSize};
};

FileReader::block FileReader::prev() {
  size_t fileEnd = snapshotMode ? bufSize : file.size;

  if (pos <= 0 || fileEnd == 0) {
    return {nullptr, 0};
  }

  DEBUG_FULL("FileReader prev - " << file.pathStr);

  size_t currentBlockSize = std::min(FileReader::defaultBlockSize, pos);

  if (pos < bufStart || pos + currentBlockSize > bufStart + bufSize) {
    load(pos, pos + currentBlockSize);
    bufStart = pos;
  }

  char *currPtr = &buf.data()[pos];

  if (readReverse) {
    if (pos < fileEnd - 1) {
      pos += currentBlockSize;
    }
  } else if (pos > 0) {
    pos -= currentBlockSize;
  }

  return {currPtr, currentBlockSize};
};

void FileReader::reset() {
  DEBUG("FileReader reset");
  bufStart = 0;
  if (readReverse) {
    pos = file.size;
  } else {
    pos = 0;
  }
};

FileReader::~FileReader() {
  DEBUG_FULL("FileReader destroyed");
};

// FileWriter

const std::vector<size_t>& FileWriter::getRowOffsets(){
  UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());
  return rowOffsets;
}

FileWriter::FileWriter(const FileSnapshot snap) {
  DEBUG_FULL("FileWriter ctor with snap");
  this->snap = snap;
  file = snap.file;
  _isValid = file.isValid;
  rowOffsetsValid = false;
  UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());
};

FileWriter::FileWriter(std::string path) {
  DEBUG_FULL("FileWriter ctor with path");
  FileReader tmp(path);
  snap = tmp.snapshot();
  file = tmp.getFile();
  rowOffsets = tmp.getRowOffsets();
  _isValid = file.isValid;
}

FileWriter::FileWriter(File f) {
  DEBUG_FULL("FileWriter ctor with file");
  FileReader tmp(f);
  if(tmp.bufSize != tmp.getFile().size){
    tmp.sync();
  }
  snap = tmp.snapshot();
  file = tmp.getFile();
  rowOffsets = tmp.getRowOffsets();
  _isValid = file.isValid;
}

FileWriter::FileWriter(const FileWriter &copy) {
  DEBUG_FULL("FileWriter copy ctor");
  snap = copy.snap;
  file = copy.file;
  rowOffsets = copy.rowOffsets;
  _isValid = copy.file.isValid;
};

FileWriter::~FileWriter() {
  DEBUG_FULL("FileReader destroyed");
  if (oFileStream.is_open())
    oFileStream.close();
};

bool FileWriter::backup(const std::string &suffix) {
  std::string bkpPath;
  bkpPath = file.pathStr + suffix;
  if (fs::exists(bkpPath)) {
    bkpPath =
        file.pathStr + "." + std::to_string(snap.lastModified) + suffix;
  }
  INFO("FileWriter backup -" << bkpPath);
  std::ofstream bkp = std::ofstream(bkpPath, std::ios::out | std::ios::trunc);
  bkp << snap.cont;

  bkp.flush();
  bool res = bkp.good();
  bkp.close();
  if (res) {
    snap.dirty = false;
  }
  return res;
};

TSPoint FileWriter::getP(size_t byteOffset) {
  return ::_getP(byteOffset, rowOffsets);
};

bool FileWriter::save() {
  INFO("FileWriter save " << file.pathStr);
  std::ofstream bkp =
      std::ofstream(file.pathStr, std::ios::out | std::ios::trunc);
  snap.cont.shrink_to_fit();
  bkp << snap.cont;

  bkp.flush();
  bool res = bkp.good();
  bkp.close();
  file.sync();
  if (res) {
    snap.dirty = false;
  }
  return res;
};

bool FileWriter::flush(std::string &path) {
  INFO("FileWriter flush to " << path);
  std::ofstream target = std::ofstream(path, std::ios::out | std::ios::trunc);
  target << snap.cont;
  target.flush();
  bool res = target.good();
  target.close();
  return res;
};

#define UPDATE_SNAP_META(snap)                                                 \
  DEBUG("FileWriter update meta");                                             \
  snap.dirty = true;                                                           \
  snap.lastModified =                                                          \
      std::chrono::system_clock::now().time_since_epoch().count();             \
  snap.file.size = snap.cont.length();                                         \
  rowOffsetsValid = false;                                                     \
  return *this;

FileWriter &FileWriter::copy(std::string &sourcePath) {
  if (!fs::exists(sourcePath)) {
    throw std::invalid_argument(
        "path to source file does not exist for: copy path-" + sourcePath);
  }
  DEBUG_FULL("FileWriter copy ctor");
  FileReader tmp(sourcePath);
  File curr = snap.file;
  snap = tmp.snapshot();
  snap.file = curr;
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::append(std::string &cont) {
  DEBUG("FileWriter append");
  snap.cont.append(cont);
  UPDATE_SNAP_META(snap);
}

FileWriter &FileWriter::insert(size_t offset, std::string &slice) {
  assert(offset < snap.cont.size());
  DEBUG("FileWriter insert at - " << offset);
  snap.cont.insert(offset, slice);
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::write(const std::string &content) {
  DEBUG("FileWriter write content");
  snap.cont = std::string(content);
  UPDATE_SNAP_META(snap);
}

FileWriter &FileWriter::write(size_t offset, char *newCont, size_t newContLen) {
  assert(offset < snap.cont.size());
  DEBUG("FileWriter write offset - " << offset);
  snap.cont.erase(offset, newContLen);
  snap.cont.insert(offset, newCont, newContLen);
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::write(size_t offset, std::string &cont) {
  assert(offset < snap.cont.size());
  DEBUG("FileWriter write offset - " << offset);
  snap.cont.erase(offset, cont.length());
  snap.cont.insert(offset, cont);
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::write(size_t from, size_t to, std::string &cont) {
  assert(to < snap.cont.size());
  DEBUG("FileWriter write from - " << from << " to - " << to);
  snap.cont.erase(from, to-from);
  snap.cont.insert(from, cont);
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::deleteCont(size_t from, size_t to) {
  assert(to < snap.cont.size());
  DEBUG("FileWriter delete " << from  << " to " << to);
  snap.cont.erase(from, to - from);
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::deleteRow(size_t row) {
  UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());

  assert(rowOffsets.size() > row);

  DEBUG("FileWriter deleteRow - " << row);

  size_t row1Offset = rowOffsets[row];
  size_t row2Offset = rowOffsets[row + 1];

  snap.cont.erase(row1Offset, row1Offset - row2Offset);
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::insertRowBefore(size_t row, const std::string &cont, bool preserveIndent) {
  UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());

  assert(rowOffsets.size() > row);

  DEBUG("FileWriter insertRow - " << row);

  bool hasEndl = cont[cont.length() - 1] == '\n';
  size_t rowOffset = rowOffsets[row];

  std::string indent = "";

  if(preserveIndent){
    std::string line = snap.cont.substr(rowOffset, rowOffset - rowOffsets[row+1] + 1);
    int end = 0;
    for(int i = 0; i < line.length(); i++){
      auto ch = line[i];
      if(!(ch == ' ' || ch == '\t')){
        end = i;
        break;
      }
    }
    indent=line.substr(0, end);
  }

  snap.cont.insert(rowOffset, indent+cont);
  if (!hasEndl)
    snap.cont.insert(rowOffset + indent.length() + cont.length(), 1, '\n');
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::insertRowAfter(size_t row, const std::string &cont, bool preserveIndent) {
  return insertRowBefore(row+1, cont, preserveIndent);
}
FileWriter &FileWriter::replaceAll(std::string pattern,
                                   std::string templateOrResult, uint32_t opt) {

  DEBUG("FileWriter replaceAll start - " << pattern  << " to " << templateOrResult);
  pcre2_code *re = PcreCache::global().get(pattern, opt);

  PCRE2_SIZE outLength = snap.cont.length() * 2;

  if (outLength == 0) {
    return *this;
  }

  std::vector<PCRE2_UCHAR> buffer;
  int rc = -1;
substitute:
  buffer.resize(outLength);
  // expands the template with captures and replcaes match
  rc = pcre2_substitute(
                   re, 
                   (PCRE2_SPTR)snap.cont.c_str(), // in buf
                   snap.cont.length(),            // in len
                   0,                              // startOffset 
                   opt,                            // options
                   nullptr,                        // matchData
                   nullptr,                        // matchContext
                   (PCRE2_SPTR)templateOrResult.c_str(), // replacement
                   templateOrResult.length(),           // rlegth
                   (PCRE2_UCHAR *)buffer.data(),        // out buf
                   &outLength);                        // out len

  if (rc == PCRE2_ERROR_NOMEMORY) {
    std::cout << "PCR2_ERROR_NOMEMORY - " << outLength << std::endl;
    goto substitute;
  }

  pcre2_code_free(re);

  if (rc < 0) {
    throw std::runtime_error("PCRE2 substitution failed");
  }

  snap.cont.assign(reinterpret_cast<char *>(buffer.data()), outLength);

  DEBUG("FileWriter replaceAll done - " << pattern  << " to " << templateOrResult);
  UPDATE_SNAP_META(snap);
};

FileWriter &FileWriter::replace(std::string pattern,
                                std::string templateOrResult, size_t nth,
                                uint32_t opt) {

  DEBUG("FileWriter replace start - " << pattern  << " to " << templateOrResult);
  FileReader snapReader(snap);

  pcre2_code *re = PcreCache::global().get(pattern, opt);

  auto results = snapReader.findWith(re, true);

  if (results.empty()) {
    return *this;
  }

  // (a%b + b)%b for -10 % 100 = 90 && 10 % 100 = 10
  nth = (nth % results.size() + results.size()) % results.size();
  auto target = results[nth];

  size_t start_offset = target.match.start_byte;
  size_t end_offset = target.match.end_byte;

  PCRE2_SIZE outLength = templateOrResult.length() * 2;

  if (outLength == 0) {
    return *this;
  }

  std::vector<PCRE2_UCHAR> buffer;
  int rc = -1;
substitute:
  buffer.resize(outLength);
  rc = pcre2_substitute(re,
                       (PCRE2_SPTR)(snap.cont.c_str() + start_offset),
                        end_offset - start_offset,
                        0,
                        opt,
                        nullptr,
                        nullptr,
                        (PCRE2_SPTR)templateOrResult.c_str(),
                        templateOrResult.length(),
                        (PCRE2_UCHAR *)buffer.data(),
                        &outLength);

  if (rc == PCRE2_ERROR_NOMEMORY) {
    std::cout << "PCR2_ERROR_NOMEMORY - " << outLength << std::endl;
    goto substitute;
  }

  pcre2_code_free(re);

  if (rc < 0) {
    throw std::runtime_error("PCRE2 substitution failed");
  }

  DEBUG("FileWriter replace done - " << pattern  << " to " << templateOrResult);
  return write(start_offset, reinterpret_cast<char *>(buffer.data()),
               outLength);
};

// FileEditor

FileEditor::FileEditor() {
  
#define GENERATE_MAP(ENUM) OP_STR[ENUM] = #ENUM;
  FOREACH_OP(GENERATE_MAP)
#undef GENERATE_MAP
#define GENERATE_MAP(ENUM) ERROR_STR[ENUM] = #ENUM;
  FOREACH_ERROR(GENERATE_MAP)
#undef GENERATE_MAP
};

/*
ReaderWriterEngineTree getReaderWriterEngineTree(const File& file, const TSLanguage* lang){
   // TODO: these should have move ctor, otherwise they are copied on return
   FileReader fr(file);
   FileWriter fw(fr.snapshot());
   FileEditor edt;
   TSEngine eng(lang);
   CSTTree t = eng.parse(fw); 
   return {fr, fw, edt, eng, t};
}
*/
#define ReaderWriterEngineTree(file,lang)            \
                     FileReader fr(file);            \
                     FileWriter fw(fr.snapshot());   \
                     FileEditor edt;                 \
                     TSEngine eng(lang);             \
                     CSTTree t = eng.parse(fw);      

FileEditor::Edit FileEditor::queue(FileEditor::Edit e) { 
  DEBUG_FULL("FileEditor queue");
  if(e.id == 0){
    e.id = ++edditIdCounter;
  }
  operations.push_back(e);
  return e;
};

bool FileEditor::delEdit(int id){
  bool deleted = false;
  operations.erase(std::remove_if(operations.begin(), operations.end(),
          [id, &deleted](const Edit& e){
            bool found = e.id == id;
            if(found){
              deleted = true;
            }
            return found;
          }
        ));
  return deleted;
}

void FileEditor::reset() {
  DEBUG_FULL("FileEditor queue");
  operations.clear();
  errors.clear();
  edditIdCounter = 0;
  currStep = 0;
};

TSPoint FileEditor::getNewEndPoint(const Edit &edit){

    TSPoint p = edit.range.start_point;

    // DELETE → nothing inserted
    if (edit.op == OP::DELETE) {
        return p;
    }

    if(edit.change.empty()) return p;

    for (char c : edit.change) {
        if (c == '\n') {
            p.row += 1;
            p.column = 0;
        } else {
            p.column += 1;
        }
    }

    return p;
};

std::vector<FileEditor::Error> FileEditor::getConflictErrors(){

  DEBUG("FileEditor getConflictErrors begins");
  // TODO: this is problematic in terms of perfs as we do two seperate sorts 
  auto op = operations;

  /*
   * scan in ascending order of start offsets.
   * Why? Because once the next edit starts after the current edit ends, no further overlap is possible.
   * This allows an efficient early break in the inner loop.
   */
  std::sort(op.begin(), op.end(),
      [](const FileEditor::Edit &a, const FileEditor::Edit &b) {
        if (a.range.start_byte != b.range.start_byte)
          return a.range.start_byte < b.range.start_byte;

        return a.range.end_byte < b.range.end_byte;
      });

  // O(n2) can this be O(nlogn)?
  for (size_t i = 0; i < op.size(); ++i) {
    const auto &x = op[i];

    // sort of a selection sort
    for (size_t j = i + 1; j < op.size(); ++j) {
      const auto &y = op[j];

      size_t x1 = x.range.start_byte;
      size_t x2 = x.range.end_byte;
      size_t y1 = y.range.start_byte;
      size_t y2 = y.range.end_byte;
      // conflicts could be - 
      //  x1 y1 y2 x2
      //  y1 x1 x2 y2
      //  x1 y1 x2 y2
      //  y1 x1 y2 x2

      // since sorted by start, no need to check all cases
      //This gives O(n²) worst-case, but the break reduces comparisons in practice.
      if (y1 >= x2) break; // no overlap possible further


      if(NOT_CONFLICTING_OP(x.op) || NOT_CONFLICTING_OP(y.op))
        continue;

      // overlap exists
      size_t overlap_start = std::max(x1, y1);
      size_t overlap_end   = std::min(x2, y2);

      TSRange r;
      r.start_byte = (uint32_t)overlap_start;
      r.end_byte   = (uint32_t)overlap_end;

      errors.push_back({CONFLICT, r, x});
      errors.push_back({CONFLICT, r, y});
    }
  }
  DEBUG("FileEditor getConflictErrors ends");
  return errors;
}

std::vector<FileEditor::Error> FileEditor::step(CSTTree &tree, FileWriter &writer){
    auto edit = operations[currStep++];
    TSInputEdit te = {
          edit.range.start_byte,    // start_byte
          edit.range.end_byte,      // old_end_byte
          edit.range.end_byte,      // new_end_byte
          edit.range.start_point,   // start_point
          edit.range.end_point,     // old_end_point
          getNewEndPoint(edit),     // new_end_point
    };

    switch (edit.op) {
    case FileEditor::OP::INSERT: 
    {
      te.old_end_byte = edit.range.start_byte;
      te.new_end_byte = edit.range.start_byte + (uint32_t)edit.change.length();
      writer.insert(edit.range.start_byte, edit.change);
      tree.edit(te, writer.snapshot().cont);
      break;
    }
    case FileEditor::OP::DELETE:
    {
      te.old_end_byte = edit.range.end_byte;
      writer.deleteCont(edit.range.start_byte, edit.range.end_byte);
      tree.edit(te, writer.snapshot().cont);
      break;
    }
    case FileEditor::OP::WRITE:
    {
      te.old_end_byte = edit.range.end_byte;
      te.new_end_byte = edit.range.start_byte + (uint32_t)edit.change.length();
      writer.write(edit.range.start_byte, edit.range.end_byte, edit.change);
      tree.edit(te, writer.snapshot().cont);
      break;
    }
    case FileEditor::OP::REPLACE:
    {
      writer.replaceAll(edit.change, edit.context);
      tree.sync(); // substitutions may occur anywhere
      break;
    }
    case FileEditor::OP::MARK:
    {
      size_t rowStart = edit.range.start_point.row;
      size_t rowEnd =   edit.range.end_point.row;
      std::string text = edit.change;
      std::string additional = edit.context;

      struct MarkInsert {
        size_t row;
        std::string text;
        std::string tag;
      };

      MarkInsert ops[] = {
        {rowEnd + 1, text, "END"}, // After
        {rowStart, additional, "INFO"},
        {rowStart, text, "START"}
      };

      for (auto &op : ops)
      {
        if(op.text.empty()){ // additional info
          continue;
        }

        size_t byteStart = writer.getRowOffsets()[op.row];
        size_t byteEndOld = byteStart;

        writer.insertRowBefore(op.row, op.text+" "+op.tag, true);

        size_t byteEndNew = writer.getRowOffsets()[op.row + 1];

        TSInputEdit te1 = te;
        te1.start_byte = byteStart;
        te1.old_end_byte = byteEndOld;
        te1.new_end_byte = byteEndNew;

        te1.start_point.row = op.row;
        te1.start_point.column = 0;

        te1.old_end_point.row = op.row;
        te1.old_end_point.column = 0;

        te1.new_end_point.row = op.row + 1;
        te1.new_end_point.column = 0;

        tree.edit(te1, writer.snapshot().cont);
      }

      break;
    }
    case FileEditor::OP::VALIDATE_CST: {
      for (auto& err : tree.getErrors()) {
        // Find the modifying edit whose range overlaps this CST error
        Edit causingEdit = edit; // fallback to the validate edit itself
        for (auto& op : operations) {
          if( NOT_CONFLICTING_OP(op.op)
          || (op.op == OP::VALIDATE_CST) ) continue;
          
          bool overlaps = op.range.start_byte <= err.end_byte &&
          op.range.end_byte   >= err.start_byte;
          if (overlaps) {
            causingEdit = op;
              break;
          }
        }
        errors.push_back({CST_ERROR, err, causingEdit});
      }
      break;
    }
    case FileEditor::OP::PRINT_PATH:
    {
      std::cout << writer.getFile().pathStr    << ":" 
                << edit.range.start_point.row    + 1 << ":" 
                << edit.range.start_point.column + 1 << "\n";
      break;
    }
    case FileEditor::OP::PRINT_CHANGE_BEFORE:
    case FileEditor::OP::PRINT_CHANGE_AFTER:
    {
      const auto pOld = edit.range.start_point;
      const auto pNew = getNewEndPoint(edit);

      FileReader r(writer.snapshot());

      const auto& path = writer.getFile().pathStr;

      // one based                        
      const auto startRow = pOld.row     + 1;
      const auto startCol = pOld.column  + 1;
      const auto endRow   = pNew.row     + 1;
      const auto endCol   = pNew.column  + 1;

      const auto oldStart = r.getRowOffsets()[edit.range.start_point.row] + edit.range.start_point.column;
      const auto oldEnd   = r.getRowOffsets()[edit.range.end_point.row] + edit.range.end_point.column;

      const std::string_view originalText = r.get(oldStart, oldEnd);

      std::cout << path << ":" << startRow << ":" << startCol << "\n";
      std::cout << "range: " << startRow << ":" << startCol
                << " -> " << endRow << ":" << endCol << "\n";
      std::cout << this->OP_STR[edit.op] << " : ";
      std::cout << edit.context << "\n";

      std::cout << "<<<<<<<<"   << "\n";
      std::cout.write(originalText.data(), static_cast<std::streamsize>(originalText.size())) << "\n";
      std::cout << "========"   << "\n";
      std::cout << edit.change  << "\n";
      std::cout << ">>>>>>>>"   << "\n";

      std::cout << "-----------------------------------------------------------------\n";

      break;
    }
    case FileEditor::OP::PRINT_ERRORS:
    {
      for (size_t i = 0; i < errors.size(); ++i) {
        const auto &err = errors[i];
        std::cerr << writer.getFile().pathStr << ":"
          << err.range.start_point.row + 1 << ":"
          << err.range.start_point.column + 1 << "\n";

        switch (err.e) {
          case CONFLICT: 
            {
              const auto &errX = errors[i];
              const auto &errY = errors[i + 1];

              size_t x1 = errX.edit.range.start_byte;
              size_t x2 = errX.edit.range.end_byte;
              size_t y1 = errY.edit.range.start_byte;
              size_t y2 = errY.edit.range.end_byte;

              size_t overlap_start = errX.range.start_byte;
              size_t overlap_end   = errX.range.end_byte;

              std::cerr << "CONFLICT detected:\n";

              std::cerr << "  Edit X : [" << x1 << ", " << x2 << "] -> \""
                << errX.edit.change << "\"\n";

              std::cerr << "  Edit Y : [" << y1 << ", " << y2 << "] -> \""
                << errY.edit.change << "\"\n";

              std::cerr << "  Overlap: [" << overlap_start << ", "
                << overlap_end << "]\n\n";

              i++; 
              break;
            }
          case CST_ERROR: 
            {
              std::cerr << "CST_ERROR:\n";

              std::cerr << "  Range: [" 
                << err.range.start_point.row + 1<< ":" << err.range.start_point.column
                << " , " 
                << err.range.end_point.row + 1 << ":" << err.range.end_point.column << "]\n";

              std::cerr << "  Edit : [" 
                << err.edit.range.start_point.row + 1 << ":" << err.edit.range.start_point.column
                << ", "
                << err.edit.range.end_point.row + 1 << ":" << err.edit.range.end_point.column
                << "] -> \""
                << err.edit.change << "\"\n\n";
              break;
            }
          case CST_MISSING: 
            {
              std::cerr << "CST_MISSING:\n";
              std::cerr << "  Range: [" 
                << err.range.start_byte
                << ", " 
                << err.range.end_byte 
                << "]\n";
              std::cerr << "  Edit : [" 
                << err.edit.range.start_byte
                << ", " 
                << err.edit.range.end_byte 
                << "] -> \""
                << err.edit.change 
                << "\"\n\n";
              break;
            }
          default:
            assert(0 && "NOT_IMPLEMENTED");
        }
      }
      break;
    }
    case FileEditor::OP::SAVE: 
    {
      writer.save();
      break;
    }
    case FileEditor::OP::FLUSH: {
      writer.flush(edit.change);
      break;
    }
    default: {
      assert(0 && "NOT IMPLEMENTED");
      break;
    }
    };
}

void FileEditor::sortOperations(){
 std::sort(operations.begin(), operations.end(),
  [](const FileEditor::Edit &a, const FileEditor::Edit &b) {
    // desc
    if (a.op != b.op)
      return a.op > b.op;
    // desc
    if (a.range.start_byte != b.range.start_byte)
      return a.range.start_byte > b.range.start_byte;
    // desc
    return a.range.end_byte > b.range.end_byte;
  });

}

std::vector<FileEditor::Error> FileEditor::apply(CSTTree &tree, FileWriter &writer) {

  DEBUG("FileEditor apply begins");
  // TODO: maybe handle the conflicts based on some priority
  getConflictErrors();
  
  sortOperations();

  for(size_t i = 0; i < operations.size(); i++) {
    DEBUG("FileEditor apply op - " << operations[i].id);
    step(tree, writer);
  }

  DEBUG("FileEditor apply ends");
  return errors;
};

// DirWalker
DirWalker::DirWalker(std::string dir) {
  DEBUG_FULL("DirWalker ctor");
  path = dir;
  _isValid = fs::exists(dir);
};

void DirWalker::copyConfig(DirWalker* other){  
  recursive     = other->recursive;
  obeyGitIgnore = other->obeyGitIgnore;
  includeDotDir = other->includeDotDir;
  ignore        = other->ignore;
  inverted      = other->inverted;
  matchExt      = other->matchExt;
  filesOnly     = other->filesOnly;
}

DirWalker::~DirWalker() {
  DEBUG_FULL("DirWalker destroyed");
};

template <typename Action> DirWalker::STATUS DirWalker::walk(Action &&action) {
  int p = 0;
  return walk(std::forward<Action>(action), p);
};

template <typename Payload, typename Action>
DirWalker::STATUS DirWalker::walk(Action &&action, Payload &payload) {
  LibGit repo = LibGit::open(path);
  for(auto rule : ignore){
    repo.addIgnoreRule(rule);
  }
  DEBUG("DirWalker walk begin - " << path);
  auto res = walk(repo, action, payload);
  DEBUG("DirWalker walk begin - " << path);
  return res;
};

template <typename Payload, typename Action>
DirWalker::STATUS DirWalker::walk(LibGit& repo, Action &&action,
                                  Payload &payload) {

  if (!_isValid)
    return FAILED;

  DEBUG_FULL("DirWalker walk begin - " << path);
  std::vector<fs::directory_entry> entries(
      fs::directory_iterator(this->path), // begin it
      fs::directory_iterator()            // end it
  );

  for (size_t i = 0; i < entries.size(); i++) {

    auto entry = entries[i];
    auto entryPath = entry.path();

    DEBUG_FULL("DirWalker walk with pool entry - " << entryPath);

    if (obeyGitIgnore && repo.isPathIgnored(entryPath)) {
      continue;
    }

    auto ext = entryPath.extension();

    if(!matchExt.empty() 
        && !entry.is_directory()
        && (matchExt.find(ext.string()) == matchExt.end())){
      continue;
    }

    File file(entries[i]);
    file.level = level;

  
    ACTION actRes;
    if(!filesOnly || !file.isDir){
      DEBUG("DirWalker walk do job - " << file.pathStr);
      actRes = callAction(action, OPENED, file, repo, payload);
      DEBUG("DirWalker walk job done- " << file.pathStr);
    } else{
      actRes = ACTION::CONTINUE;
    }

    if (actRes == ACTION::SKIP) {
      DEBUG("DirWalker skip - " << file.pathStr);
      continue;
    } else if (actRes == ACTION::STOP) {
      DEBUG("DirWalker stop - " << file.pathStr);
      return STATUS::STOPPED;
    } else if (actRes == ACTION::ABORT) {
      DEBUG("DirWalker abort - " << file.pathStr);
      return STATUS::ABORTED;
    } else if (actRes == ACTION::CONTINUE &&
        !((file.name == ".") || (file.name == "..")) && file.isDir &&
        recursive && !inverted) {

      DirWalker child(file.pathStr);
      child.level = level + 1;
      child.copyConfig(this);
      STATUS res = child.walk(repo, action, payload);

      if (res == STATUS::ABORTED) {
        return res;
      } else if (res == STATUS::FAILED) {
        ACTION actRes = callAction(action, FAILED, file, repo, payload);
      }

    }

    if (inverted && i == entries.size() - 1) {
      fs::path parent = fs::absolute(path).parent_path();
      if (path == ".")
        parent = parent.parent_path();
      if (parent.has_relative_path()) {
        DirWalker child(parent.string());
        child.copyConfig(this);
        child.level = level - 1;
        child.recursive = false;
        child.inverted = true;

        DEBUG_FULL("DirWalker walk done - " << path);
        return child.walk(repo, action, payload);
      }
    }
  }

  return STATUS::DONE;
};

template <typename Action>
void DirWalker::walk(ThreadPool &pool, Action &&action) {
  int p = 0;
  walk(pool, std::forward<Action>(action), p);
};

template <typename Payload, typename Action>
void DirWalker::walk(ThreadPool &pool, Action &&action, Payload &payload) {

  AbortSignal abortSignal =
      std::make_shared<std::atomic<bool>>(false);

  LibGit repo = LibGit::open(path);
  for(auto rule : ignore){
    repo.addIgnoreRule(rule);
  }

  walk(repo, pool, action, abortSignal, payload);
}

template <typename Payload, typename Action>
void DirWalker::walk(LibGit& repo, ThreadPool &pool, Action &&action,
                     AbortSignal abortSignal,
                     Payload &payload) {

  DEBUG("DirWalker walk with pool start - " << path);
  std::vector<fs::directory_entry> entries(fs::directory_iterator(this->path),
                                           fs::directory_iterator());

  for (int i = 0; i < entries.size(); i++) {

    auto entry = entries[i];
    auto entryPath = entry.path();

    DEBUG_FULL("DirWalker walk with pool entry - " << entryPath);

    // If any thread previously returned ABORT, quit now
    if (abortSignal->load()) {
      return;
    }

    if (obeyGitIgnore && repo.isPathIgnored(entryPath)) {
      continue;
    }

    auto ext = entryPath.extension();

    if(!matchExt.empty() 
        && !entry.is_directory()
        && (matchExt.find(ext.string()) == matchExt.end())){
      continue;
    }

    File file(entries[i]);
    file.level = level;

    ACTION actRes;
    if(!filesOnly || !file.isDir){
      actRes = callAction(action, QUEUING, file, repo, payload);
    } else{
      actRes = ACTION::CONTINUE;
    }

    if (actRes == ACTION::STOP) {
      DEBUG("DirWalker with pool stop - " << file.pathStr);
      return;
    }
    if (actRes == ACTION::SKIP){
      DEBUG("DirWalker with pool skip - " << file.pathStr);
      continue;
    }
    if (actRes == ACTION::ABORT) {
      DEBUG("DirWalker with pool abort - " << file.pathStr);
      abortSignal->store(true);
      return;
    }
 
    if (!((file.name == ".") || (file.name == "..")) && file.isDir &&
        recursive && !inverted) {
      DirWalker child(file.path);
      child.copyConfig(this);
      child.level = level + 1;
      child.walk(repo, pool, action, abortSignal, payload);
    } else if (inverted && i == entries.size() - 1) {
      fs::path parent = fs::absolute(path).parent_path();
      if (path == ".")
        parent = parent.parent_path();
      if (parent.has_relative_path()) {
        DirWalker child(parent.string());
        child.copyConfig(this);
        child.level = level - 1;
        child.recursive = false;
        child.inverted = true;

        child.walk(repo, pool, action, abortSignal, payload);
      }
    } else {

      DEBUG_FULL("DirWalker walk with pool enqueue job - " << file.pathStr);
      // create a anonlymous class that has action and file in constructor
      pool.enqueue([action, file, &repo, abortSignal, &payload]() {
        if (abortSignal->load())
          return;

        DEBUG("DirWalker walk with pool do job - " << file.pathStr);
        ACTION actRes = callAction(action, OPENED, file, repo, payload);
        DEBUG("DirWalker walk with pool job done- " << file.pathStr);
        if (actRes == ACTION::ABORT) {
          DEBUG("DirWalker with pool abort called");
          abortSignal->store(true);
        }
      });
    }
  }
}

// PcreCache 
pcre2_code * PcreCache::get(const std::string &pattern, uint32_t opt_compile) {
    Key k{pattern, opt_compile};
    {
      DEBUG_FULL("PcreCache lock mtx");
      std::lock_guard<std::mutex> lock(mtx);
      auto it = cache.find(k);
      if (it != cache.end()){
        DEBUG_FULL("PcreCache found from cache");
        return it->second;
      }
    }

    DEBUG_FULL("PcreCache compile pattern - " << pattern);
    int errornumber;
    PCRE2_SIZE erroroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern.c_str(),
                                   PCRE2_ZERO_TERMINATED, opt_compile,
                                   &errornumber, &erroroffset, NULL);
    if (re == NULL) {
      PCRE2_UCHAR buf[256];
      pcre2_get_error_message(errornumber, buf, sizeof(buf));
      std::string msg = "PcreCache: regex compile failed for pattern: '" + pattern + "'\n";
      msg += "  Error: " 
            + std::string(reinterpret_cast<char*>(buf)) 
            + " (code " + std::to_string(errornumber) + ")\n"
            + "  At offset: " + std::to_string(erroroffset) + "\n";

      size_t ctx = 10;
      size_t start = (erroroffset > ctx) ? erroroffset - ctx : 0;
      size_t end = std::min(pattern.size(), static_cast<size_t>(erroroffset + ctx));
      msg += "  Context: ..." + pattern.substr(start, erroroffset - start) + ">>><<<";
      
      if (erroroffset < pattern.size()){
        msg += pattern.substr(erroroffset, end - erroroffset);
      }

      msg += "...\n";
      throw std::invalid_argument(msg);
    }
    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    DEBUG_FULL("PcreCache compile pattern done - " << pattern);
    std::lock_guard<std::mutex> lock(mtx);
    auto [it, _] = cache.emplace(k, re);
    return it->second;
}

//TSEnginePool 
std::shared_ptr<TSEngine> TSEnginePool::get(const TSLanguage* lang){
  
  DEBUG_FULL("TSEnginePool get lock mtx");
  std::lock_guard<std::mutex> lock(mtx);
  auto it = engines.find(lang);
  if (it != engines.end()){
    DEBUG_FULL("TSEnginePool get found from cache");
    return it->second;
  }

  auto ptr = std::make_shared<TSEngine>(lang);
  engines[lang] = ptr;
  return ptr;
}

// QueryCache
TSQuery* QueryCache::get(TSEngine* engine, const std::string& pattern) {

  DEBUG_FULL("QueryCache get lock mtx");
  std::lock_guard<std::mutex> lock(mtx);
  auto key = std::make_pair(engine, pattern);
  auto it = cache.find(key);
  if (it != cache.end()){
    DEBUG_FULL("QueryCache found from cache");
    return it->second;
  }
  
  TSQuery* q = engine->queryNew(const_cast<std::string&>(pattern));
  cache[key] = q;
  return q;
}


// ThreadPool
ThreadPool::ThreadPool(size_t maxCount) {
  DEBUG_FULL("ThreadPool ctor");
  this->maxCount = maxCount;
  stop = false;
  for (size_t i = 0; i < maxCount; ++i) {

    workers.emplace_back([this] {

      DEBUG_FULL("ThreadPool worker ctor");
      // constructor of Thread callable
      while (true) {
        DEBUG_FULL("ThreadPool worker next");
        std::function<void()> job;
        {
          DEBUG_FULL("ThreadPool worker lock queue");
          std::unique_lock<std::mutex> lock(queueMutex);
          DEBUG("ThreadPool worker wait for task");
          // Wait until there is a task or we are stopping
          enqueueCondition.wait(lock, [this] { return stop || !task.empty(); });
          if (stop && task.empty())
            return;

          job = std::move(task.front());
          task.pop();
        }
        DEBUG("ThreadPool worker do job");
        job(); // Execute the action
        DEBUG("ThreadPool worker job done");
        if (activeTasks.fetch_sub(1) == 1) {
          DEBUG("ThreadPool worker all jobs done");
          // this was the last job
          finishCondition.notify_all();
        }
      }
    });
  }
}
ThreadPool::~ThreadPool() {
  DEBUG_FULL("ThreadPool destroyed");
  {
    std::unique_lock<std::mutex> lock(queueMutex);
    stop = true;
  }
  enqueueCondition.notify_all(); // Wake up all threads to let them finish
  for (std::thread &worker : workers) {
    worker.join(); // Wait for every thread to finish its current job
  }
}

template <class F> void ThreadPool::enqueue(F &&f) {
  {
    std::unique_lock<std::mutex> lock(queueMutex);
    activeTasks++;
    task.emplace(std::forward<F>(f));
  }
  enqueueCondition.notify_one();
}

// CSTTree

CSTTree::CSTTree(TSTree *tree, std::string_view source, TSEngine &parent)
    : source(source), parent(parent) , tree(tree){
  DEBUG_FULL("CSTTree ctor");
};

CSTTree::~CSTTree() { 
  DEBUG_FULL("CSTTree destroyed");
};

std::string CSTTree::sTree() {
  DEBUG_FULL("CSTTree sTree");
  TSNode node = ts_tree_root_node(tree.get());
  char *raw = ts_node_string(node);
  auto res = std::string(raw);
  free(raw);
  return res;
};

void CSTTree::getQueryForNode(TSNode node, std::string &query, size_t level) {
  DEBUG_FULL("CSTTree getQueryForNode");
  query.append(std::string(level, '\t'));
  query.append("(");
  query.append(ts_node_type(node));

  uint32_t count = ts_node_child_count(node);

  for (size_t i = 0; i < count; ++i) {
    TSNode child = ts_node_child(node, i);
    if (!ts_node_is_named(child))
      continue;
    query.append("\n");
    getQueryForNode(child, query, level + 1);
    query.append(std::string(level, '\t'));
  }

  query.append(")");
  // query.append("@");
  // query.append(ts_node_type(node));
  // query.append("_"+std::to_string(level));
  query.append("\n");
};

std::string CSTTree::getText(TSNode n){
  DEBUG_FULL("CSTTree getText");
  auto sb = ts_node_start_byte(n);
  auto eb = ts_node_end_byte(n);
  return std::string(source.substr(sb, eb - sb));
};

std::string CSTTree::asQuery() {
  DEBUG_FULL("CSTTree asQuery");
  std::string query;
  TSNode node = ts_tree_root_node(tree.get());
  getQueryForNode(node, query);
  return query;
};

template <typename cb> 
void CSTTree::find(TSQuery *query, cb handle) {
  DEBUG("CSTTree find start");
  TSNode root = ts_tree_root_node(tree.get());
  TSQueryCursor *cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, root);
  TSQueryMatch match;

  while (ts_query_cursor_next_match(cursor, &match)) {
    DEBUG("CSTTree find handle start");
    handle(match);
    DEBUG("CSTTree find handle end");
  }

  ts_query_cursor_delete(cursor);
  DEBUG("CSTTree find end");
}

bool CSTTree::validate(const TSInputEdit ed, size_t insertL, size_t delL) {
  size_t size = source.size();

  if (ed.start_byte > size)
    return false;
  if (ed.old_end_byte > size)
    return false;
  if (ed.new_end_byte < ed.start_byte)
    return false;
  if (ed.old_end_byte < ed.start_byte)
    return false;
  if (insertL != 0 || delL != 0) {
    if (ed.old_end_byte != ed.start_byte + delL)
      return false;
    if (ed.new_end_byte != ed.start_byte + insertL)
      return false;
  }

  if (!(ed.start_byte <= ed.old_end_byte && ed.start_byte <= ed.new_end_byte))
    return false;

  return true;
};

void CSTTree::edit(const TSInputEdit ed, const std::string &source) {
  DEBUG("CSTTree edit");
  this->source = source;
  ts_tree_edit(tree.get(), &ed);
  auto newTree = parent.parse(*this, source);
  tree = std::move(newTree.tree);
  newTree.tree = nullptr;
}

void CSTTree::sync(){
  DEBUG("CSTTree sync");
  auto newTree = parent.parse(source);
  tree = std::move(newTree.tree);
  newTree.tree = nullptr;
}

std::vector<TSRange> CSTTree::getErrors() {
  std::string q = R"(
      [
         (ERROR)
         (MISSING)
      ] @syntax.error
  )";

  DEBUG("CSTTree getErrors start");
  // parent is reference of TSEngine , is &parent ok here? 
  TSQuery *sq = QueryCache::global().get(&parent, q);
  std::vector<TSRange> errors;
  find(sq, [&errors](TSQueryMatch m) mutable {
    for (size_t i = 0; i < m.capture_count; i++) {
      TSNode n = m.captures[i].node;
      TSRange r = {ts_node_start_point(n), ts_node_end_point(n),
                   ts_node_start_byte(n), ts_node_end_byte(n)};
      errors.push_back(r);
    }
  });

  DEBUG("CSTTree getErrors ends");
  return errors;
}

// TSEngine

TSEngine::TSEngine(const TSLanguage *lang) {
  // TODO: should lang be a shared pointer
  this->lang = lang;
  // TODO: should parser be a shared pointer or should it come from pool
  //
  DEBUG_FULL("TSEngine ctor");
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);
  this->parser = parser;
};

TSEngine::~TSEngine() { 
  DEBUG_FULL("TSEngine destroyed");
  ts_parser_delete(parser);
  parser = nullptr;
};

const CSTTree TSEngine::parse(FileReader &reader) {
  DEBUG("TSEngine parse begin");
  TSTree *tree = ts_parser_parse(parser, NULL, reader.asTsInput());
  DEBUG("TSEngine parse end");
  return CSTTree(tree, reader.get(reader.bufStart, reader.bufSize), *this);
}

const CSTTree TSEngine::parse(FileWriter &writer) {
  auto source = writer.snapshot().cont;
  DEBUG("TSEngine parse begin");
  TSTree *tree =
      ts_parser_parse_string(parser, NULL, source.data(), source.length());
  DEBUG("TSEngine parse end");
  return CSTTree(tree, source, *this);
}

const CSTTree TSEngine::parse(std::string_view source) {
  DEBUG("TSEngine parse begin");
  TSTree *tree =
      ts_parser_parse_string(parser, NULL, source.data(), source.length());
  DEBUG("TSEngine parse end");
  return CSTTree(tree, source, *this);
};

const CSTTree TSEngine::parse(const CSTTree &old, std::string_view source) {
  DEBUG("TSEngine parse begin");
  TSTree *tree =
      ts_parser_parse_string(parser, old.tree, source.data(), source.length());
  DEBUG("TSEngine parse end");
  return CSTTree(tree, source, *this);
};

TSQuery *TSEngine::queryNew(std::string &queryExpr) {
  DEBUG("TSEngine queryNew " << queryExpr);
  uint32_t errorOffset;
  TSQueryError error;

  TSQuery *query = ts_query_new(lang, queryExpr.c_str(), queryExpr.length(),
                                &errorOffset, &error);

  if (error != TSQueryErrorNone) {
    const char* errType = "Unknown";
    switch (error) {
      case TSQueryErrorSyntax: errType = "Syntax"; break;
      case TSQueryErrorNodeType: errType = "NodeType"; break;
      case TSQueryErrorField: errType = "Field"; break;
      case TSQueryErrorCapture: errType = "Capture"; break;
      case TSQueryErrorStructure: errType = "Structure"; break;
      default: break;
    }
    std::string msg = "TSEngine::queryNew error: type=";
    msg += errType;
    msg += ", offset=" + std::to_string(errorOffset) + ", expr='" + queryExpr + "'";
    throw std::runtime_error(msg);
  }
  
  return query;
};

std::map<std::string, std::string> TSEngine::getAvailableNodeTypes() {
    std::map<std::string, std::string> result;

    uint32_t symbol_count = ts_language_symbol_count(lang);

    for (uint32_t i = 0; i < symbol_count; ++i) {
        const char* name = ts_language_symbol_name(lang, i);
        TSSymbolType type = ts_language_symbol_type(lang, i);

        if (!name) continue;

        std::string typeStr;
        switch (type) {
            case TSSymbolTypeRegular:
                typeStr = "regular";
                break;
            case TSSymbolTypeAnonymous:
                typeStr = "anonymous";
                break;
            case TSSymbolTypeAuxiliary:
                typeStr = "auxiliary";
                break;
            case TSSymbolTypeSupertype:
                typeStr = "supertype";
                break;
            default :
                typeStr = "unknown";
                break;
        }

        result[name] = typeStr;
    }

    return result;
}

TSRange TSEngine::getRange(TSNode n){
  TSRange r = {
     ts_node_start_point(n),
     ts_node_end_point(n),
     ts_node_start_byte(n),
     ts_node_end_byte(n)
  };
  return r;
};

// LibGit

std::once_flag LibGit::lib_git_init; // needs a global instance to track init
                                    
void LibGit::init(){
    std::call_once(lib_git_init, +[](){
        DEBUG("LibGit init");
        git_libgit2_init();
    });
}

LibGit::LibGit(git_repository *repo) {
  assert(repo != nullptr);
  DEBUG_FULL("LibGit ctor");
  init();
  this->repo = make_repo(repo);
  root = git_repository_workdir(repo);
  setSignature(username, email);
}

LibGit::~LibGit(){
  DEBUG_FULL("LibGit destroyed");
  git_signature_free(signature);
}

LibGit::RepoPtr LibGit::make_repo(git_repository *raw){
    return RepoPtr(raw, [](git_repository* r) {
        git_repository_free(r); 
        // this will free when the last instance of LibGit using this is delete
        // also this is thread safe
    });
}

LibGit LibGit::clone(std::string url, std::string path, bool shallow, git_clone_options opts){
  init();
  DEBUG("LibGit clone start");
  try{
    return open(path);
  }catch(std::runtime_error e){
    git_repository* repo = nullptr;
    
    if(shallow){
      opts.fetch_opts.depth = 1;
    }

    if(git_clone(&repo, url.c_str(), path.c_str(), &opts) < 0){
      auto e = git_error_last();
      throw std::runtime_error(std::string("Unable to clone repository at " + url + " due to :") + 
                        ((e && e->message) ? e->message : "Unknown"));
    }
    DEBUG("LibGit clone done");
    return LibGit(repo);
  }
}

LibGit LibGit::open(std::string path){
  git_repository* repo = nullptr;
  init();
  DEBUG("LibGit open start");
  if (git_repository_open(&repo, path.c_str()) < 0) {
    const git_error* e = git_error_last();

    throw std::runtime_error(std::string("Unable to open repository at " + path + " due to : ") +
      (e && e->message ? e->message : "Unknown"));
  }

  DEBUG("LibGit open done");
  return LibGit(repo);
}

bool LibGit::isPathIgnored(fs::path path){
  return isPathIgnored(path.string());
}

bool LibGit::isPathIgnored(std::string path){
  DEBUG_FULL("LibGit isPathIgnored");
  int ignored;
  if(git_ignore_path_is_ignored(&ignored, repo.get(), path.c_str()) < 0){
    return false;
  }
  return ignored == 1;
}

void LibGit::add(const fs::path &path) {
  git_index *index = nullptr;

  fs::path relPath = fs::relative(path, root);

  DEBUG("LibGit add " << relPath.c_str());

  std::lock_guard<std::mutex> lock(gitMutex);
  int err = 0;
  err = git_repository_index(&index, repo.get());
  err = git_index_add_bypath(index, relPath.c_str());
  err = git_index_write(index);
  git_index_free(index);

  if(err < 0){
    const git_error* e = git_error_last();
    throw std::runtime_error(std::string("Unable to add file ") + relPath.c_str() + " due to : " +
        (e && e->message ? e->message : "Unknown"));
  }
};

void LibGit::addAll(){
  DEBUG("LibGit add all - " << root);

  std::lock_guard<std::mutex> lock(gitMutex);
  int err = 0;
  git_index* index;
  err = git_repository_index(&index, repo.get());
  err = git_index_add_all(index, NULL, 0, NULL, NULL);
  git_index_free(index);
  if(err < 0) {
    throw std::runtime_error("Failed to add all files to index");
  }
}

void LibGit::addIgnoreRule(std::string rule){
  git_ignore_add_rule(repo.get(), rule.c_str());
}

void LibGit::checkout(std::string blobId, git_checkout_options opts){
  DEBUG("LibGit checkout - " << blobId);

  std::lock_guard<std::mutex> lock(gitMutex);
  git_object *target = nullptr;
  git_reference *ref = nullptr;

  int err = 0;

  if (git_reference_dwim(&ref, repo.get(), blobId.c_str()) == 0) {
    //It's a branch/tag

    const char* refname = git_reference_name(ref);
    DEBUG("LibGit checkout branch/tag ref - " << refname);
    err = git_repository_set_head(repo.get(), refname);
    err = git_checkout_head(repo.get(), &opts);
    git_reference_free(ref);
  } else {
    //It's a commit/tree
    
    DEBUG("LibGit checkout commit/tree target - " << blobId);
    err = git_revparse_single(&target, repo.get(), blobId.c_str()); 
    err = git_checkout_tree(repo.get(), target, &opts);
    err = git_repository_set_head_detached(repo.get(), git_object_id(target)); 
    git_object_free(target);  
  }

  if(err < 0){
    const git_error* e = git_error_last();
    throw std::runtime_error(std::string("Unable to checkout ") + blobId + " due to : " +
        (e && e->message ? e->message : "Unknown"));
  }
}

bool LibGit::branchExists(std::string name){
  git_reference* ref = nullptr;
  bool exists = git_reference_dwim(&ref, repo.get(), name.c_str()) == 0;
  if(ref){
    git_reference_free(ref);
  }
  return exists;
}

void LibGit::branchCreate(std::string name){
  DEBUG("LibGit branchCreate - " << name);
  std::lock_guard<std::mutex> lock(gitMutex);
  git_reference *ref = nullptr;
  git_object *head = nullptr;
  git_commit *commit = nullptr;
  
  int err = 0;
  err = git_revparse_single(&head, repo.get(), "HEAD");  
  err = git_object_peel((git_object**)&commit, head, GIT_OBJECT_COMMIT); 
  err = git_branch_create(&ref, repo.get(), name.c_str(), commit, 0); 

  git_commit_free(commit);
  git_object_free(head);
  git_reference_free(ref);

  if(err < 0){
    const git_error* e = git_error_last();
    throw std::runtime_error(std::string("Unable tocreate branch ") + name + " due to : " +
        (e && e->message ? e->message : "Unknown"));
  }
}

void LibGit::setSignature(std::string username, std::string email){
  this->username = username;
  this->email = email;
  if (git_signature_new(&signature, username.c_str(), email.c_str(), time(nullptr), 0) < 0) {
    throw std::runtime_error("Failed to set signature");
  }
}

void LibGit::commit(std::string message) {
  git_index *index = nullptr;
  int err = 0;

  std::lock_guard<std::mutex> lock(gitMutex);

  err = git_repository_index(&index, repo.get()); 
  err = git_index_read(index, 0);

  git_oid tree_id;
  git_tree *tree = nullptr;
  git_reference *ref = nullptr;
  git_commit *parent = nullptr;
  git_oid commit_id;
  err = git_index_write_tree(&tree_id, index);
  err = git_tree_lookup(&tree, repo.get(), &tree_id);
  err = git_repository_head(&ref, repo.get());
  err = git_commit_lookup(&parent, repo.get(), git_reference_target(ref));
  err = git_commit_create_v(&commit_id, repo.get(), "HEAD", signature, signature, NULL, message.c_str(), tree, parent ? 1 : 0, parent);
  git_tree_free(tree);
  git_index_free(index);
  git_commit_free(parent);
  git_reference_free(ref);

  if(err < 0){
    const git_error* e = git_error_last();
    throw std::runtime_error(std::string("Unable to create commit - ") + message + " due to : " +
        (e && e->message ? e->message : "Unknown"));
  }
}

void LibGit::resetHead(git_reset_t opt){
    DEBUG("LibGit reset HEAD - " << opt);
    git_object *target = NULL;
    int err;
    err = git_revparse_single(&target, repo.get(), "HEAD");

    std::lock_guard<std::mutex> lock(gitMutex);
    git_checkout_options opts;
    err = git_reset(repo.get(), target, opt, &opts);
    git_object_free(target);
    
    if(err < 0){
      const git_error* e = git_error_last();
      throw std::runtime_error(std::string("Unable to create HEAD - ")  + " due to : " +
          (e && e->message ? e->message : "Unknown"));
    }
}

std::vector<LibGit::FileDiff> LibGit::diff(){
  return diff("HEAD", "workdir");
}

std::vector<LibGit::FileDiff> LibGit::diff(std::string fromBlobId, std::string toBlobId,
                                          git_diff_options opts) {
  DEBUG("LibGit diff "  << fromBlobId << toBlobId);
  std::vector<FileDiff> result;

  git_object *from_obj = nullptr;
  git_object *to_obj = nullptr;
  git_tree *from_tree = nullptr;
  git_tree *to_tree = nullptr;
  git_diff *diff = nullptr;

  int err = 0;

  // Resolve FROM (can be any git object)
  err = git_revparse_single(&from_obj, repo.get(), fromBlobId.c_str());
  err = git_object_peel((git_object**)&from_tree, from_obj, GIT_OBJECT_TREE);

  // Resolve TO (can be any git object)
  if (toBlobId != "workdir") {
    err = git_revparse_single(&to_obj, repo.get(), toBlobId.c_str());
    err = git_object_peel((git_object**)&to_tree, to_obj, GIT_OBJECT_TREE);
  }

   // Create diff
  if (toBlobId == "workdir") {
    err = git_diff_tree_to_workdir_with_index(
        &diff,
        repo.get(),
        from_tree,
        &opts
        );
  } else {
    err = git_diff_tree_to_tree(
        &diff,
        repo.get(),
        from_tree,
        to_tree,
        &opts
        );
  }

  /*
  git_diff
  └── git_diff_delta (file metadata)
        └── git_patch (optional, content diff)
              └── hunks
                    └── lines
  */

  // Iterate diff
  size_t deltaNum = git_diff_num_deltas(diff); // delta = one file change
  
  DEBUG("LibGit diff deltaNum - "  << deltaNum);
  for (size_t i = 0; i < deltaNum; i++) {
    const git_diff_delta *delta = git_diff_get_delta(diff, i);
    FileDiff f;
    
    f.status = delta->status;
    f.flags =  (git_diff_flag_t) delta->flags;
                delta->nfiles; // number of files in this delta ?
    f.oldPath = delta->old_file.path;
    f.newPath = delta->new_file.path;

    git_patch* patch = nullptr;
    err = git_patch_from_diff(&patch, diff, i);

    size_t hunkNum = git_patch_num_hunks(patch);
    DEBUG("LibGit diff hunkNum - "  << hunkNum);
    for(int j = 0; j < hunkNum; j++){
      const git_diff_hunk* hunk;

      size_t lineNum = 0;
      err = git_patch_get_hunk(&hunk, &lineNum, patch , j);

      Hunk h;
      h.oldStartLine = hunk->old_start; 
      h.oldLinesCount = hunk->old_lines; 
      h.newStartLine = hunk->new_start; 
      h.newLinesCount = hunk->new_lines; 
      h.header = std::string(hunk->header); 

      DEBUG("LibGit diff lineNum - "  << lineNum);
      for(int k = 0; k < lineNum; k++){
        const git_diff_line* line;
        git_patch_get_line_in_hunk(&line, patch, j, k);
        LineDiff l;
        l.type = (git_diff_line_t) line->origin;
        l.oldLineNo = line->old_lineno;
        l.newLineNo = line->new_lineno;
        l.fileOffset = line->content_offset;
        l.cont = std::string(line->content, line->content_len);

        //TODO
        l.blameAuthor;
        l.blameCommit;

        h.lineDiffs.push_back(l);
        // git_line_free(line); // line is ref counted
      }
      f.hunks.push_back(h);
      //git_hunk_free(hunk); //hunk is ref counted
    }

    result.push_back(f);
    git_patch_free(patch);
  }
  
  git_diff_free(diff);
  git_tree_free(from_tree);
  git_tree_free(to_tree);
  git_object_free(from_obj);
  git_object_free(to_obj);

  if (err < 0) {
    const git_error* e = git_error_last();
    throw std::runtime_error(
        std::string("Diff failed: ") +
        (e && e->message ? e->message : "Unknown")
        );
  }

  return result;
}


#endif // LIB_IMPLEMENTATION
