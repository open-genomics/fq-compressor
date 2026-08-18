#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace fqc {

enum class ErrorCode : std::uint8_t {
    kSuccess = 0,
    kUsageError = 1,
    kIOError = 2,
    kFormatError = 3,
    kChecksumError = 4,
    kUnsupportedCodec = 5,
    kInternalError = 17
};

[[nodiscard]] constexpr int toExitCode(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kSuccess:
            return 0;
        case ErrorCode::kUsageError:
            return 1;
        case ErrorCode::kIOError:
            return 2;
        case ErrorCode::kFormatError:
        case ErrorCode::kInternalError:
            return 3;
        case ErrorCode::kChecksumError:
            return 4;
        case ErrorCode::kUnsupportedCodec:
            return 5;
    }
    return 1;
}

[[nodiscard]] constexpr std::string_view errorCodeToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kSuccess:
            return "success";
        case ErrorCode::kUsageError:
            return "usage error";
        case ErrorCode::kIOError:
            return "I/O error";
        case ErrorCode::kFormatError:
            return "format error";
        case ErrorCode::kChecksumError:
            return "checksum error";
        case ErrorCode::kUnsupportedCodec:
            return "unsupported codec";
        case ErrorCode::kInternalError:
            return "internal error";
    }
    return "unknown error";
}

struct Error {
    ErrorCode code;
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

using VoidResult = std::expected<void, Error>;

template <typename T>
[[nodiscard]] Result<T> makeError(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

template <typename T>
[[nodiscard]] Result<T> makeError(Error error) {
    return std::unexpected(std::move(error));
}

[[nodiscard]] inline VoidResult makeVoidError(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

}  // namespace fqc

#define FQC_CONCAT_INNER(a, b) a##b
#define FQC_CONCAT(a, b) FQC_CONCAT_INNER(a, b)

/// If `expr` is an error `Result`/`VoidResult`, return that error from the
/// enclosing function. Success values are discarded; use `FQC_TRY_ASSIGN` to
/// bind one. Parentheses around `expr` are not required, even with commas.
#define FQC_TRY(...)                                                      \
    do {                                                                  \
        if (auto _fqcTryResult = (__VA_ARGS__); !_fqcTryResult) {         \
            return ::std::unexpected(::std::move(_fqcTryResult.error())); \
        }                                                                 \
    } while (0)

/// Like `FQC_TRY`, but binds the success value to `var`.
#define FQC_TRY_ASSIGN(var, ...)                                                       \
    auto FQC_CONCAT(_fqcTry_, __LINE__) = (__VA_ARGS__);                               \
    if (!FQC_CONCAT(_fqcTry_, __LINE__)) {                                             \
        return ::std::unexpected(::std::move(FQC_CONCAT(_fqcTry_, __LINE__).error())); \
    }                                                                                  \
    auto var = ::std::move(*FQC_CONCAT(_fqcTry_, __LINE__))
