#include <CacheAndPool.hpp>
#include <Logger.hpp>
#include <functional>

namespace copypasta {
    // PcreCache 
    pcre2_code* PcreCache::get(const std::string& pattern, uint32_t opt_compile) {
        Key k{ pattern, opt_compile };
        {
            DEBUG_FULL("PcreCache lock mtx");
            std::lock_guard<std::mutex> lock(mtx);
            auto it = cache.find(k);
            if (it != cache.end()) {
                DEBUG_FULL("PcreCache found from cache");
                return it->second;
            }
        }

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
        std::lock_guard<std::mutex> lock(mtx);
        auto [it, _] = cache.emplace(k, re);
        return it->second;
    }

    //TSEnginePool (dont use with ThreadPool)
    std::shared_ptr<TSEngine> TSEnginePool::get(const TSLanguage* lang) {

        DEBUG_FULL("TSEnginePool get lock mtx");
        std::lock_guard<std::mutex> lock(mtx);
        auto it = engines.find(lang);
        if (it != engines.end()) {
            DEBUG_FULL("TSEnginePool get found from cache");
            return it->second;
        }

        auto ptr = std::make_shared<TSEngine>(lang);
        engines[lang] = ptr;
        return ptr;
    }

    // TSQueryCache
    TSQuery* TSQueryCache::get(const TSEngine* engine, const std::string& pattern) {

        DEBUG_FULL("TSQueryCache get lock mtx");
        std::lock_guard<std::mutex> lock(mtx);
        auto key = std::make_pair(engine, pattern);
        auto it = cache.find(key);
        if (it != cache.end()) {
            DEBUG_FULL("TSQueryCache found from cache");
            return it->second;
        }

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
                    job(); // Execute the action
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

    std::shared_ptr<FileReader> FileReaderCache::get(const std::string& path) {
      std::string key = fs::absolute(fs::path(path).lexically_normal()).string();

      std::lock_guard<std::mutex> lock(mtx);

      auto it = cache.find(key);
      if (it != cache.end()){
        return it->second;
      }

      auto reader = std::make_shared<FileReader>(path);

      if (reader->isValid()){
        cache[key] = reader;
      }

      return reader;
    }

    std::shared_ptr<FileReader> FileReaderCache::updateAndGet(const std::string& path) {
      invalidate(path);
      return get(path);
    }

    void FileReaderCache::invalidate(const std::string& path) {
      std::string key = fs::absolute(fs::path(path).lexically_normal()).string();
      std::lock_guard<std::mutex> lock(mtx);
      cache.erase(key);
    }

    void FileReaderCache::clear() {
      std::lock_guard<std::mutex> lock(mtx);
      cache.clear();
    }
} // copypasta
