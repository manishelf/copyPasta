
#include <stdexcept>
#include <map>
#include <string>
#include <tree_sitter/api.h>
#include <lib.hpp>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#ifndef LOADER_
#define LOADER_

#ifdef _WIN32
  #include <windows.h>
  #define LIB_HANDLE HMODULE
#else
  #include <dlfcn.h>
  #define LIB_HANDLE void*
#endif

/*
use prebuilt binaries from release of https://github.com/casualjim/breeze-tree-sitter-parsers
and put into depa/tree-sitter-parsers/tree-sitter-{lang} if make and c compiler not available i.e on windows
*/

// API
#define LIST_OF_PARSERS_PAGE "https://github.com/tree-sitter/tree-sitter/wiki/List-of-parsers"
#define LIST_OF_PARSER_PATH "../deps/tree-sitter-wiki/List-of-parsers.md"
#define PARSER_PATH "../deps/tree-sitter-parsers"
#define C_COMPILER "gcc"



// function pointer type for language constructor
typedef const TSLanguage *(*TSLanguageFn)(void);

static void *load_symbol(LIB_HANDLE lib, const char *symbol) {
#ifdef _WIN32
    return (void*)GetProcAddress(lib, symbol);
#else
    return dlsym(lib, symbol);
#endif
}

static LIB_HANDLE load_library(const char *path) {
#ifdef _WIN32
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void close_library(LIB_HANDLE lib) {
#ifdef _WIN32
    FreeLibrary(lib);
#else
    dlclose(lib);
#endif
}

class TSLang{
  public:
  LIB_HANDLE handle = nullptr;
  const TSLanguage* lang = nullptr;
  TSLang(LIB_HANDLE handle, const TSLanguage* lang);
  TSLang(){};
  
  TSLang(TSLang&& other) noexcept {
    handle = other.handle;
    lang = other.lang;
    other.handle = nullptr;
    other.lang = nullptr;
  }

  TSLang& operator=(TSLang&& other) noexcept {
    if (this != &other) {
      if (handle) close_library(handle);
      handle = other.handle;
      lang = other.lang;
      other.handle = nullptr;
      other.lang = nullptr;
    }
    return *this;
  }
  
  ~TSLang();
};

class Loader{
  std::map<std::string, std::string> lookup;
  public:
  Loader();
  TSLang get(std::string lang);
  static TSLang loadTSLang(std::string libPath, std::string lang);
};

#endif // LOADER_


// IMPL

#ifndef LOADER_IMPLEMENTATION 
#define LOADER_IMPLEMENTATION

TSLang::TSLang(LIB_HANDLE handle, const TSLanguage* lang){ 
  this->handle = handle;
  this->lang = lang;
}

TSLang::~TSLang() {
  if (handle) {
    close_library(handle);
  }
}

Loader::Loader(){
  FileReader reader(LIST_OF_PARSER_PATH);
  std::string pattern = 
      R"(\|\s*([a-zA-Z]+)\s*\|\s*\[[a-zA-Z\.\/-]+\]\((https:\/\/[a-zA-Z0-9\:\/\.\-]+)\)\s*\|)";
  auto matches = reader.find(pattern, true, PCRE2_MULTILINE);
  for(auto match : matches){
    assert(match.captures.size() == 2);
    auto langP = match.captures[0];
    auto urlP = match.captures[1];
    std::string langName = std::string(reader.get(langP.start_byte, langP.end_byte));
    std::string gitUrl = std::string(reader.get(urlP.start_byte, urlP.end_byte));
    lookup[langName] = gitUrl;
  }
}

TSLang Loader::loadTSLang(std::string libPath, std::string lang)
{
    LIB_HANDLE handle;

    handle = load_library(libPath.c_str());
    if (!handle) {
      throw std::runtime_error("unable to load library at "+ libPath);
    }

    std::string symbol = "tree_sitter_" + lang;

    TSLanguageFn fn = (TSLanguageFn)load_symbol(handle, symbol.c_str());
    if (!fn) {
        close_library(handle);
        throw std::runtime_error("failed to load symbol "+ symbol);
    }

    const TSLanguage* language = fn();

    return TSLang(handle, language);
}


TSLang Loader::get(std::string lang){
  std::string gitUrl = lookup[lang];
  
  std::string repoPath = std::string(PARSER_PATH)+"/tree-sitter-"+lang;
  std::cout << "Cloning " << gitUrl << std::endl;
  LibGit::clone(gitUrl, repoPath, true);

  // TODO: make this cross platform and portable
  // or maybe make is fine?
  std::string compileCommand =  ("make -C " + repoPath);
  // TODO: maybe do this differetly as there may be code execution from the md file
  int status = std::system(compileCommand.c_str());

  if (status != 0) {
    throw std::runtime_error("Unable to compile parser - " + compileCommand);
  }

  DirWalker walker(repoPath);
  walker.matchExt.insert(".so");
  walker.matchExt.insert(".dll");
  walker.recursive = true;
  walker.obeyGitIgnore = false;

  TSLang tsLang;
  std::string str;

  walker.walk([&tsLang, lang](DirWalker::STATUS status, File file){

      if(file.name.find("libtree-sitter-"+lang+".so") == std::string::npos) return DirWalker::CONTINUE;

      std::cout << "Found library - " << file.pathStr << std::endl;
      tsLang = loadTSLang(file.pathStr, lang);

      return DirWalker::STOP;
  });

  std::cout << "Loaded TS Parser - " << ts_language_name(tsLang.lang) << std::endl;
  
  return tsLang; 
}

#endif // LOADER_IMPLEMENTATION
