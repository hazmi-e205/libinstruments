#ifndef INSTRUMENTS_LOG_H
#define INSTRUMENTS_LOG_H

#include "../../include/instruments/types.h"
#include <cstdio>
#include <cstdarg>
#include <array>

#if defined(__has_include)
#if __has_include(<QDebug>)
#include <QDebug>
#define INSTRUMENTS_LOG_USE_QDEBUG 1
#elif __has_include(<QtCore/QDebug>)
#include <QtCore/QDebug>
#define INSTRUMENTS_LOG_USE_QDEBUG 1
#endif
#endif

namespace instruments {

class Log {
public:
    static void SetLevel(LogLevel level) { s_level = level; }
    static LogLevel GetLevel() { return s_level; }

    static void Write(LogLevel level, const char* tag, const char* fmt, ...) {
        if (level > s_level) return;
        const char* prefix = "";
        switch (level) {
            case LogLevel::Error: prefix = "ERROR"; break;
            case LogLevel::Warn:  prefix = "WARN "; break;
            case LogLevel::Info:  prefix = "INFO "; break;
            case LogLevel::Debug: prefix = "DEBUG"; break;
            case LogLevel::Trace: prefix = "TRACE"; break;
            default: break;
        }

        std::array<char, 2048> msg{};
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(msg.data(), msg.size(), fmt, args);
        va_end(args);

#if defined(INSTRUMENTS_LOG_USE_QDEBUG)
        qDebug().noquote().nospace() << "[" << prefix << "][" << tag << "] " << msg.data();
#else
        fprintf(stderr, "[%s][%s] ", prefix, tag);
        fprintf(stderr, "%s", msg.data());
        fprintf(stderr, "\n");
#endif
    }

private:
    static inline LogLevel s_level = LogLevel::Info;
};

} // namespace instruments

// Guard argument evaluation so expensive calls (e.g. Dump()) are skipped
// when the log level is above the threshold. Define INSTRUMENTS_ENABLE_LOGGING
// to compile in logging calls (disabled by default).
#if defined(INSTRUMENTS_ENABLE_LOGGING)
#define INST_LOG_ERROR(tag, ...) do { if (::instruments::LogLevel::Error <= ::instruments::Log::GetLevel()) ::instruments::Log::Write(::instruments::LogLevel::Error, tag, __VA_ARGS__); } while(0)
#define INST_LOG_WARN(tag, ...)  do { if (::instruments::LogLevel::Warn  <= ::instruments::Log::GetLevel()) ::instruments::Log::Write(::instruments::LogLevel::Warn,  tag, __VA_ARGS__); } while(0)
#define INST_LOG_INFO(tag, ...)  do { if (::instruments::LogLevel::Info  <= ::instruments::Log::GetLevel()) ::instruments::Log::Write(::instruments::LogLevel::Info,  tag, __VA_ARGS__); } while(0)
#define INST_LOG_DEBUG(tag, ...) do { if (::instruments::LogLevel::Debug <= ::instruments::Log::GetLevel()) ::instruments::Log::Write(::instruments::LogLevel::Debug, tag, __VA_ARGS__); } while(0)
#define INST_LOG_TRACE(tag, ...) do { if (::instruments::LogLevel::Trace <= ::instruments::Log::GetLevel()) ::instruments::Log::Write(::instruments::LogLevel::Trace, tag, __VA_ARGS__); } while(0)
#else
#define INST_LOG_ERROR(tag, ...) do { (void)(tag); } while(0)
#define INST_LOG_WARN(tag, ...)  do { (void)(tag); } while(0)
#define INST_LOG_INFO(tag, ...)  do { (void)(tag); } while(0)
#define INST_LOG_DEBUG(tag, ...) do { (void)(tag); } while(0)
#define INST_LOG_TRACE(tag, ...) do { (void)(tag); } while(0)
#endif

#endif // INSTRUMENTS_LOG_H
