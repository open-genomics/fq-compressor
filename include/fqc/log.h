#pragma once

#include <cstdint>
#include <cstdio>

#include <fmt/format.h>

namespace fqc::log {

enum class Level : std::uint8_t { kDebug = 0, kInfo, kWarning, kError };

inline Level threshold = Level::kInfo;

constexpr const char* tag(Level l) {
    switch (l) {
        case Level::kDebug:
            return "DBG";
        case Level::kInfo:
            return "INF";
        case Level::kWarning:
            return "WRN";
        default:
            return "ERR";
    }
}

template <typename... Args>
void emit(Level lvl, fmt::format_string<Args...> f, Args&&... args) {
    if (static_cast<int>(lvl) < static_cast<int>(threshold)) {
        return;
    }
    fmt::println(stderr, "[{}] {}", tag(lvl), fmt::format(f, std::forward<Args>(args)...));
}

}  // namespace fqc::log

#define FQC_LOG_DEBUG(...) ::fqc::log::emit(::fqc::log::Level::kDebug, __VA_ARGS__)
#define FQC_LOG_INFO(...) ::fqc::log::emit(::fqc::log::Level::kInfo, __VA_ARGS__)
#define FQC_LOG_WARNING(...) ::fqc::log::emit(::fqc::log::Level::kWarning, __VA_ARGS__)
#define FQC_LOG_ERROR(...) ::fqc::log::emit(::fqc::log::Level::kError, __VA_ARGS__)
