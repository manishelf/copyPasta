#ifndef LIB_H_
#define LIB_H_

#include "git2/types.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <git2.h>
#include <iostream>
#include <iterator>
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
#include <assert.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace fs = std::filesystem;

// ----------------------------------------------------------
// API
// ----------------------------------------------------------

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

class File {
public:
  std::string pathStr;
  std::string name;
  std::string ext;
  bool isDir;
  bool isReg;
  bool isValid;
  size_t size;
  fs::path path;
  fs::file_status status;
  fs::directory_entry dir_entry;

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
  size_t defaultBlockSize;
  bool readReverse;
  bool snapShotMode; // disables fresh load and sync
  FileReader(File file, size_t blockSize = 4096);
  FileReader(std::string filePath, size_t blockSize = 4096);
  FileReader(const FileSnapshot snap, size_t blockSize = 4096);
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
                                size_t opt = PCRE2_CASELESS);
  std::vector<MatchResult> findWith(pcre2_code *re,
                                    size_t opt = PCRE2_CASELESS);
  TSPoint getPointFromByte(size_t byteOffset);

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

class FileWriter {
  File file;
  bool _isValid;
  std::ofstream oFileStream;
  FileSnapshot snap;

public:
  FileWriter(const FileSnapshot snap);
  FileWriter(std::string path);
  ~FileWriter();

  bool isValid() { return _isValid; };
  File getFile() { return file; };
  const FileSnapshot getSnapshot() { return snap; };

  std::vector<size_t> rowOffsets;

  bool commit(); // save buf to underling file
  bool
  backup(const std::string &suffix = ".bak"); // create a backup in same folder
  bool
  flush(std::string &path); // create if non existing , will over write existing

  FileWriter &copy(std::string &path); // load file cont to buf
  FileWriter &append(std::string &cont);
  FileWriter &insert(size_t offset, std::string &newCont);

  // overwrite
  FileWriter &write(const std::string &content); // replace entire buf content
  FileWriter &write(size_t offset, char *newCont, size_t newContLen);
  FileWriter &write(size_t offset, std::string &cont);

  FileWriter &deleteRow(size_t row);
  FileWriter &insertRow(size_t row, const std::string &line);
  FileWriter &deleteCont(size_t from, size_t to);
  FileWriter &replaceAll(std::string pattern, std::string templateOrResult,
                         size_t opt = PCRE2_SUBSTITUTE_GLOBAL |
                                      PCRE2_SUBSTITUTE_EXTENDED);
  FileWriter &
  replace(std::string pattern, std::string templateOrResult,
          size_t nth_occ = 0, // 0 for first 1 for second and
                              //-1 for last, -2 for last second and so on
          size_t opt = PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED);
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
  std::set<std::string> ignoring;
  enum STATUS {
    QUEUING, // file queued for processing; may be skipped based on action
             // result
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

  bool isValid() { return _isValid; }

  std::vector<File> allChildren();

  ~DirWalker();

  using WalkAction_t = std::function<ACTION(STATUS, File, void *payload)>;

  STATUS walk(WalkAction_t action, void *payload = nullptr);

  void walk(ThreadPool &pool, WalkAction_t action, void *payload = nullptr);

private:
  STATUS walk(git_repository* repo, WalkAction_t action, void *payload = nullptr);

  void walk(git_repository* repo, ThreadPool &pool, WalkAction_t action,
            std::shared_ptr<std::atomic<bool>> globalAbort, void *payload);
};

class TSEngine;

class CSTTree {
private:
  TSTree *tree;
  std::string_view source;

public:
  CSTTree(TSTree *tree, std::string_view source);
  ~CSTTree();

  std::string sTree();
  std::string asQuery();
  void getQueryForNode(TSNode node, std::string &query, size_t level = 0);

   std::vector<TSQueryMatch> find(TSQuery* query);
};

class TSEngine {
  const TSLanguage *lang;
  TSParser *parser;

public:
  TSEngine(const TSLanguage *lang);
  ~TSEngine();
  const CSTTree parse(std::string_view source);
  const CSTTree parse(FileReader &reader);
  TSQuery* queryNew(std::string& queryExpr); // free with ts_query_delete
  static TSQuery* queryNew(const TSLanguage* lang, std::string& queryExpr); // free with ts_query_delete
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
  defaultBlockSize = blockSize;
  _isValid = !file.isDir;
  readFileMetadata();
  snapShotMode = false;
};

FileReader::FileReader(std::string filePath, size_t blockSize)
    : iFileStream(filePath.c_str(), std::ios::binary | std::ios::ate) {
  this->file = File(filePath);
  if (file.isValid) {
    _isValid = true && !file.isDir;
    defaultBlockSize = blockSize;
    readFileMetadata();
  } else {
    defaultBlockSize = 0;
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
  std::memcpy(buf, snap.cont.data(), snap.cont.length());
  _isValid = true;
  defaultBlockSize = blockSize;
};

FileReader::FileReader(const FileReader &copy) {
  this->iFileStream = std::ifstream(copy.file.pathStr);
  this->file = copy.file;
  this->_isValid = copy._isValid;
  this->pos = copy.pos;
  this->buf = new char[copy.bufSize];
  memcpy(this->buf, copy.buf, copy.bufSize);
  this->level = copy.level;
  this->rowOffsets = copy.rowOffsets;
  this->bufStart = copy.bufStart;
  this->bufSize = copy.bufSize;
  this->defaultBlockSize = copy.defaultBlockSize;
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

    buf = new char[file.size];
    iFileStream.read(buf, file.size);
    iFileStream.clear();

    UPDATE_ROW_OFFSETS(buf, file.size);
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

  size_t size = std::min(defaultBlockSize, file.size - pos);

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
      std::min(reader->defaultBlockSize, reader->file.size - byte_index);

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
                                                      bool regex, size_t opt) {

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
      throw std::invalid_argument(
          "could not compile provided regex for fn find " + pattern + " - " +
          file.pathStr);
    }

    pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);

    if (buf == nullptr)
      if (sync().cont == nullptr)
        return matches;

    matches = findWith(re, opt);

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
      range.start_point = getPointFromByte(matchStart);
      range.end_point = getPointFromByte(matchEnd);
      MatchResult match;
      match.match = range;
      matches.push_back(match);

      offset = matchEnd;
    }
    return matches;
  }
};

std::vector<FileReader::MatchResult> FileReader::findWith(pcre2_code *re,
                                                          size_t opt) {

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
      pcre2_match_data_free(match_data);
      throw std::runtime_error("PCRE2 match error");
    }
    ovector = pcre2_get_ovector_pointer(match_data);

    MatchResult match;
    TSRange range;

    range.start_byte = static_cast<uint32_t>(ovector[0]);
    range.end_byte = static_cast<uint32_t>(ovector[1]);

    range.start_point = getPointFromByte(range.start_byte);
    range.end_point = getPointFromByte(range.end_byte);

    match.match = range;

    for (int i = 1; i < rc; i++) {
      PCRE2_SIZE start = ovector[2 * i];
      PCRE2_SIZE end = ovector[2 * i + 1];

      if (start == PCRE2_UNSET || end == PCRE2_UNSET)
        continue;

      TSRange capture;
      capture.start_byte = static_cast<uint32_t>(start);
      capture.end_byte = static_cast<uint32_t>(end);
      capture.start_point = getPointFromByte(start);
      capture.end_point = getPointFromByte(end);
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

TSPoint FileReader::getPointFromByte(size_t byteOffset) {

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

const FileSnapshot FileReader::snapshot() {

  if (!file.isValid)
    return FileSnapshot();

  file.sync();
  sync();

  FileSnapshot snap = {};
  snap.file = file;
  snap.cont = std::string(buf, file.size);
  fs::file_time_type mtim = file.dir_entry.last_write_time();
  snap.lastModified = mtim.time_since_epoch().count();
  snap.dirty = false;
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

  size_t currentBlockSize = std::min(defaultBlockSize, pos);
  ;

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
};

FileWriter::FileWriter(std::string path) {
  FileReader tmp(path);
  snap = tmp.snapshot();
  file = tmp.getFile();
  rowOffsets = tmp.rowOffsets;
  _isValid = file.isValid;
};

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

bool FileWriter::commit() {
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
  snap.cont.insert(offset, slice);
  modifySnap
};

FileWriter &FileWriter::write(const std::string &content) {
  snap.cont = std::string(content);
  modifySnap
}

FileWriter &FileWriter::write(size_t offset, char *newCont, size_t newContLen) {
  snap.cont.erase(offset, newContLen);
  snap.cont.insert(offset, newCont, newContLen);
  modifySnap
};

FileWriter &FileWriter::write(size_t offset, std::string &cont) {
  snap.cont.erase(offset, cont.length());
  snap.cont.insert(offset, cont);
  modifySnap
};

FileWriter &FileWriter::deleteCont(size_t from, size_t to) {
  snap.cont.erase(from, to - from);
  modifySnap
};

FileWriter &FileWriter::deleteRow(size_t row) {
  auto it1 = std::upper_bound(rowOffsets.begin(), rowOffsets.end(), row);
  size_t row1Offset = std::distance(rowOffsets.begin(), it1) - 1;

  auto it2 = std::upper_bound(rowOffsets.begin(), rowOffsets.end(), row + 1);
  size_t row2Offset = std::distance(rowOffsets.begin(), it2) - 1;

  snap.cont.erase(row1Offset, row2Offset);
  modifySnap
};

FileWriter &FileWriter::insertRow(size_t row, const std::string &cont) {
  bool hasEndl = cont[cont.length() - 1] == '\n';
  auto it = std::upper_bound(rowOffsets.begin(), rowOffsets.end(), row);
  size_t rowOffset = std::distance(rowOffsets.begin(), it) - 1;

  snap.cont.insert(rowOffset, cont);
  if (!hasEndl)
    snap.cont.insert(rowOffset + cont.length(), '\n', 1);
  modifySnap
};

FileWriter &FileWriter::replaceAll(std::string pattern,
                                   std::string templateOrResult, size_t opt) {

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
                                size_t opt) {
  FileReader snapReader(snap);
  auto results = snapReader.find(pattern, true);

  if (results.empty()) {
    return *this;
  }

  // (a%b + b)%b for -10 % 100 = 90 && 10 % 100 = 10
  nth = (nth % results.size() + results.size()) % results.size();
  auto target = results[nth];

  size_t start_offset = target.match.start_byte;
  size_t end_offset = target.match.end_byte;

  int errornumber;
  PCRE2_SIZE erroroffset;

  pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern.c_str(), pattern.length(),
                                 0, &errornumber, &erroroffset, nullptr);

  if (!re) {
    throw std::runtime_error("PCRE2 compilation failed at offset " +
                             std::to_string(erroroffset));
  }

  pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);

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

// DirWalker
DirWalker::DirWalker(std::string dir) {
  path = dir;
  _isValid = fs::exists(dir);
};

DirWalker::~DirWalker() {};

DirWalker::STATUS DirWalker::walk(WalkAction_t action, void *payload){
  git_repository *repo = NULL;

  if (git_repository_open_ext(&repo, path.c_str(),
                              GIT_REPOSITORY_OPEN_NO_SEARCH,
                              NULL) != GIT_ENOTFOUND) {
    std::cout << "found repository in path " << path << std::endl;
  }
  auto res = walk(repo, action, payload);
  git_repository_free(repo);
  return res;
};

DirWalker::STATUS DirWalker::walk(git_repository* repo, WalkAction_t action, void *payload) {

  if (!_isValid)
    return FAILED;

  std::vector<fs::directory_entry> entries(
      fs::directory_iterator(this->path), // begin it
      fs::directory_iterator()            // end it
  );

  git_repository* child_repo = nullptr;
  git_submodule* sub = nullptr;

  for (size_t i = 0; i < entries.size(); i++) {
    File file(entries[i]);

    if (obeyGitIgnore && repo) {
      int ignored;
      git_ignore_path_is_ignored(&ignored, repo, file.path.c_str());
      if (ignored) {
        continue;
      }
    }

    ACTION actRes = action(STATUS::OPENED, file, payload);

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
      child.recursive = recursive;
      child.obeyGitIgnore = obeyGitIgnore;
      child.inverted = inverted;
      STATUS res = child.walk(repo, action, payload);

      if (res == STATUS::ABORTED) {
        return res;
      } else if (res == STATUS::FAILED) {
        actRes = action(STATUS::FAILED, file, payload);
      }
    }

    if (inverted && i == entries.size() - 1) {
      fs::path parent = fs::absolute(path).parent_path();
      if (path == ".")
        parent = parent.parent_path();
      if (parent != "/") {
        DirWalker child(parent.string());
        child.level = level - 1;
        child.recursive = false;
        child.obeyGitIgnore = obeyGitIgnore;
        child.inverted = true;

        return child.walk(repo, action, payload);
      }
    }
  }

  return STATUS::DONE;
};

void DirWalker::walk(ThreadPool &pool, WalkAction_t action, void *payload) {
  std::shared_ptr<std::atomic<bool>> abortSignal =
      std::make_shared<std::atomic<bool>>(false);
  git_repository *repo = NULL;

  if (git_repository_open_ext(&repo, path.c_str(),
                              GIT_REPOSITORY_OPEN_NO_SEARCH,
                              NULL) != GIT_ENOTFOUND) {
    std::cout << "found repository in path " << path << std::endl;
  }
  walk(repo, pool, action, abortSignal, payload);
  git_repository_free(repo);
}

void DirWalker::walk(git_repository* repo, ThreadPool &pool, WalkAction_t action,
                     std::shared_ptr<std::atomic<bool>> abortSignal,
                     void *payload) {
  

  std::vector<fs::directory_entry> entries(fs::directory_iterator(this->path),
                                           fs::directory_iterator());

  for (int i = 0; i < entries.size(); i++) {
    File file(entries[i]);
    // If any thread previously returned ABORT, quit now
    if (abortSignal->load()) {
      return;
    }

    if (obeyGitIgnore && repo) {
      int ignored;
      git_ignore_path_is_ignored(&ignored, repo, file.path.c_str());
      if (ignored) {
        continue;
      }
    }

    ACTION actRes = action(STATUS::QUEUING, file, payload);

    if (actRes == ACTION::STOP) {
      return;
    }
    if (actRes == ACTION::SKIP)
      continue;
    if (actRes == ACTION::ABORT) {
      abortSignal->store(true);
      return;
    }

    if (!((file.name == ".") || (file.name == "..")) && file.isDir &&
        recursive && !inverted) {
      DirWalker child(file.path);
      child.recursive = recursive;
      child.obeyGitIgnore = obeyGitIgnore;
      child.inverted = inverted;

      child.walk(repo, pool, action, abortSignal, payload);
    } else if (inverted && i == entries.size() - 1) {
      fs::path parent = fs::absolute(path).parent_path();
      if (path == ".")
        parent = parent.parent_path();
      if (parent != "/") {
        DirWalker child(parent.string());
        child.level = level - 1;
        child.recursive = false;
        child.obeyGitIgnore = obeyGitIgnore;
        child.inverted = true;

        child.walk(repo, pool, action, abortSignal, payload);
      }
    } else {

      // create a anonlymous class that has action and file in constructor
      pool.enqueue([action, file, abortSignal, payload]() {
        if (abortSignal->load())
          return;

        ACTION result = action(STATUS::OPENED, file, payload);

        if (result == ACTION::ABORT) {
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

CSTTree::CSTTree(TSTree *tree, std::string_view source) : source(source) {
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
  // query.append(std::string(level, '\t'));
  query.append("(");
  query.append(ts_node_type(node));

  uint32_t count = ts_node_child_count(node);

  for (size_t i = 0; i < count; ++i) {
    TSNode child = ts_node_child(node, i);
    if (!ts_node_is_named(child))
      continue;
    // query.append("\n");
    getQueryForNode(child, query, level + 1);
    // query.append(std::string(level, '\t'));
  }

  query.append(")");
  // query.append("@");
  // query.append(ts_node_type(node));
  // query.append("_"+std::to_string(level));
  // query.append("\n");
};

std::string CSTTree::asQuery() {
  std::string query;
  TSNode node = ts_tree_root_node(tree);
  getQueryForNode(node, query);
  return query;
};

std::vector<TSQueryMatch> CSTTree::find(TSQuery* query) {
  std::vector<TSQueryMatch> results;
  TSNode root = ts_tree_root_node(tree);
  TSQueryCursor *cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query, root);
  TSQueryMatch match;

  while (ts_query_cursor_next_match(cursor, &match)) {
    results.push_back(match);
  }

  ts_query_cursor_delete(cursor);
  return results;
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
  return CSTTree(tree, reader.get(reader.bufStart, reader.bufSize));
}

const CSTTree TSEngine::parse(std::string_view source) {
  TSTree *tree =
      ts_parser_parse_string(parser, NULL, source.data(), source.length());
  return CSTTree(tree, source);
};

TSQuery* TSEngine::queryNew(std::string& queryExpr){ 
  return TSEngine::queryNew(lang, queryExpr); 
};

TSQuery* TSEngine::queryNew(const TSLanguage* lang, std::string& queryExpr){
  uint32_t errorOffset;
  TSQueryError error;

  TSQuery *query = ts_query_new(lang, queryExpr.c_str(),
                                queryExpr.length(), &errorOffset, &error);

  assert(error != TSQueryErrorSyntax);
  assert(error != TSQueryErrorNodeType);
  assert(error != TSQueryErrorField);
  assert(error != TSQueryErrorCapture);
  assert(error == TSQueryErrorNone);

 return query; 
};

#endif // LIB_IMPLEMENTATION
