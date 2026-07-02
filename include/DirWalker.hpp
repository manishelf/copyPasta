#ifndef DIR_WALKER_HPP
#define DIR_WALKER_HPP

#include <FileReaderWriter.hpp>
#include <LibGit.hpp>
#include <CacheAndPool.hpp>
#include <Logger.hpp>

#include <string>
#include <set>
#include <atomic>


namespace copypasta {

class DirWalker {

public:
  std::string path;
  size_t level = 0;
  bool recursive = false;
  bool inverted = false;
  bool includeDotDir = false;
  bool obeyGitIgnore = true;
  bool filesOnly = true;
  bool useCache = false;
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

  DirWalker(std::string dir) {
    DEBUG_FULL("DirWalker ctor");
    path = dir;
  };

  bool isValid(){
    bool exists = fs::exists(path);
    if(!exists){
      LERROR("DirWalker isValid - path " << path << " does not exist");
    }
    return exists; 
  }

  void copyConfig(DirWalker* other){  
    recursive     = other->recursive;
    obeyGitIgnore = other->obeyGitIgnore;
    includeDotDir = other->includeDotDir;
    ignore        = other->ignore;
    inverted      = other->inverted;
    matchExt      = other->matchExt;
    filesOnly     = other->filesOnly;
  }

  ~DirWalker() {
    DEBUG_FULL("DirWalker destroyed");
  };

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

  std::shared_ptr<std::vector<fs::directory_entry>> getChildren() {
    std::shared_ptr<std::vector<fs::directory_entry>> entries;
    if (useCache) {
      DEBUG("DirWalker walk with pool use cached path");
      DirEntryCache& dirCache = DirEntryCache::global();
      entries = dirCache.getCachedChildren(this->path);
    }
    else {
      entries = std::make_shared<std::vector<fs::directory_entry>>(
          fs::directory_iterator(this->path), // begin it
          fs::directory_iterator() // end it
      );
    }
    return entries;
  }

  template <typename Payload, typename Action>
  STATUS walk(LibGit& repo, Action &&action, Payload &payload = NULL);

  using AbortSignal = std::shared_ptr<std::atomic<bool>>;
  template <typename Payload, typename Action>
  void walk(LibGit& repo, ThreadPool &pool, Action &&action,
            AbortSignal globalAbort, Payload &payload);
};

// IMPL

template <typename Action>
DirWalker::STATUS DirWalker::walk(Action &&action) {
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
 STATUS res = walk(repo, action, payload);

 DEBUG("DirWalker walk begin - " << path);
 return res;
};

template <typename Payload, typename Action>
DirWalker::STATUS DirWalker::walk(LibGit& repo, Action &&action,
                                  Payload &payload) {

  if (!isValid())
    return FAILED;

  DEBUG_FULL("DirWalker walk begin - " << path);

  auto entries = getChildren();
  
  for (size_t i = 0; i < entries->size(); i++) {

    auto entry = entries->at(i);
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

    File file(entries->at(i));
    file.level = level;

  
    ACTION actRes;
    if(!filesOnly || !file.isDir){
      DEBUG("DirWalker walk do job - \n" << file.pathStr);
      actRes = callAction(action, OPENED, file, repo, payload);
      DEBUG("DirWalker walk job done - \n" << file.pathStr);
    } else{
      actRes = ACTION::CONTINUE;
    }

    if (actRes == ACTION::SKIP) {
      DEBUG("DirWalker skip - \n" << file.pathStr);
      continue;
    } else if (actRes == ACTION::STOP) {
      DEBUG("DirWalker stop - \n" << file.pathStr);
      return STATUS::STOPPED;
    } else if (actRes == ACTION::ABORT) {
      DEBUG("DirWalker abort - \n" << file.pathStr);
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

    if (inverted && i == entries->size() - 1) {
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

  AbortSignal abortSignal = std::make_shared<std::atomic<bool>>(false);

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

  if(!isValid()){
    return;
  }

  DEBUG("DirWalker walk with pool start - " << path);
  auto entries = getChildren();

  for (size_t i = 0; i < entries->size(); i++) {

    auto entry = entries->at(i);
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

    File file(entries->at(i));
    file.level = level;

    ACTION actRes;
    if(!filesOnly || !file.isDir){
      actRes = callAction(action, QUEUING, file, repo, payload);
    } else{
      actRes = ACTION::CONTINUE;
    }

    if (actRes == ACTION::STOP) {
      DEBUG("DirWalker with pool stop - \n" << file.pathStr);
      return;
    }
    if (actRes == ACTION::SKIP){
      DEBUG("DirWalker with pool skip - \n" << file.pathStr);
      continue;
    }
    if (actRes == ACTION::ABORT) {
      DEBUG("DirWalker with pool abort - \n" << file.pathStr);
      abortSignal->store(true);
      return;
    }
 
    if (!((file.name == ".") || (file.name == "..")) && file.isDir &&
        recursive && !inverted) {
      DirWalker child(file.pathStr);
      child.copyConfig(this);
      child.level = level + 1;
      child.walk(repo, pool, action, abortSignal, payload);
    } else if (inverted && i == entries->size() - 1) {
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

      DEBUG_FULL("DirWalker walk with pool enqueue job - \n" << file.pathStr);
      // create a anonlymous class that has action and file in constructor
      pool.enqueue([action, file, &repo, abortSignal, &payload]() {
        if (abortSignal->load())
          return;

        DEBUG("DirWalker walk with pool do job - \n" << file.pathStr);
        ACTION actRes = callAction(action, OPENED, file, repo, payload);
        DEBUG("DirWalker walk with pool job done - \n" << file.pathStr);
        if (actRes == ACTION::ABORT) {
          DEBUG("DirWalker with pool abort called");
          abortSignal->store(true);
        }
      });
    }
  }
}

} // namespace copypasta

#endif // DIR_WALKER_HPP
