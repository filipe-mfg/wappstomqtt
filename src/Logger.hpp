#pragma once

#include <cstdio>
#include <ctime>
#include <string>
#include <mutex>

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Logger {
public:
    static void setLevel(LogLevel level) { s_level = level; }
    static void setLevel(const std::string& level) {
        if (level == "debug")      s_level = LogLevel::DEBUG;
        else if (level == "warn")  s_level = LogLevel::WARN;
        else if (level == "error") s_level = LogLevel::ERROR;
        else                       s_level = LogLevel::INFO;
    }

    template<typename... Args>
    static void debug(const char* fmt, Args&&... args) { log(LogLevel::DEBUG, "DEBUG", fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    static void info(const char* fmt, Args&&... args)  { log(LogLevel::INFO,  "INFO ", fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    static void warn(const char* fmt, Args&&... args)  { log(LogLevel::WARN,  "WARN ", fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    static void error(const char* fmt, Args&&... args) { log(LogLevel::ERROR, "ERROR", fmt, std::forward<Args>(args)...); }

private:
    static LogLevel s_level;
    static std::mutex s_mutex;

    template<typename... Args>
    static void log(LogLevel level, const char* tag, const char* fmt, Args&&... args) {
        if (level < s_level) return;
        std::lock_guard<std::mutex> lock(s_mutex);
        char ts[32];
        std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));
        std::fprintf(stderr, "[%s] [%s] ", ts, tag);
        std::fprintf(stderr, fmt, std::forward<Args>(args)...);
        std::fprintf(stderr, "\n");
    }
};

inline LogLevel Logger::s_level = LogLevel::INFO;
inline std::mutex Logger::s_mutex;
