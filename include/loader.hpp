
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
#define BUILD_CMD "make -j4 -C"



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
  const std::string name;
  TSLang(LIB_HANDLE handle, const TSLanguage* lang, std::string name);
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

TSLang::TSLang(LIB_HANDLE handle, const TSLanguage* lang, const std::string name): name(name){ 
  this->handle = handle;
  this->lang = lang;
}

TSLang::~TSLang() {
  if (handle) {
    DEBUG("unloaded ts lib " << name);
    close_library(handle);
  }
}

Loader::Loader(){
  FileReader reader(LIST_OF_PARSER_PATH);
  std::string pattern = 
      R"(\|\s*([a-zA-Z]+)\s*\|\s*\[[a-zA-Z\.\/-]+\]\((https:\/\/[a-zA-Z0-9\:\/\.\-]+)\)\s*\|)";
  DEBUG("loader init");
  auto matches = reader.find(pattern, true, PCRE2_MULTILINE);
  for(auto match : matches){
    assert(match.captures.size() == 2);
    auto langP = match.captures[0];
    auto urlP = match.captures[1];
    std::string langName = std::string(reader.get(langP.start_byte, langP.end_byte));
    std::string gitUrl = std::string(reader.get(urlP.start_byte, urlP.end_byte));
    lookup[langName] = gitUrl;
    DEBUG("Loader lib available - " << langName << " from "+gitUrl);
  }
  DEBUG("loader init done");
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

    return TSLang(handle, language, lang);
}


TSLang Loader::get(std::string lang){

  std::string repoPath = std::string(PARSER_PATH)+"/tree-sitter-"+lang;

  DEBUG("Loader get - " << lang);
  TSLang tsLang;
  DirWalker walker(repoPath);
  // TODO: different ext for mac, also maybe do introspection incase of static linking 
  std::set<std::string> libExt = {".so", ".dll", ".dylib"};
  walker.matchExt = libExt;
  walker.recursive = true;
  walker.obeyGitIgnore = false;

  auto action =[&libExt, &tsLang, lang](DirWalker::STATUS status, File file){

      DEBUG("Loader checking - "+file.pathStr);

      bool match = false;
      for(auto ext : libExt){
        if(file.name.find("libtree-sitter-"+lang+ext) != std::string::npos) {
          match = true;
        }
      }

      if(!match) {
        return DirWalker::CONTINUE;
      }

      INFO("Found library - " << file.pathStr);
      tsLang = loadTSLang(file.pathStr, lang);

      return DirWalker::STOP;
  }; 

  walker.walk(action);

  if(!(tsLang.lang && tsLang.handle)){
    std::string gitUrl = lookup[lang];
    
    INFO("Cloning " << gitUrl);
    LibGit::clone(gitUrl, repoPath, true);

    // TODO: make this cross platform and portable
    // or maybe make is fine?
    std::string compileCommand =  (BUILD_CMD + repoPath);
    INFO("Building lib with - " << compileCommand);
    // TODO: maybe do this differetly as there may be code execution from the md file
    int status = std::system(compileCommand.c_str());

    if (status != 0) {
      throw std::runtime_error("Unable to compile parser - " + compileCommand);
    }

    walker.walk(action);
  }

  if(!(tsLang.lang && tsLang.handle)){
    throw std::runtime_error("unable to load - "+lang);
  }

  INFO("Loaded TS Parser - " << lang);
  DEBUG("abi - " << ts_language_abi_version(tsLang.lang));
  auto name = ts_language_name(tsLang.lang);
  if(name){
    DEBUG("name - "+std::string(name));
  }else{
    DEBUG("lib may have abi < LANGUAGE_VERSION_WITH_RESERVED_WORDS");
  }
  
  return tsLang; 
}

#endif // LOADER_IMPLEMENTATION
