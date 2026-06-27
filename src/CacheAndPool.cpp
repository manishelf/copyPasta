#include <CacheAndPool.hpp>
#include <Logger.hpp>
#include <functional>
#include <exception>

namespace copypasta {
    // PcreCache 
    pcre2_code* PcreCache::get(const std::string& pattern, uint32_t opt_compile) {
        Key k{ pattern, opt_compile };
        {
            DEBUG_FULL("PcreCache get read lock");
            std::shared_lock<std::shared_mutex> lock(mtx);
            auto it = cache.find(k);
            if (it != cache.end()) {
                DEBUG_FULL("PcreCache found from cache");
                return it->second;
            }
        }

        DEBUG_FULL("PcreCache get write lock");

        std::unique_lock<std::shared_mutex> lock(mtx);

        DEBUG_FULL("PcreCache compile pattern - " << pattern);
        int errornumber;
        PCRE2_SIZE erroroffset;
        pcre2_code* re = pcre2_compile((PCRE2_SPTR)pattern.c_str(),
            PCRE2_ZERO_TERMINATED, opt_compile,
            &errornumber, &erroroffset, NULL);
        if (re == NULL) {
            PCRE2_UCHAR buf[256];
            pcre2_get_error_message(errornumber, buf, sizeof(buf));
            std::string msg = "PcreCache: regex compile failed for pattern: '" + pattern + "'\n";
            msg += "  Error: "
                + std::string(reinterpret_cast<char*>(buf))
                + " (code " + std::to_string(errornumber) + ")\n"
                + "  At offset: " + std::to_string(erroroffset) + "\n";

            size_t ctx = 10;
            size_t start = (erroroffset > ctx) ? erroroffset - ctx : 0;
            size_t end = std::min(pattern.size(), static_cast<size_t>(erroroffset + ctx));
            msg += "  Context: ..." + pattern.substr(start, erroroffset - start) + ">>><<<";

            if (erroroffset < pattern.size()) {
                msg += pattern.substr(erroroffset, end - erroroffset);
            }

            msg += "...\n";
            LERROR(msg);
            throw std::invalid_argument(msg);
        }
        pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
        DEBUG_FULL("PcreCache compile pattern done - " << pattern);
        auto [it, _] = cache.emplace(k, re);
        return it->second;
    }

    //TSEnginePool (dont use with ThreadPool)
    std::shared_ptr<TSEngine> TSEnginePool::get(const TSLanguage* lang) {
        {
            DEBUG_FULL("TSEnginePool get read lock");
            std::shared_lock<std::shared_mutex>  lock(mtx);
            auto it = engines.find(lang);
            if (it != engines.end()) {
                DEBUG_FULL("TSEnginePool get found from cache");
                return it->second;
            }
        }
        DEBUG_FULL("TSEnginePool get write lock");
        std::unique_lock<std::shared_mutex>  lock(mtx);
        auto ptr = std::make_shared<TSEngine>(lang);
        engines[lang] = ptr;
        return ptr;
    }

    // TSQueryCache
    TSQuery* TSQueryCache::get(const TSEngine* engine, const std::string& pattern) {
        auto key = std::make_pair(engine, pattern);
        {
            DEBUG_FULL("TSQueryCache get read lock");
            std::shared_lock<std::shared_mutex>  lock(mtx);
            auto it = cache.find(key);
            if (it != cache.end()) {
                DEBUG_FULL("TSQueryCache found from cache");
                return it->second;
            }
        }
        DEBUG_FULL("TSQueryCache get write lock");
        std::unique_lock<std::shared_mutex>  lock(mtx);
        TSQuery* q = engine->queryNew(const_cast<std::string&>(pattern));
        cache[key] = q;
        return q;
    }


    // ThreadPool
    ThreadPool::ThreadPool(size_t maxCount) {
        DEBUG_FULL("ThreadPool ctor");
        this->maxCount = maxCount;
        stop = false;
        for (size_t i = 0; i < maxCount; ++i) {

            workers.emplace_back([this] {

                DEBUG_FULL("ThreadPool worker ctor");
                // constructor of Thread callable
                while (true) {
                    DEBUG_FULL("ThreadPool worker next");
                    std::function<void()> job;
                    {
                        DEBUG_FULL("ThreadPool worker lock queue");
                        std::unique_lock<std::mutex> lock(queueMutex);
                        DEBUG("ThreadPool worker wait for task");
                        // Wait until there is a task or we are stopping
                        enqueueCondition.wait(lock, [this] { return stop || !task.empty(); });
                        if (stop && task.empty())
                            return;

                        job = std::move(task.front());
                        task.pop();
                    }
                    DEBUG("ThreadPool worker do job");
                    try {
                        job(); // Execute the action
                    }
                    catch (const std::exception& e) {
                        LERROR("Exception: " << e.what());
                    }
                    catch (...) {
                        LERROR("Unknown exception during processing of job\n");
                    }
                    DEBUG("ThreadPool worker job done");
                    if (activeTasks.fetch_sub(1) == 1) {
                        DEBUG("ThreadPool worker all jobs done");
                        // this was the last job
                        finishCondition.notify_all();
                    }
                }
                });
        }
    }

    ThreadPool::~ThreadPool() {
        DEBUG_FULL("ThreadPool destroyed");
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        enqueueCondition.notify_all(); // Wake up all threads to let them finish
        for (std::thread& worker : workers) {
            worker.join(); // Wait for every thread to finish its current job
        }
    }

    std::shared_ptr<FileSnapshot> FileSnapCache::get(const std::string& path) {
      std::string key = fs::absolute(fs::path(path).lexically_normal()).string();
      {
          DEBUG_FULL("FileReaderCache get read lock");
          std::shared_lock<std::shared_mutex>  lock(mtx);

          auto it = cache.find(key);
          if (it != cache.end()) {
              return it->second;
          }
      }

      DEBUG_FULL("FileReaderCache get write lock");
      std::unique_lock<std::shared_mutex>  lock(mtx);
      FileReader fr(key);
      auto snap = std::make_shared<FileSnapshot>(fr.isValid() ? fr.snapshot() : FileSnapshot{0});
      cache[key] = snap;
      return snap;
    }

    std::shared_ptr<FileSnapshot> FileSnapCache::updateAndGet(const std::string& path) {
      invalidate(path);
      return get(path);
    }

    void FileSnapCache::invalidate(const std::string& path) {
      std::string key = fs::absolute(fs::path(path).lexically_normal()).string();
      DEBUG_FULL("FileReaderCache invalidate " << key);
      std::unique_lock<std::shared_mutex> lock(mtx);
      cache.erase(key);
    }

    void FileSnapCache::clear() {
      DEBUG_FULL("FileReaderCache clear");
      std::unique_lock<std::shared_mutex> lock(mtx);
      cache.clear();
    }


    std::shared_ptr<std::vector<fs::directory_entry>> DirEntryCache::getCachedChildren(const std::string& root) {
        {
            DEBUG_FULL("DirEntryCache get read lock");
            std::shared_lock<std::shared_mutex>  lock(mtx);
            auto it = cache.find(root);
            if (it != cache.end()) {
                return it->second;
            }
        }

        DEBUG_FULL("DirEntryCache get write lock");
        std::unique_lock<std::shared_mutex>  lock(mtx);
        
        auto children = std::make_shared<std::vector<fs::directory_entry>>(
            fs::directory_iterator(root), // begin it
            fs::directory_iterator() // // end it
        );

        cache[root] = children;
        return children;
    }

    void DirEntryCache::clear() {
        DEBUG_FULL("DirEntryCache clear");
        std::unique_lock<std::shared_mutex>  lock(mtx);
        cache.clear();
    }

} // copypasta
