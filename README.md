# copyPasta

# program the refactoring logic instead of copy+pasting 1000 times

A **single header stb style library** to allow **large-scale semantic aware refactoring of code for error free transformation**
with the goal of having `minimal manual work` in bulk refactoring projects like `version upgrade / migrations` that require `complex and context aware code changes`.

Changes you make can be **error prone** and require extra separate step of **third party verification**.

**copyPasta allows you to**:

* Program the logic for the transformation you want, test the logic with **dry runs**
* find accurate edit locations accross the codebase using `tree-sitter queries and regex`
* perform valid edit operations like `write`,`append`,`insert`,`replace`,`delete`
* validate the changes in dry run checking the **CST** - concrete syntax tree for errors 
* Have **safe, verifiable, repeatable refactoring** with **git** as VCS

> This project was my way of implementing a refactoring engine to help with hibernate upgrade from 4.5 to 6.4 which involved code changes across 5804 java files
> The migrations guides [1](https://docs.hibernate.org/orm/6.0/migration-guide/) [2](https://github.com/quarkusio/quarkus/wiki/Migration-Guide-3.0:-Hibernate-ORM-5-to-6-migration)  already had the list of changes needed to be made but some changes are not straight forward global replace but require context aware change
> **Examples of context-aware changes:**
>
> ```java
> Query old = session.createQuery("from MyEntity m");
> Query<MyEntity> new = session.createQuery("from MyEntity m", MyEntity.class);
> ```
> **Examples of simple changes:**
> * package namespace changes to jakarta
> ```java
> setString(...), setInteger(...), etc. → setParameter(...)
> query.list() → query.getResultList()
> query.iterator() → query.getResultList().iterator()
> ```
> **Examples of major changes:**
> * Migrating from **Criterion API**
> * Using **tupleTransformers** in place of **resultTransformers**

---

## How it Works

* Using **Tree-sitter parsers** to parse code files and generate tree
* find the position in the file where to apply the edit using tree sitter queries or using regex 
* Perform **edit operations** and check for errors in the **CST tree** generated using Tree-sitter
* Complete **regex support** with **PCRE2**

---

## File Processing

* Walk a folder with **DirWalker** in **single-threaded order**
* The walk can be **aborted**, **skipped**, or **stopped** by the job
* Perform **refactoring jobs per file**
* Use **ThreadPool** when processing to **increase performance** when the files are mix of **many small and large sizes**

---

## Git Integration

* When an **error-free job** is processed, the file can be **added to stage programmatically in git**
* Files with **CST having errors** remain **unstaged**



### build
```bash
git submodule update --init
mkdir build
cd build
cmake ..
make
```

## Workflow

```
Parse → Edit → Validate → Save → Stage
```

```
DirWalker
     ↓
FileWriter, FileReader
     ↓
TSEngine →  TSQuery
     ↓
CSTTree
     ↓
FileEditor (queue edits: INSERT, SAVE, VALIDATE_CST)
     ↓
Apply edits → check errors
     ↓
if no errors → LibGit.add
     ↓
compile and verify
```
---

## Usage Example

```cpp
ThreadPool pool;
DirWalker walker("./src");
walker.recursive = true;

walker.walk(pool, [&](DirWalker::STATUS status, File file) {
    if (status != DirWalker::OPENED || !file.isReg)
        return DirWalker::CONTINUE;

    FileReader reader(file);
    TSEngine engine(tree_sitter_cpp());
    auto tree = engine.parse(reader);
    LibGit git(file.repo);

    FileWriter writer(file);
    FileEditor editor;

    // queue edits
    editor.queue(...)

    auto errors = editor.apply(tree, writer);

    if (errors.empty()) {
        writer.save();
        git.add(file.path);
    }

    return DirWalker::CONTINUE;
});

pool.waitUntilFinished();
```
#### checkout the /examples for more 

---

## Goals
* **Refactoring in large scale codebase**
* **Safe transformations with dry run and semantic validation**
* **Semantic correctness**: Prevent staging of files with CST errors.
* **High performance**: ThreadPool support for parallel file processing.
* **Language-agnostic**: Work with any language supported by Tree-sitter.
## Dependencies

* [**libgit2**](https://libgit2.org/)
* [**PCRE2**](https://www.pcre.org/) 
* [**Tree-sitter**](https://tree-sitter.github.io/tree-sitter/) 
* [**Tree-stter-parser](https://github.com/tree-sitter)
---
### API
```c++
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
                                size_t opt = PCRE2_CASELESS);
  std::vector<MatchResult> findWith(pcre2_code *re,
                                    size_t opt = PCRE2_CASELESS);
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

  FileWriter &append(std::string &cont);
  FileWriter &insert(size_t offset, std::string &newCont);
  FileWriter &insertRow(size_t row, const std::string &line);
  FileWriter &deleteRow(size_t row);
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

// order based on increasing priority
#define FOREACH_OP(OP)                                                         \
  OP(PRINT_PATH)                                                               \
  OP(VALIDATE_CST)                                                             \
  OP(FLUSH)                                                                    \
  OP(SAVE)                                                                     \
  OP(PRINT_CHANGE)                                                             \
  OP(MARK)                                                                     \
  OP(WRITE)                                                                    \
  OP(INSERT)                                                                   \
  OP(REPLACE)                                                                  \
  OP(DELETE)

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
    // [s, e)
    size_t range[2] = {0, 0};
    std::string change[2];
    pcre2_code *rc;
  };
  struct Error {
    ERROR e;
    TSRange range;
    Edit edit;
  };
  FileEditor();
  void queue(Edit e);
  void reset();
  std::vector<Error> apply(CSTTree &original, FileWriter &writer);

private:
  std::vector<Edit> operations;
  std::vector<Error> errors;
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

  // using WalkAction_t = std::function<ACTION(STATUS, File, void *payload)>;

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
  STATUS walk(git_repository *repo, Action &&action, Payload &payload = NULL);

  template <typename Payload, typename Action>
  void walk(git_repository *repo, ThreadPool &pool, Action &&action,
            std::shared_ptr<std::atomic<bool>> globalAbort, Payload &payload);
};

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

  template <typename cb> void find(TSQuery *query, cb handle);

  bool validate(TSInputEdit edit, size_t insertL = 0, size_t delL = 0);
  void edit(TSInputEdit edit, std::string_view &source);

  std::vector<TSRange> getErrors();
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

  // TSQuery is not thread safe although it is immutable
  // unpredictable cursors
  // use thread_local
  TSQuery *queryNew(std::string &queryExpr);
};

class LibGit {
  git_repository *repo;
  std::string root;
  std::mutex gitMutex;

public:
  LibGit(git_repository *repo);

  void add(fs::path &path);
};



```

