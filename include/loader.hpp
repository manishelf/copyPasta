
#ifndef LOADER_
#define LOADER_


#include <memory>
#include <stdexcept>
#include <map>
#include <string>
#include <tree_sitter/api.h>
#include <lib.hpp>
#include <ListOfParsers.hpp>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

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

// API
#define LIST_OF_PARSERS_PAGE "https://github.com/tree-sitter/tree-sitter/wiki/List-of-parsers"
//#define LIST_OF_PARSER_PATH "../deps/tree-sitter-wiki/List-of-parsers.md"
#define PARSER_PATH "./deps/tree-sitter-parsers"
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
  TSLang(const TSLanguage* lang, const std::string& name);
  ~TSLang();
  const TSLanguage* getRaw(){return lang;};
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

#endif // LOADER_


// IMPL

#ifndef LOADER_IMPLEMENTATION 
#define LOADER_IMPLEMENTATION

LibHandle::LibHandle(LIB_HANDLE handle, std::string name): name(name){
  this->handle = handle;
};

LibHandle::~LibHandle(){
  if(handle){
    DEBUG_FULL("LibHandle Unloaded external lib " << name);
    close_library(handle);
  }
}

TSLang::TSLang(const TSLanguage* lang, const std::string& name): name(name){
  this->lang = lang;
}

TSLang::~TSLang(){
  if(lang){
    DEBUG_FULL("TSLang deleted lang");
    ts_language_delete(lang);
  }
}

TSLangWrapper::TSLangWrapper(LIB_HANDLE handle, const TSLanguage* lang, 
                const std::string& name){
  this->handle = std::make_shared<LibHandle>(handle, name);
  this->lang = std::make_shared<TSLang>(lang, name);
}

std::once_flag TSLoader::initFlag; // needs a global instance to track init
std::map<std::string, std::string> TSLoader::lookup;

TSLoader::TSLoader(){
   std::call_once(initFlag, [](){
    //FileReader reader(LIST_OF_PARSER_PATH);
    FileSnapshot s;
    s.cont = listOfParsers;
    FileReader reader(s);
    std::string pattern = 
      R"(\|\s*([^\|\s]+)\s*\|\s*\[[^\]]+\]\((https:\/\/[^\)]+)\)\s*\|)";
    DEBUG("loader init");
    auto matches = reader.find(pattern, true, PCRE2_MULTILINE);
    for(const auto& match : matches){
      assert(match.captures.size() == 2);
      auto langP = match.captures[0];
      auto urlP = match.captures[1];
      std::string langName = std::string(reader.get(langP.start_byte, langP.end_byte));
      std::string gitUrl = std::string(reader.get(urlP.start_byte, urlP.end_byte));
      lookup[langName] = gitUrl;
      DEBUG("Loader lib available - " << langName << " from "+gitUrl);
    }
  });
  DEBUG("loader init done");
}

TSLangWrapper TSLoader::loadTSLangFromSelf(std::string lang){
    std::string symbol = "tree_sitter_" + lang;
    
    //already Loaded or statically included
    LIB_HANDLE handle =  load_library(NULL); 
    TSLanguageFn fn = nullptr;

    if(handle){
      fn = (TSLanguageFn)load_symbol(handle, symbol.c_str());
    }

    if(fn){
      const TSLanguage* tsLang = fn();
      return TSLangWrapper(nullptr, tsLang, lang);
    }

    return TSLangWrapper(nullptr, nullptr, lang);
}

TSLangWrapper TSLoader::loadTSLangFromExtern(std::string libPath, std::string lang)
{
    std::string symbol = "tree_sitter_" + lang;

    // load external lib  
    LIB_HANDLE handle = load_library(libPath.c_str());
    if (!handle) {
      throw std::runtime_error("unable to load library at "+ libPath);
    }

    TSLanguageFn fn = (TSLanguageFn)load_symbol(handle, symbol.c_str());
    if (!fn) {
        close_library(handle);
        throw std::runtime_error("failed to load symbol "+ symbol);
    }

    const TSLanguage* language = fn();

    return TSLangWrapper(handle, language, lang);
}


TSLangWrapper TSLoader::get(std::string lang){

  std::string repoPath = std::string(PARSER_PATH)+"/tree-sitter-"+lang;

  DEBUG("Loader get - " << lang);
  TSLangWrapper tsLang  = loadTSLangFromSelf(lang);

  if(tsLang.isValid()){
    return tsLang;
  }

  DirWalker walker(repoPath);
  // TODO: do introspection incase of static linking 
  std::set<std::string> libExt = {".so", ".dll", ".dylib"};
  walker.matchExt = libExt;
  walker.recursive = true;
  walker.obeyGitIgnore = false;

  auto action =[&libExt, &tsLang, lang](DirWalker::STATUS status, File file){

      DEBUG_FULL("Loader checking - "+file.pathStr);

      bool match = false;
      for(auto ext : libExt){
        if(file.name.find("libtree-sitter-"+lang+ext) != std::string::npos) {
          match = true;
          break;
        }
      }

      if(!match) {
        return DirWalker::CONTINUE;
      }

      INFO("Found library - " << file.pathStr);
      tsLang = loadTSLangFromExtern(file.pathStr, lang);

      return DirWalker::STOP;
  }; 

  try{
    walker.walk(action);
    if(tsLang.isValid()){
      return tsLang;
    }
  }catch(std::runtime_error e){
    std::string gitUrl = lookup[lang];
    try{ 
      LibGit::clone(gitUrl, repoPath, true);
    }catch(std::runtime_error e){
      LERROR(e.what());
    }
    // TODO: make this cross platform and portable
    // or maybe make is fine?
    std::string compileCommand =  (BUILD_CMD + repoPath);
    INFO("Building lib with - " << compileCommand);
    // TODO: maybe do this differetly as there may be code execution from the md file
    int status = std::system(compileCommand.c_str());

    if (status != 0) {
      LERROR("Unable to compile parser - " + compileCommand);
    }

    walker.walk(action);

    if(!(tsLang.isValid())){
      throw std::runtime_error("unable to load - "+lang);
    }

    INFO("Loaded TS Parser - " << lang);
    DEBUG("abi - " << ts_language_abi_version(tsLang.getLang()->getRaw()));
    auto name = ts_language_name(tsLang.getLang()->getRaw());
    if(name){
      DEBUG("name - "+std::string(name));
    }else{
      DEBUG("lib may have abi < LANGUAGE_VERSION_WITH_RESERVED_WORDS");
    }
    return tsLang; 

  }
  throw std::runtime_error("Unable to load TS parser - " + lang + " from " + walker.path);
}

#endif // LOADER_IMPLEMENTATION
