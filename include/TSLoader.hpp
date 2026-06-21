#ifndef TS_LOADER_HPP
#define TS_LOADER_HPP

#include <map>
#include <string>
#include <tree_sitter/api.h>
#include <memory>
#include <mutex>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h> // TODO: this is VERY heavy and polutes the global namespace, 
                       // should be used in IMPL only
  #define LIB_HANDLE HMODULE
#else
  #include <dlfcn.h>
  #define LIB_HANDLE void*
#endif

/*
use prebuilt binaries from release of https://github.com/casualjim/breeze-tree-sitter-parsers
and put into depa/tree-sitter-parsers/tree-sitter-{lang} if make and c compiler not available i.e on windows
*/

#define LIST_OF_PARSERS_PAGE "https://github.com/tree-sitter/tree-sitter/wiki/List-of-parsers"
//#define LIST_OF_PARSER_PATH "../deps/tree-sitter-wiki/List-of-parsers.md"
#define PARSER_PATH "./deps/tree-sitter-parsers"
#define BUILD_CMD "make -j4 -C"


namespace copypasta {

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
    if(!path) GetModuleHandle(NULL); // loaded lib
    return LoadLibraryA(path); // external lib
#else
    if(!path) dlopen(NULL, RTLD_NOW | RTLD_LOCAL); // loaded lib
    return dlopen(path, RTLD_NOW | RTLD_LOCAL); // external lib
#endif
}

static void close_library(LIB_HANDLE lib) {
#ifdef _WIN32
    FreeLibrary(lib);
#else
    dlclose(lib);
#endif
}

class LibHandle {
  private:
  LIB_HANDLE handle;
  public:
  const std::string name;
  LibHandle(LIB_HANDLE handle, std::string name);
  ~LibHandle();
  LIB_HANDLE getRaw(){return handle;};
};

class TSLang{
  private:
  const TSLanguage* lang;
  public:
  const std::string name;
  TSLang():name("") {};
  TSLang(const TSLanguage* lang, const std::string& name);
  ~TSLang();
  const TSLanguage* getRaw(){ return lang; };
};

class TSLangWrapper{
  std::shared_ptr<LibHandle> handle = nullptr;
  std::shared_ptr<TSLang> lang = nullptr;
  public:
  
  TSLangWrapper(LIB_HANDLE handle, const TSLanguage* lang,
         const std::string& name);

  std::shared_ptr<void> getHandle(){ return handle; }
  std::shared_ptr<TSLang> getLang(){ return lang; }

  bool isValid() const {
    return lang.get() != nullptr && lang.get()->getRaw() != nullptr;
  }
};

class TSLoader{
  static std::once_flag initFlag;
  public:
  static std::map<std::string, std::string> lookup;
  static TSLangWrapper loadTSLangFromSelf(std::string lang);
  static TSLangWrapper loadTSLangFromExtern(std::string libPath, std::string lang);
  TSLoader();
  TSLangWrapper get(std::string lang);
};

} // namespace copypasta

#endif // TS_LOADER_HPP
