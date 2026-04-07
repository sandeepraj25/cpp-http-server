#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <iostream>

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void setLevel(LogLevel lvl) { level_ = lvl; }
    void setFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.open(path, std::ios::app);
    }

    template<typename... Args>
    void log(LogLevel lvl, Args&&... args) {
        if (lvl < level_) return;
        std::ostringstream oss;
        oss << timestamp() << " [" << levelStr(lvl) << "] ";
        (oss << ... << std::forward<Args>(args));
        std::string msg = oss.str();
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << msg << "\n";
        if (file_.is_open()) file_ << msg << "\n", file_.flush();
    }

private:
    Logger() : level_(LogLevel::INFO) {}
    LogLevel   level_;
    std::mutex mutex_;
    std::ofstream file_;

    std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
    const char* levelStr(LogLevel l) {
        switch(l) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
        }
        return "?????";
    }
};

#define LOG_DEBUG(...) Logger::instance().log(LogLevel::DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  Logger::instance().log(LogLevel::INFO,  __VA_ARGS__)
#define LOG_WARN(...)  Logger::instance().log(LogLevel::WARN,  __VA_ARGS__)
#define LOG_ERROR(...) Logger::instance().log(LogLevel::ERROR, __VA_ARGS__)