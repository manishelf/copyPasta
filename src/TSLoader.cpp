#include <TSLoader.hpp>

#include <map>
#include <string>
#include <tree_sitter/api.h>
#include <assert.h>

#include <FileReaderWriter.hpp>
#include <LibGit.hpp>
#include <DirWalker.hpp>
#include <Logger.hpp>
#include <ListOfParsers.hpp>

namespace copypasta {

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

  walker.walk(action);

  if (!tsLang.isValid()) {

      std::string gitUrl = lookup[lang];
      try {
          LibGit::clone(gitUrl, repoPath, true);
      }
      catch (std::runtime_error e) {
          LERROR(e.what());
      }
      // TODO: make this cross platform and portable
      // or maybe make is fine?
      std::string compileCommand = (BUILD_CMD + repoPath);
      INFO("Building lib with - " << compileCommand);
      // TODO: maybe do this differetly as there may be code execution from the md file
      int status = std::system(compileCommand.c_str());

      if (status != 0) {
          LERROR("Unable to compile parser - " + compileCommand);
      }

      walker.walk(action);

      if (!(tsLang.isValid())) {
          throw std::runtime_error("unable to load - " + lang);
      }
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

} // namespace copypasta
