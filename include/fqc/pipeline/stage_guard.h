// =============================================================================
// fq-compressor - Stage Error Guard
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/pipeline/worker_error.h"

#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>

namespace fqc::pipeline {

/// Record `error` into `slot` only if it is still empty. This is the
/// single-writer primitive; pipelines with a shared first-error slot use the
/// mutex overload below.
inline void recordGuardedError(std::optional<Error>& slot, Error error) {
    if (!slot) {
        slot = std::move(error);
    }
}

/// Multi-writer variant: every stage of a pipeline shares one first-error
/// slot, so the first-writer check and store must be atomic with respect to
/// each other. The lock is taken only on the error path.
inline void recordGuardedError(std::mutex& mutex, std::optional<Error>& slot, Error error) {
    const std::lock_guard lk(mutex);
    recordGuardedError(slot, std::move(error));
}

/// Run `fn` on the calling thread and convert an escaping exception into a
/// worker-crash error. The first error is kept and the pipeline is cancelled
/// cooperatively through `stopSource`; later failures are treated as cascade
/// noise.
template <typename Fn>
void runGuarded(std::string_view stageName,
                std::stop_source& stopSource,
                std::optional<Error>& firstError,
                Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& error) {
        recordGuardedError(firstError, workerCrashError(stageName, error.what()));
        stopSource.request_stop();
    } catch (...) {
        recordGuardedError(firstError, workerCrashError(stageName, "unknown exception"));
        stopSource.request_stop();
    }
}

/// Multi-writer variant used by pipelines with a shared first-error slot.
template <typename Fn>
void runGuarded(std::string_view stageName,
                std::stop_source& stopSource,
                std::optional<Error>& firstError,
                std::mutex& errorMutex,
                Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& error) {
        recordGuardedError(errorMutex, firstError, workerCrashError(stageName, error.what()));
        stopSource.request_stop();
    } catch (...) {
        recordGuardedError(
            errorMutex, firstError, workerCrashError(stageName, "unknown exception"));
        stopSource.request_stop();
    }
}

}  // namespace fqc::pipeline
