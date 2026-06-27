#ifndef CACHE_AND_POOL_HPP
#define CACHE_AND_POOL_HPP

#include <string>
#include <mutex>
#include <map>
#include <queue>
#include <thread>
#include <functional>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <TSEngine.hpp>

#include <condition_variable>
#include <atomic>
#include <shared_mutex>

namespace copypasta {

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

  template <class F> 
  void enqueue(F &&f) {
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      activeTasks++;
      task.emplace(std::forward<F>(f));
    }
    enqueueCondition.notify_one();
  }

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

  static ThreadPool& global() {
      static ThreadPool instance;
      return instance;
  }
};

// Thread-safe cache for compiled PCRE2 patterns.
// Patterns are keyed by (pattern_string, compile_options).
// The compiled pcre2_code* is owned by the cache for its lifetime.
// Callers must NOT call pcre2_code_free on pointers returned by get().
class PcreCache {
  struct Key {
    std::string pattern;
    uint32_t    opts;
    bool operator<(const Key &o) const {
      return pattern < o.pattern || (pattern == o.pattern && opts < o.opts);
    }
  };
  std::map<Key, pcre2_code *> cache;
  mutable std::shared_mutex mtx;

public:
  PcreCache(){}

  ~PcreCache() {
    for (auto &[k, re] : cache)
      pcre2_code_free(re);
    cache.clear();
  }

  pcre2_code *get(const std::string &pattern, uint32_t opt_compile = PCRE2_CASELESS);

  // thread safe
  static PcreCache &global() {
    static PcreCache instance;
    return instance;
  }
};

// Thread-safe pool for persistent TSEngine instances per language
class TSEnginePool {
  mutable std::shared_mutex mtx;
  std::map<const TSLanguage*, std::shared_ptr<TSEngine>> engines;
public:
  std::shared_ptr<TSEngine> get(const TSLanguage* lang);
  // thread safe
  static TSEnginePool &global() {
    static TSEnginePool instance;
    return instance;
  }
};

// Thread-safe query cache for reusing TSQuery* per engine and pattern
class TSQueryCache {
   mutable std::shared_mutex mtx;
   std::map<std::pair<const TSEngine*, std::string>, TSQuery*> cache;
public:
  TSQuery* get(const TSEngine* engine, const std::string& pattern); 
  // thread safe
  static TSQueryCache &global() {
    static TSQueryCache instance;
    return instance;
  }
};


class FileSnapCache {
  std::unordered_map<std::string, std::shared_ptr<FileSnapshot>> cache;
  mutable std::shared_mutex mtx;

public:
  static FileSnapCache& global() {
    static FileSnapCache instance;
    return instance;
  }

  std::shared_ptr<FileSnapshot> get(const std::string& path);
  std::shared_ptr<FileSnapshot> updateAndGet(const std::string& path);
  void invalidate(const std::string& path);
  void clear();

};

class DirEntryCache {
    std::unordered_map<std::string, std::shared_ptr<std::vector<fs::directory_entry>>> cache;
    mutable std::shared_mutex mtx;

public:

    std::shared_ptr<std::vector<fs::directory_entry>> getCachedChildren(const std::string& key);
    void clear();

    // thread safe
    static DirEntryCache& global() {
        static DirEntryCache instance;
        return instance;
    }
};

class FileMetaCache {

};

} // namespace copypasta

#endif // CACHE_AND_POOL_HPP
