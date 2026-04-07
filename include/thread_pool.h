#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <stdexcept>

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop_(false), activeTasks_(0) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] {
                            return stop_ || !tasks_.empty();
                        });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    ++activeTasks_;
                    task();
                    --activeTasks_;
                    doneCv_.notify_all();
                }
            });
        }
    }

    ~ThreadPool() { shutdown(); }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) throw std::runtime_error("ThreadPool is stopped");
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void waitAll() {
        std::unique_lock<std::mutex> lock(mutex_);
        doneCv_.wait(lock, [this] {
            return tasks_.empty() && activeTasks_ == 0;
        });
    }

    size_t queueSize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    size_t threadCount() const { return workers_.size(); }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
    }

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex                mutex_;
    std::condition_variable           cv_;
    std::condition_variable           doneCv_;
    std::atomic<bool>                 stop_;
    std::atomic<int>                  activeTasks_;
};