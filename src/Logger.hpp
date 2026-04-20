#pragma once

#include <cstdio>
#include <ctime>
#include <string>
#include <mutex>

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    static void setLevel(LogLevel level) { s_level = level; }
    static void setLevel(const std::string& level) {
        if (level == "debug")      s_level = LogLevel::Debug;
        else if (level == "warn")  s_level = LogLevel::Warn;
        else if (level == "error") s_level = LogLevel::Error;
        else                       s_level = LogLevel::Info;
    }

    template<typename... Args>
    static void debug(const char* fmt, Args&&... args) { log(LogLevel::Debug, "DEBUG", fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    static void info(const char* fmt, Args&&... args)  { log(LogLevel::Info,  "INFO ", fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    static void warn(const char* fmt, Args&&... args)  { log(LogLevel::Warn,  "WARN ", fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    static void error(const char* fmt, Args&&... args) { log(LogLevel::Error, "ERROR", fmt, std::forward<Args>(args)...); }

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

inline LogLevel Logger::s_level = LogLevel::Info;
inline std::mutex Logger::s_mutex;
