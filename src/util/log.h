#ifndef INSTRUMENTS_LOG_H
#define INSTRUMENTS_LOG_H

#include "../../include/instruments/types.h"
#include <cstdio>
#include <cstdarg>

namespace instruments {

class Log {
public:
    static void SetLevel(LogLevel level) { s_level = level; }
    static LogLevel GetLevel() { return s_level; }

    static void Write(LogLevel level, const char* tag, const char* fmt, ...) {
        if (level > s_level) return;
        va_list args;
        va_start(args, fmt);
        const char* prefix = "";
        switch (level) {
            case LogLevel::Error: prefix = "ERROR"; break;
            case LogLevel::Warn:  prefix = "WARN "; break;
            case LogLevel::Info:  prefix = "INFO "; break;
            case LogLevel::Debug: prefix = "DEBUG"; break;
            case LogLevel::Trace: prefix = "TRACE"; break;
            default: break;
        }
        fprintf(stderr, "[%s][%s] ", prefix, tag);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

private:
    static inline LogLevel s_level = LogLevel::Info;
};

} // namespace instruments

#define INST_LOG_ERROR(tag, ...) ::instruments::Log::Write(::instruments::LogLevel::Error, tag, __VA_ARGS__)
#define INST_LOG_WARN(tag, ...)  ::instruments::Log::Write(::instruments::LogLevel::Warn, tag, __VA_ARGS__)
#define INST_LOG_INFO(tag, ...)  ::instruments::Log::Write(::instruments::LogLevel::Info, tag, __VA_ARGS__)
#define INST_LOG_DEBUG(tag, ...) ::instruments::Log::Write(::instruments::LogLevel::Debug, tag, __VA_ARGS__)
#define INST_LOG_TRACE(tag, ...) ::instruments::Log::Write(::instruments::LogLevel::Trace, tag, __VA_ARGS__)

#endif // INSTRUMENTS_LOG_H
