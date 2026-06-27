#ifndef FILE_READER_WRITER_HPP
#define FILE_READER_WRITER_HPP

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <tree_sitter/api.h>

namespace copypasta {

namespace fs = std::filesystem;

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
  std::vector<size_t> rowOffsets;
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
  bool writeTo(const std::string &path); // create file if non existing , will over write existing

  FileWriter &copy(std::string &path); // load file cont to buf

  // overwrite
  FileWriter &write(const std::string &content); // replace entire buf content
  FileWriter &write(size_t offset, char *newCont, size_t newContLen);
  FileWriter &write(size_t offset, std::string &cont);
  FileWriter &write(size_t from, size_t to, std::string &cont);

  FileWriter &append(const std::string &cont);
  FileWriter &insert(size_t offset, const std::string &newCont);
  FileWriter &insertRowBefore(size_t row, const std::string &line, bool preserveIndent = false);
  FileWriter &insertRowAfter(size_t row, const std::string &line, bool preserveIndent = false);
  FileWriter &deleteRow(size_t row);
  FileWriter &deleteCont(size_t from, size_t to);

  FileWriter &replace(const std::string& pattern, 
                      const std::string& templateOrResult,
                      size_t nth_occ = 0, // 0 for first 1 for second and
                                          //-1 for last, -2 for last second and so on
                      uint32_t opt = PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED);
  FileWriter &replaceAll(
                      const std::string& pattern,
                      const std::string& templateOrResult,
                      uint32_t opt = PCRE2_SUBSTITUTE_GLOBAL |
                                      PCRE2_SUBSTITUTE_EXTENDED);
};

} // namespace copypasta

#endif // _FILE_READER_WRITER
