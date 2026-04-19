
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

class LibGit {

  using RepoPtr = std::shared_ptr<git_repository>;
  static RepoPtr make_repo(git_repository* repo);
 
  RepoPtr repo;
  std::string root;
  std::mutex gitMutex;
  static std::once_flag lib_git_init;
  static void init();

public:
  LibGit(git_repository *repo);
  ~LibGit();

  static LibGit clone(std::string url, std::string path = ".", bool shallow = false);
  static LibGit open(std::string path = ".");
 
  bool isPathIgnored(fs::path path);
  bool isPathIgnored(std::string path);
  
  void addIgnoreRule(std::string rule);

  void add(fs::path &path);
};



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

  git_repository *repo;

  File(std::string path);
  File(fs::directory_entry entry);
  File();
  ~File();

  void sync();

  static bool deleteFile(File &target); // deletes entry file and commits
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
  std::ifstream iFileStream;
  File file;
  void readFileMetadata();
  bool _isValid = false;
  size_t pos;

  static const char *tsRead(void *payload, uint32_t byte_index, TSPoint point,
                            uint32_t *bytes_read);

  char *buf = nullptr;

public:
  size_t level = 0;
  std::vector<size_t> rowOffsets;
  size_t bufStart;
  size_t bufSize;
  static constexpr size_t defaultBlockSize = 1024 * 1024;
  size_t blockSize = defaultBlockSize;
  bool readReverse;
  bool snapShotMode; // disables fresh load and sync

  FileReader(File file, size_t blockSize = defaultBlockSize);
  FileReader(std::string filePath, size_t blockSize = defaultBlockSize);
  FileReader(const FileSnapshot snap, size_t blockSize = defaultBlockSize);
  FileReader(const FileReader &copy);
  FileReader() {};
  ~FileReader();

  bool isValid() { return _isValid; };
  File getFile() { return file; };

  typedef struct {
    char *cont;
    size_t size;
  } block;

  block sync();
  block load(size_t from, size_t to);
  std::string_view get();
  std::string_view get(size_t from, size_t to);
  std::string_view getLine(size_t row);
  void reset();
  block readBlockAt(size_t pos);
  block next();
  block prev();

  TSInput asTsInput();
  typedef struct {
    TSRange match;
    std::vector<TSRange> captures;
  } MatchResult;

  std::vector<MatchResult> find(std::string pattern, bool regex = false,
                                uint32_t opt_compile = PCRE2_CASELESS);
  std::vector<MatchResult> findWith(pcre2_code *re,
                                    uint32_t opt_match = PCRE2_NO_UTF_CHECK); // some compile 
                                                                              // options allowed 
  TSPoint getP(size_t byteOffset);

  const FileSnapshot snapshot();

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

TSPoint _getP(size_t byteOffset, std::vector<size_t> rowOffsets);

class FileWriter {
  File file;
  bool _isValid;
  std::ofstream oFileStream;
  FileSnapshot snap;

public:
  FileWriter(const FileSnapshot snap);
  FileWriter(std::string path);
  FileWriter(File f);
  FileWriter(const FileWriter &copy);
  ~FileWriter();

  bool isValid() { return _isValid; };
  File getFile() { return file; };
  const FileSnapshot snapshot() const { return snap; };

  std::vector<size_t> rowOffsets;

  TSPoint getP(size_t byteOffset);

  bool save(); // save buf to underling file
  bool
  backup(const std::string &suffix = ".bak"); // create a backup in same folder
  bool
  flush(std::string &path); // create if non existing , will over write existing

  FileWriter &copy(std::string &path); // load file cont to buf

  // overwrite
  FileWriter &write(const std::string &content); // replace entire buf content
  FileWriter &write(size_t offset, char *newCont, size_t newContLen);
  FileWriter &write(size_t offset, std::string &cont);
  FileWriter &write(size_t from, size_t to, std::string &cont);

  FileWriter &append(std::string &cont);
  FileWriter &insert(size_t offset, std::string &newCont);
  FileWriter &insertRow(size_t row, const std::string &line);
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
    OP op;
    TSRange range;
    std::string change;
    std::string context;
    pcre2_code *rc;
  };
  struct Error {
    ERROR e;
    TSRange range;
    Edit edit;
  };
  FileEditor();
  void queue(Edit e); // unordered
  void reset();
  std::vector<Error> getConflictErrors();
  std::vector<Error> apply(CSTTree &original, FileWriter &writer);

private:
  std::vector<Edit> operations;
  std::vector<Error> errors;

  static TSPoint getNewEndPoint(const Edit& edit);
};

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
      actRes = action(STATUS::OPENED, file, repo, payload);
    } else if constexpr (std::is_invocable_v<Action, STATUS, File, LibGit&>) {
      actRes = action(STATUS::OPENED, file, repo);
    } else if constexpr (std::is_invocable_v<Action, STATUS, File>) {
      actRes = action(STATUS::OPENED, file);
    } else{
      throw std::invalid_argument("Invalid signature for walk action");
    }
    return actRes;
  }

  bool isPathIgnored(std::string path);


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
  TSTree *tree;
  std::string_view source;
  TSEngine &parent;

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
};

class TSEngine {
  const TSLanguage *lang;
  TSParser *parser;
  std::map<std::string, TSQuery*> queryCache;

public:
  TSEngine(const TSLanguage *lang);
  ~TSEngine();
  const CSTTree parse(std::string_view source);
  const CSTTree parse(const CSTTree &old, std::string_view modSource);
  const CSTTree parse(FileReader &reader);
  const CSTTree parse(FileWriter &writer);

  static TSRange getRange(TSNode n);

  // TSQuery is not thread safe although it is immutable
  // unpredictable cursors
  // use thread_local
  TSQuery *queryNew(std::string &queryExpr);
};


#endif // LIB_H_

// ----------------------------------------------------------
// IMPL
// ----------------------------------------------------------

#ifndef LIB_IMPLEMENTATION
#define LIB_IMPLEMENTATION

File::File(std::string path) {
  dir_entry = fs::directory_entry(path);
  this->pathStr = path;
  level = 0;
  if (dir_entry.exists()) {
    this->path = fs::absolute(dir_entry.path().lexically_normal());
    name = this->path.filename();
    ext = this->path.extension();
    isDir = dir_entry.is_directory();
    isReg = dir_entry.is_regular_file();
    status = dir_entry.status();
    if (isReg) {
      size = dir_entry.file_size();
    } else {
      size = 0;
    }
    isValid = true;
  } else {
    isValid = false;
  }
};

File::File(fs::directory_entry entry) {
  dir_entry = entry;
  this->pathStr = entry.path();
  level = 0;
  if (dir_entry.exists()) {
    this->path = fs::absolute(dir_entry.path().lexically_normal());
    name = this->path.filename();
    ext = this->path.extension();
    isDir = dir_entry.is_directory();
    isReg = dir_entry.is_regular_file();
    status = dir_entry.status();
    if (isReg) {
      size = dir_entry.file_size();
    } else {
      size = 0;
    }
    isValid = true;
  } else {
    isValid = false;
  }
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
};

bool File::deleteFile(File &target) {
  if (target.isDir)
    return false;
  return fs::remove(target.path);
};

int File::deleteDir(File &target) {
  if (!target.isDir)
    return -1;
  return fs::remove_all(target.path);
};

bool File::rename(File &file, std::string name) {
  fs::rename(file.pathStr, name);
  file.path = fs::path(name);
  file.pathStr = name;
  file.dir_entry = fs::directory_entry(file.path);
  file.sync();
  return true;
};

File::~File() {};

// FileReader

#define UPDATE_ROW_OFFSETS(data, len)                                          \
  rowOffsets.clear();                                                          \
  rowOffsets.push_back(0);                                                     \
  for (size_t i = 0; i < (len); ++i) {                                         \
    if ((data)[i] == '\n') {                                                   \
      rowOffsets.push_back(i + 1);                                             \
    }                                                                          \
  }

FileReader::FileReader(File file, size_t blockSize)
    : iFileStream(file.path, std::ios::binary | std::ios::ate) {
  this->file = file;
  this->blockSize = blockSize;
  _isValid = !file.isDir;
  readFileMetadata();
  snapShotMode = false;
};

FileReader::FileReader(std::string filePath, size_t blockSize)
    : iFileStream(filePath.c_str(), std::ios::binary | std::ios::ate) {
  this->file = File(filePath);
  if (file.isValid) {
    _isValid = true && !file.isDir;
    this->blockSize = blockSize;
    readFileMetadata();
  } else {
    _isValid = false;
  }
  snapShotMode = false;
};

FileReader::FileReader(const FileSnapshot snap, size_t blockSize) {
  snapShotMode = true;
  buf = new char[snap.cont.length()];
  bufStart = 0;
  bufSize = snap.cont.length();
  file = snap.file;
  this->blockSize = blockSize;
  std::memcpy(buf, snap.cont.data(), snap.cont.length());
  _isValid = true;
  UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());
};

FileReader::FileReader(const FileReader &copy) {
  this->iFileStream = std::ifstream(copy.file.pathStr);
  this->file = copy.file;
  this->_isValid = copy._isValid;
  this->pos = copy.pos;
  this->buf = new char[copy.bufSize];
  this->blockSize = copy.blockSize;
  memcpy(this->buf, copy.buf, copy.bufSize);
  this->level = copy.level;
  this->rowOffsets = copy.rowOffsets;
  this->bufStart = copy.bufStart;
  this->bufSize = copy.bufSize;
  this->readReverse = copy.readReverse;
  this->snapShotMode = copy.snapShotMode;
}

void FileReader::readFileMetadata() {
  if (iFileStream.is_open() && file.isValid && file.size != 0) {

    bufSize = file.size;
    bufStart = 0;

    iFileStream.clear();
    iFileStream.seekg(0, std::ios::beg);

    rowOffsets.reserve(file.size / 50);
    rowOffsets.push_back(0); // row no 0
    size_t blockSize = std::min(this->blockSize, file.size);

    buf = new char[blockSize];
    size_t currentOffset = 0;

    while (iFileStream) {
      iFileStream.read(buf, blockSize);
      std::streamsize bytesRead = iFileStream.gcount();

      if (bytesRead <= 0)
        break;

      for (std::streamsize i = 0; i < bytesRead; ++i) {
        if (buf[i] == '\n') {
          rowOffsets.push_back(currentOffset + i + 1);
        }
      }

      bufStart = currentOffset;
      bufSize = blockSize;
      currentOffset += bytesRead;
    }

    iFileStream.read(buf, file.size);
    iFileStream.clear();

  } else {
    bufSize = 0;
    bufStart = 0;
    _isValid = false;
  }
};

FileReader::block FileReader::sync() {
  if (!_isValid)
    return {nullptr, 0};

  if (snapShotMode)
    return {buf, bufSize};

  file.sync();

  if (buf) {
    delete[] buf;
    bufSize = 0;
  }

  iFileStream.clear();
  iFileStream.seekg(0, std::ios::beg);
  buf = new char[file.size];
  bufStart = 0;
  bufSize = file.size;
  iFileStream.read(buf, file.size);
  iFileStream.clear();

  UPDATE_ROW_OFFSETS(buf, file.size);

  return {buf, file.size};
};

std::string_view FileReader::get() { return get(bufStart, bufSize); }

std::string_view FileReader::get(size_t from, size_t to) {
  if (!_isValid)
    return {};

  if (file.isValid && (from > file.size || to > file.size))
    return {};

  size_t length = to - from;

  if (buf == nullptr || (from > 0 && from < bufStart) ||
      (to < file.size && to > bufSize + bufStart))
    if (load(from, to).cont == nullptr)
      return {};
  return std::string_view(&buf[from - bufStart], length);
};

std::string_view FileReader::getLine(size_t row) {
  // this has caused OOM due to unbounded access over the array :)
  if(row + 1 == rowOffsets.size()){
    return get(rowOffsets[row], this->file.size);
  }else if (row >= rowOffsets.size()){
    return "";
  }
  return get(rowOffsets[row], rowOffsets[row + 1]);
}

FileReader::block FileReader::load(size_t from, size_t to) {
  if (!_isValid)
    return {nullptr, 0};

  if (snapShotMode)
    return {buf, bufSize};

  if (from > file.size || to > file.size || to == 0)
    return {nullptr, 0};

  if (buf) {
    delete[] buf;
    bufSize = 0;
  }

  size_t length = to - from;
  buf = new char[length];
  iFileStream.clear();
  iFileStream.seekg(from, std::ios::beg);

  if (!iFileStream.read(buf, length)) {
    delete[] buf;
    bufSize = 0;
    return {nullptr, 0};
  }

  bufStart = from;
  bufSize = length;

  return {buf, length};
};

FileReader::block FileReader::readBlockAt(size_t pos) {
  if (!_isValid)
    return {nullptr, 0};
  if (pos >= file.size)
    return {nullptr, 0};

  size_t size = std::min(FileReader::defaultBlockSize, file.size - pos);

  if (!buf || pos < bufStart || pos + size > bufStart + bufSize) {
    load(pos, pos + size);
    bufStart = pos;
  }

  return {buf + (pos - bufStart), size};
}

TSInput FileReader::asTsInput() {
  TSInput input;
  input.payload = this;
  input.read = &FileReader::tsRead;
  input.encoding = TSInputEncodingUTF8;
  return input;
};

const char *FileReader::tsRead(void *payload, uint32_t byte_index,
                               TSPoint position, uint32_t *bytes_read) {
  auto *reader = static_cast<FileReader *>(payload);

  if (byte_index >= reader->file.size) {
    *bytes_read = 0;
    return nullptr;
  }

  size_t blockSize =
      std::min(reader->blockSize, reader->file.size - byte_index);

  // Ensure buffer covers requested range
  if (!reader->buf || byte_index < reader->bufStart ||
      byte_index + blockSize > reader->bufStart + reader->bufSize) {

    reader->load(byte_index, byte_index + blockSize);
    reader->bufStart = byte_index;
  }

  *bytes_read = static_cast<uint32_t>(blockSize);
  return reader->buf + (byte_index - reader->bufStart);
}

std::vector<FileReader::MatchResult> FileReader::find(std::string pattern,
                                                      bool regex, uint32_t opt) {

  std::vector<MatchResult> matches;

  if (regex) {
    PCRE2_SPTR pcrePattern = (PCRE2_SPTR)pattern.c_str();


    int errornumber;
    PCRE2_SIZE erroroffset;


    pcre2_code *re = pcre2_compile(pcrePattern, PCRE2_ZERO_TERMINATED, opt,
                                   &errornumber, &erroroffset, NULL);

    if (re == NULL) {
      PCRE2_UCHAR buffer[256];
      pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
      std::cout << "PCRE ERROR compiling : " << buffer << std::endl;
      throw std::invalid_argument(
          "could not compile provided regex for fn find " + pattern + " - " +
          file.pathStr);
    }

    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);

    if (buf == nullptr)
      if (sync().cont == nullptr)
        return matches;

    matches = findWith(re);

    pcre2_code_free(re);

    return matches;
  } else {

    if (buf == nullptr)
      if (sync().cont == nullptr)
        return matches;

    std::string_view searchSpace(buf, bufSize);

    size_t foundPos = 0;
    size_t offset = 0;
    while ((foundPos = searchSpace.find(pattern, offset)) !=
           std::string_view::npos) {

      size_t matchStart = foundPos;
      size_t matchEnd = matchStart + pattern.size();

      TSRange range;
      range.start_byte = static_cast<uint32_t>(matchStart);
      range.end_byte = static_cast<uint32_t>(matchEnd);
      range.start_point = getP(matchStart);
      range.end_point = getP(matchEnd);
      MatchResult match;
      match.match = range;
      matches.push_back(match);

      offset = matchEnd;
    }
    return matches;
  }
};

std::vector<FileReader::MatchResult> FileReader::findWith(pcre2_code *re,
                                                          uint32_t opt) {

  std::vector<MatchResult> matches;

  if (buf == nullptr)
    if (sync().cont == nullptr || bufSize == 0)
      return matches;

  PCRE2_SPTR subject = (PCRE2_SPTR)buf;
  PCRE2_SIZE subject_length = bufSize;
  PCRE2_SIZE *ovector;

  pcre2_match_data *match_data;
  match_data = pcre2_match_data_create_from_pattern(re, NULL);

  int rc = 0;
  PCRE2_SIZE startOffset = 0;
  while (true) {
    rc = pcre2_match(re, subject, subject_length, startOffset, opt, match_data,
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

      TSRange capture;
      capture.start_byte = static_cast<uint32_t>(start);
      capture.end_byte = static_cast<uint32_t>(end);
      capture.start_point = getP(start);
      capture.end_point = getP(end);
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

  return matches;
};

TSPoint _getP(size_t byteOffset, std::vector<size_t> rowOffsets) {

  if (rowOffsets.empty())
    return {0, static_cast<uint32_t>(byteOffset)};

  // Find the first row offset that is GREATER than our byte
  auto it = std::upper_bound(rowOffsets.begin(), rowOffsets.end(), byteOffset);

  if (it == rowOffsets.begin()) {
    return {0, static_cast<uint32_t>(byteOffset)};
  }

  // The row number is the index of the element before 'it'
  uint32_t row = std::distance(rowOffsets.begin(), it) - 1;

  // The column is the difference between our offset and the rows start offset
  uint32_t col = byteOffset - rowOffsets[row];

  return {row, col};
}

TSPoint FileReader::getP(size_t byteOffset) {
  // :: is needed to scope it from outside
  return ::_getP(byteOffset, rowOffsets);
}

const FileSnapshot FileReader::snapshot() {

  FileSnapshot snap = {};

  snap.cont = std::string(buf, bufSize);
  snap.dirty = false;

  if (file.isValid) {
    snap.file = file;
    file.sync();
    sync();
    fs::file_time_type mtim = file.dir_entry.last_write_time();
    snap.lastModified = mtim.time_since_epoch().count();
  }

  return snap;
}

FileReader::block FileReader::next() {
  if (!buf || pos >= file.size || file.size == 0) {
    return {nullptr, 0};
  }

  size_t currentBlockSize = 0;
  if (file.size - pos < defaultBlockSize) {
    currentBlockSize = file.size - pos;
  } else {
    currentBlockSize = defaultBlockSize;
  }

  if (pos < bufStart || pos + currentBlockSize > bufStart + bufSize) {
    load(pos, pos + currentBlockSize);
    bufStart = pos;
  }

  char *currPtr = buf + (pos - bufSize);

  if (readReverse) {
    pos = (pos >= currentBlockSize) ? pos - currentBlockSize : 0;
  } else {
    pos += currentBlockSize;
  }

  return {currPtr, currentBlockSize};
};

FileReader::block FileReader::prev() {
  if (!buf || pos <= 0 || file.size == 0) {
    return {nullptr, 0};
  }

  size_t currentBlockSize = std::min(FileReader::defaultBlockSize, pos);

  if (pos < bufStart || pos + currentBlockSize > bufStart + bufSize) {
    load(pos, pos + currentBlockSize);
    bufStart = pos;
  }

  char *currPtr = buf + (pos - bufSize);

  if (readReverse) {
    if (pos < file.size - 1) {
      pos += currentBlockSize;
    }
  } else if (pos > 0) {
    pos -= currentBlockSize;
  }

  return {currPtr, currentBlockSize};
};

void FileReader::reset() {
  if (buf) {
    delete[] buf;
    buf = nullptr;
  }
  bufSize = 0;
  bufStart = 0;
  if (readReverse) {
    pos = file.size;
  } else {
    pos = 0;
  }
};

FileReader::~FileReader() {
  if (iFileStream.is_open()) {
    iFileStream.close();
  }
  if (buf)
    delete[] buf;
};

// FileWriter

FileWriter::FileWriter(const FileSnapshot snap) {
  this->snap = snap;
  file = snap.file;
  _isValid = file.isValid;
  UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());
};

FileWriter::FileWriter(std::string path) {
  FileReader tmp(path);
  if(tmp.bufSize != tmp.getFile().size){
    tmp.sync();
  }
  snap = tmp.snapshot();
  file = tmp.getFile();
  rowOffsets = tmp.rowOffsets;
  _isValid = file.isValid;
}

FileWriter::FileWriter(File f) {
  FileReader tmp(f);
  if(tmp.bufSize != tmp.getFile().size){
    tmp.sync();
  }
  snap = tmp.snapshot();
  file = tmp.getFile();
  rowOffsets = tmp.rowOffsets;
  _isValid = file.isValid;
}

FileWriter::FileWriter(const FileWriter &copy) {
  snap = copy.snap;
  file = copy.file;
  rowOffsets = copy.rowOffsets;
  _isValid = copy.file.isValid;
};
;

FileWriter::~FileWriter() {
  if (oFileStream.is_open())
    oFileStream.close();
};

bool FileWriter::backup(const std::string &suffix) {
  std::string bkpPath;
  bkpPath = file.pathStr + suffix;
  if (fs::exists(bkpPath)) {
    bkpPath =
        file.pathStr + ".(" + std::to_string(snap.lastModified) + ")" + suffix;
  }
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
  std::ofstream target = std::ofstream(path, std::ios::out | std::ios::trunc);
  target << snap.cont;
  target.flush();
  bool res = target.good();
  target.close();
  return res;
};

#define modifySnap                                                             \
  snap.dirty = true;                                                           \
  snap.lastModified =                                                          \
      std::chrono::system_clock::now().time_since_epoch().count();             \
  snap.file.size = snap.cont.length();                                         \
  UPDATE_ROW_OFFSETS(snap.cont, snap.cont.length());                           \
  return *this;

FileWriter &FileWriter::copy(std::string &sourcePath) {
  if (!fs::exists(sourcePath)) {
    throw std::invalid_argument(
        "path to source file does not exist for: copy path-" + sourcePath);
  }
  FileReader tmp(sourcePath);
  File curr = snap.file;
  snap = tmp.snapshot();
  snap.file = curr;
  modifySnap
};

FileWriter &FileWriter::append(std::string &cont) {
  snap.cont.append(cont);
  modifySnap
}

FileWriter &FileWriter::insert(size_t offset, std::string &slice) {
  assert(offset < snap.cont.size());
  snap.cont.insert(offset, slice);
  modifySnap
};

FileWriter &FileWriter::write(const std::string &content) {
  snap.cont = std::string(content);
  modifySnap
}

FileWriter &FileWriter::write(size_t offset, char *newCont, size_t newContLen) {
  assert(offset < snap.cont.size());
  snap.cont.erase(offset, newContLen);
  snap.cont.insert(offset, newCont, newContLen);
  modifySnap
};

FileWriter &FileWriter::write(size_t offset, std::string &cont) {
  assert(offset < snap.cont.size());
  snap.cont.erase(offset, cont.length());
  snap.cont.insert(offset, cont);
  modifySnap
};

FileWriter &FileWriter::write(size_t from, size_t to, std::string &cont) {
  assert(from >= 0);
  assert(to < snap.cont.size());
  snap.cont.erase(from, to-from);
  snap.cont.insert(from, cont);
  modifySnap
};

FileWriter &FileWriter::deleteCont(size_t from, size_t to) {
  assert(from >= 0);
  assert(to < snap.cont.size());
  snap.cont.erase(from, to - from);
  modifySnap
};

FileWriter &FileWriter::deleteRow(size_t row) {
  assert(rowOffsets.size() > row);
  assert(row > -1);
  size_t row1Offset = rowOffsets[row];
  size_t row2Offset = rowOffsets[row + 1];

  snap.cont.erase(row1Offset, row1Offset - row2Offset);
  modifySnap
};

FileWriter &FileWriter::insertRow(size_t row, const std::string &cont) {
  assert(rowOffsets.size() > row);
  assert(row > -1);
  bool hasEndl = cont[cont.length() - 1] == '\n';
  size_t rowOffset = rowOffsets[row];

  snap.cont.insert(rowOffset, cont);
  if (!hasEndl)
    snap.cont.insert(rowOffset + cont.length(), 1, '\n');
  modifySnap
};

FileWriter &FileWriter::replaceAll(std::string pattern,
                                   std::string templateOrResult, uint32_t opt) {

  int errornumber;
  PCRE2_SIZE erroroffset;

  // Compile pattern
  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern.c_str(), pattern.length(),
                                 0, // default options
                                 &errornumber, &erroroffset, nullptr);

  if (!re) {
    throw std::runtime_error("PCRE2 compilation failed at offset " +
                             std::to_string(erroroffset));
  }

  pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);

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
      re, (PCRE2_SPTR)snap.cont.c_str(), snap.cont.length(), 0, opt, nullptr,
      nullptr, (PCRE2_SPTR)templateOrResult.c_str(), templateOrResult.length(),
      (PCRE2_UCHAR *)buffer.data(), &outLength);

  if (rc == PCRE2_ERROR_NOMEMORY) {
    std::cout << "PCR2_ERROR_NOMEMORY - " << outLength << std::endl;
    goto substitute;
  }

  pcre2_code_free(re);

  if (rc < 0) {
    throw std::runtime_error("PCRE2 substitution failed");
  }

  snap.cont.assign(reinterpret_cast<char *>(buffer.data()), outLength);

  modifySnap
};

FileWriter &FileWriter::replace(std::string pattern,
                                std::string templateOrResult, size_t nth,
                                uint32_t opt) {
  FileReader snapReader(snap);


  int errornumber;
  PCRE2_SIZE erroroffset;

  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern.c_str(), pattern.length(),
                                 0, &errornumber, &erroroffset, nullptr);

  if (!re) {
    throw std::runtime_error("PCRE2 compilation failed at offset " +
                             std::to_string(erroroffset));
  }


  pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);


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
  rc = pcre2_substitute(re, (PCRE2_SPTR)(snap.cont.c_str() + start_offset),
                        end_offset - start_offset, 0, opt, nullptr, nullptr,
                        (PCRE2_SPTR)templateOrResult.c_str(),
                        templateOrResult.length(), (PCRE2_UCHAR *)buffer.data(),
                        &outLength);

  if (rc == PCRE2_ERROR_NOMEMORY) {
    std::cout << "PCR2_ERROR_NOMEMORY - " << outLength << std::endl;
    goto substitute;
  }

  pcre2_code_free(re);

  if (rc < 0) {
    throw std::runtime_error("PCRE2 substitution failed");
  }

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

void FileEditor::queue(FileEditor::Edit e) { operations.push_back(e); };

void FileEditor::reset() {
  operations.clear();
  errors.clear();
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
  return errors;
}

std::vector<FileEditor::Error> FileEditor::apply(CSTTree &original,
                                                 FileWriter &writer) {

  // TODO: maybe handle the conflicts based on some priority
  getConflictErrors();

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

  for (size_t i = 0; i < operations.size(); i++) {
    auto edit = operations[i];
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
      original.edit(te, writer.snapshot().cont);
      break;
    }
    case FileEditor::OP::DELETE:
    {
      te.old_end_byte = edit.range.end_byte;
      writer.deleteCont(edit.range.start_byte, edit.range.end_byte);
      original.edit(te, writer.snapshot().cont);
      break;
    }
    case FileEditor::OP::WRITE:
    {
      te.old_end_byte = edit.range.end_byte;
      te.new_end_byte = edit.range.start_byte + (uint32_t)edit.change.length();
      writer.write(edit.range.start_byte, edit.range.end_byte, edit.change);
      original.edit(te, writer.snapshot().cont);
      break;
    }
    case FileEditor::OP::VALIDATE_CST: {
      for (auto err : original.getErrors()) {
        errors.push_back({CST_ERROR, err, edit});
      }
      break;
    }
    case FileEditor::OP::PRINT_PATH:
    {
      std::cout << writer.snapshot().file.pathStr    << ":" 
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

      const auto& path = writer.snapshot().file.pathStr;

      // one based                        
      const auto startRow = pOld.row     + 1;
      const auto startCol = pOld.column  + 1;
      const auto endRow   = pNew.row     + 1;
      const auto endCol   = pNew.column  + 1;

      const auto oldStart = r.rowOffsets[edit.range.start_point.row] + edit.range.start_point.column;
      const auto oldEnd   = r.rowOffsets[edit.range.end_point.row] + edit.range.end_point.column;

      const std::string_view original = r.get(oldStart, oldEnd);

      std::cout << path << ":" << startRow << ":" << startCol << "\n";
      std::cout << "range: " << startRow << ":" << startCol
                << " -> " << endRow << ":" << endCol << "\n";
      std::cout << this->OP_STR[edit.op] << " : ";
      std::cout << edit.context << "\n";

      std::cout << "<<<<<<<<"   << "\n";
      std::cout.write(original.data(), static_cast<std::streamsize>(original.size())) << "\n";
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
        std::cout << writer.snapshot().file.pathStr << ":"
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

              std::cout << "CONFLICT detected:\n";

              std::cout << "  Edit X : [" << x1 << ", " << x2 << "] -> \""
                << errX.edit.change << "\"\n";

              std::cout << "  Edit Y : [" << y1 << ", " << y2 << "] -> \""
                << errY.edit.change << "\"\n";

              std::cout << "  Overlap: [" << overlap_start << ", "
                << overlap_end << "]\n\n";

              i++; 
              break;
            }
          case CST_ERROR: 
            {
              std::cout << "CST_ERROR:\n";

              std::cout << "  Range: [" 
                << err.range.start_point.row << ":" << err.range.start_point.column
                << " , " 
                << err.range.end_point.row << ":" << err.range.end_point.column << "]\n";

              std::cout << "  Edit : [" 
                << err.edit.range.start_point.row << ":" << err.edit.range.start_point.column
                << ", "
                << err.edit.range.end_point.row << ":" << err.edit.range.end_point.column
                << "] -> \""
                << err.edit.change << "\"\n\n";
              break;
            }
          case CST_MISSING: 
            {
              std::cout << "CST_MISSING:\n";
              std::cout << "  Range: [" 
                << err.range.start_byte
                << ", " 
                << err.range.end_byte 
                << "]\n";
              std::cout << "  Edit : [" 
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

  return errors;
};

// DirWalker
DirWalker::DirWalker(std::string dir) {
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

DirWalker::~DirWalker() {};

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
  auto res = walk(repo, action, payload);
  return res;
};

template <typename Payload, typename Action>
DirWalker::STATUS DirWalker::walk(LibGit& repo, Action &&action,
                                  Payload &payload) {

  if (!_isValid)
    return FAILED;

  std::vector<fs::directory_entry> entries(
      fs::directory_iterator(this->path), // begin it
      fs::directory_iterator()            // end it
  );

  for (size_t i = 0; i < entries.size(); i++) {
    File file(entries[i]);
    file.level = level;
    
    if (obeyGitIgnore && repo.isPathIgnored(file.path)) {
      continue;
    }

    if(!matchExt.empty() 
        && !file.isDir
        && (matchExt.find(file.ext) == matchExt.end())){
      continue;
    }
  
    ACTION actRes;
    if(!filesOnly || !file.isDir){
      actRes = callAction(action, OPENED, file, repo, payload);
    } else{
      actRes = ACTION::CONTINUE;
    }

    if (actRes == ACTION::SKIP) {
      continue;
    } else if (actRes == ACTION::STOP) {
      return STATUS::STOPPED;
    } else if (actRes == ACTION::ABORT) {
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

  std::vector<fs::directory_entry> entries(fs::directory_iterator(this->path),
                                           fs::directory_iterator());

  for (int i = 0; i < entries.size(); i++) {
    File file(entries[i]);
    file.level = level;

    // If any thread previously returned ABORT, quit now
    if (abortSignal->load()) {
      return;
    }

    if (obeyGitIgnore && repo.isPathIgnored(file.path)) {
      continue;
    }

    if(!matchExt.empty() 
        && !file.isDir
        && (matchExt.find(file.ext) == matchExt.end())){
      continue;
    }

    if(!filesOnly || !file.isDir){

      ACTION actRes;
      if(!filesOnly || !file.isDir){
        actRes = callAction(action, QUEUING, file, repo, payload);
      } else{
        actRes = ACTION::CONTINUE;
      }

      if (actRes == ACTION::STOP) {
        return;
      }
      if (actRes == ACTION::SKIP)
        continue;
      if (actRes == ACTION::ABORT) {
        abortSignal->store(true);
        return;
      }
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

      // create a anonlymous class that has action and file in constructor
      pool.enqueue([action, file, &repo, abortSignal, &payload]() {
        if (abortSignal->load())
          return;

        ACTION actRes = callAction(action, OPENED, file, repo, payload);

        if (actRes == ACTION::ABORT) {
          abortSignal->store(true);
        }
      });
    }
  }
}

// ThreadPool
ThreadPool::ThreadPool(size_t maxCount) {
  this->maxCount = maxCount;
  stop = false;
  for (size_t i = 0; i < maxCount; ++i) {

    workers.emplace_back([this] {
      // constructor of Thread callable
      while (true) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lock(queueMutex);
          // Wait until there is a task or we are stopping
          enqueueCondition.wait(lock, [this] { return stop || !task.empty(); });
          if (stop && task.empty())
            return;
          job = std::move(task.front());
          task.pop();
        }
        job(); // Execute the action
        if (activeTasks.fetch_sub(1) == 1) {
          // this was the last job
          finishCondition.notify_all();
        }
      }
    });
  }
}
ThreadPool::~ThreadPool() {
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
    : source(source), parent(parent) {
  this->tree = tree;
};

CSTTree::~CSTTree() { ts_tree_delete(tree); };

std::string CSTTree::sTree() {
  TSNode node = ts_tree_root_node(tree);
  char *raw = ts_node_string(node);
  auto res = std::string(raw);
  free(raw);
  return res;
};

void CSTTree::getQueryForNode(TSNode node, std::string &query, size_t level) {
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
  auto sb = ts_node_start_byte(n);
  auto eb = ts_node_end_byte(n);
  return std::string(source.substr(sb, eb - sb));
};

std::string CSTTree::asQuery() {
  std::string query;
  TSNode node = ts_tree_root_node(tree);
  getQueryForNode(node, query);
  return query;
};

template <typename cb> void CSTTree::find(TSQuery *query, cb handle) {
  TSNode root = ts_tree_root_node(tree);
  TSQueryCursor *cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, root);
  TSQueryMatch match;

  while (ts_query_cursor_next_match(cursor, &match)) {
    handle(match);
  }

  ts_query_cursor_delete(cursor);
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
  this->source = source;
  ts_tree_edit(tree, &ed);
  auto newTree = parent.parse(*this, source);
  ts_tree_delete(tree);
  tree = newTree.tree;
  newTree.tree = nullptr;
}

std::vector<TSRange> CSTTree::getErrors() {
  std::string q = R"(
      [
         (ERROR)
         (MISSING)
      ] @syntax.error
  )";

  TSQuery *sq = parent.queryNew(q);
  std::vector<TSRange> errors;
  find(sq, [&errors](TSQueryMatch m) mutable {
    for (size_t i = 0; i < m.capture_count; i++) {
      TSNode n = m.captures[i].node;
      TSRange r = {ts_node_start_point(n), ts_node_end_point(n),
                   ts_node_start_byte(n), ts_node_end_byte(n)};
      errors.push_back(r);
    }
  });
  ts_query_delete(sq);
  return errors;
}

// TSEngine

TSEngine::TSEngine(const TSLanguage *lang) {
  this->lang = lang;
  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, lang);
  this->parser = parser;
};

TSEngine::~TSEngine() { ts_parser_delete(this->parser); };

const CSTTree TSEngine::parse(FileReader &reader) {
  TSTree *tree = ts_parser_parse(parser, NULL, reader.asTsInput());
  return CSTTree(tree, reader.get(reader.bufStart, reader.bufSize), *this);
}

const CSTTree TSEngine::parse(FileWriter &writer) {
  auto source = writer.snapshot().cont;
  TSTree *tree =
      ts_parser_parse_string(parser, NULL, source.data(), source.length());
  return CSTTree(tree, source, *this);
}

const CSTTree TSEngine::parse(std::string_view source) {
  TSTree *tree =
      ts_parser_parse_string(parser, NULL, source.data(), source.length());
  return CSTTree(tree, source, *this);
};

const CSTTree TSEngine::parse(const CSTTree &old, std::string_view source) {
  TSTree *tree =
      ts_parser_parse_string(parser, old.tree, source.data(), source.length());
  return CSTTree(tree, source, *this);
};

TSQuery *TSEngine::queryNew(std::string &queryExpr) {
  uint32_t errorOffset;
  TSQueryError error;

  TSQuery *query = ts_query_new(lang, queryExpr.c_str(), queryExpr.length(),
                                &errorOffset, &error);

  assert(error != TSQueryErrorSyntax);
  assert(error != TSQueryErrorNodeType);
  assert(error != TSQueryErrorField);
  assert(error != TSQueryErrorCapture);
  assert(error == TSQueryErrorNone);


  return query;
};

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
    std::call_once(lib_git_init, git_libgit2_init);
}

LibGit::LibGit(git_repository *repo) {
  assert(repo != nullptr);
  init();
  this->repo = make_repo(repo);
  root = git_repository_workdir(repo);
}

LibGit::~LibGit(){}

LibGit::RepoPtr LibGit::make_repo(git_repository *raw){
    return RepoPtr(raw, [](git_repository* r) {
        git_repository_free(r); 
        // this will free when the last instance of LibGit using this is delete
        // also this is thread safe
    });
}

LibGit LibGit::clone(std::string url, std::string path, bool shallow){
  init();
  try{
    return open(path);
  }catch(std::runtime_error e){
    git_repository* repo = nullptr;
    git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
    
    if(shallow){
      opts.fetch_opts.depth = 1;
    }

    auto cloneProgressCb = 
    [](const git_indexer_progress *stats, void *payload) -> int {
      printf("Cloning progrss %d/%d objects\r",
          stats->received_objects,
          stats->total_objects);
      printf("\n");
      return 0;
    };
    opts.fetch_opts.callbacks.transfer_progress = cloneProgressCb;

    if(git_clone(&repo, url.c_str(), path.c_str(), &opts) < 0){
      auto e = git_error_last();
      throw std::runtime_error(std::string("Unable to clone repository at " + url + " due to :") + 
                        ((e && e->message) ? e->message : "Unknown"));
    }
    return LibGit(repo);
  }
}

LibGit LibGit::open(std::string path){
  git_repository* repo = nullptr;
  init();
  if (git_repository_open(&repo, path.c_str()) < 0) {
    const git_error* e = git_error_last();

    throw std::runtime_error(std::string("Unable to open repository at " + path + " due to : ") +
      (e && e->message ? e->message : "Unknown"));
  }

  return LibGit(repo);
}

bool LibGit::isPathIgnored(fs::path path){
  return isPathIgnored(path.string());
}

bool LibGit::isPathIgnored(std::string path){
  int ignored;
  if(git_ignore_path_is_ignored(&ignored, repo.get(), path.c_str()) < 0){
    return false;
  }
  return ignored == 1;
}

void LibGit::add(fs::path &path) {
  git_index *index = nullptr;

  fs::path relPath = fs::relative(path, root);

  std::lock_guard<std::mutex> lock(gitMutex);
  
  git_repository_index(&index, repo.get());
  git_index_add_bypath(index, relPath.c_str());
  git_index_write(index);
  git_index_free(index);
};

void LibGit::addIgnoreRule(std::string rule){
  git_ignore_add_rule(repo.get(), rule.c_str());
}


#endif // LIB_IMPLEMENTATION
