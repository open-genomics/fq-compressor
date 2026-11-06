// =============================================================================
// fq-compressor - Shared Worker Stage
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/pipeline/mpmc_queue.h"
#include "fqc/pipeline/timing.h"
#include "fqc/pipeline/worker_error.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fqc::pipeline {

/// Per-worker wall-clock counters. Each worker accumulates into a local copy
/// and merges once at exit, so clock reads and atomic traffic stay out of the
/// per-item hot path. `computeNs` is the primary transform stage; `secondaryNs`
/// is an optional second stage (zstd compression in the encoder pipelines).
struct WorkerStageTimings {
    std::uint64_t popNs = 0;
    std::uint64_t computeNs = 0;
    std::uint64_t secondaryNs = 0;
    std::uint64_t pushNs = 0;
};

/// Shared atomic sink for `WorkerStageTimings`.
struct WorkerStageTimingSink {
    std::atomic<std::uint64_t> popNs{0};
    std::atomic<std::uint64_t> computeNs{0};
    std::atomic<std::uint64_t> secondaryNs{0};
    std::atomic<std::uint64_t> pushNs{0};

    void merge(const WorkerStageTimings& local) {
        popNs.fetch_add(local.popNs, std::memory_order_relaxed);
        computeNs.fetch_add(local.computeNs, std::memory_order_relaxed);
        secondaryNs.fetch_add(local.secondaryNs, std::memory_order_relaxed);
        pushNs.fetch_add(local.pushNs, std::memory_order_relaxed);
    }
};

/// Start `workerCount` threads that repeatedly pop from `input`, call
/// `handler(item, localTimings)` and push the result to `output`.
///
/// This helper owns only the repeated control flow and exception/error
/// barrier. The caller keeps the pipeline-specific lifecycle: it decides when
/// to close queues and when to join the returned threads. The handler must be
/// copyable, must not throw (the barrier catches accidents), and returns a
/// `Result<Output>`; an error records the first failure under `errorMutex`,
/// requests cancellation, and stops that worker.
///
/// All referenced queues, stop state and the timing sink must outlive the
/// returned threads. `stageName` is used in `workerCrashError`; it must refer
/// to storage that outlives the threads (string literals in practice).
template <typename Input,
          std::size_t InputCapacity,
          typename Output,
          std::size_t OutputCapacity,
          typename Handler>
[[nodiscard]] auto startWorkerStage(std::size_t workerCount,
                                    MpmcQueue<Input, InputCapacity>& input,
                                    MpmcQueue<Output, OutputCapacity>& output,
                                    const std::stop_token& stopToken,
                                    std::stop_source& stopSource,
                                    std::mutex& errorMutex,
                                    std::optional<Error>& firstError,
                                    std::string_view stageName,
                                    Handler handler,
                                    WorkerStageTimingSink& timings) -> std::vector<std::jthread> {
    assert(workerCount > 0 && "worker stage needs at least one worker");
    std::vector<std::jthread> workers;
    workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        workers.emplace_back([&, handler, stopToken, stageName]() mutable {
            WorkerStageTimings local;
            try {
                while (!stopToken.stop_requested()) {
                    const auto popStart = Clock::now();
                    auto in = input.pop(stopToken);
                    local.popNs += nanosSince(popStart);
                    if (!in.has_value()) {
                        break;
                    }
                    auto out = handler(*in, local);
                    if (!out) {
                        const std::lock_guard lk(errorMutex);
                        if (!firstError) {
                            firstError = out.error();
                        }
                        stopSource.request_stop();
                        break;
                    }
                    const auto pushStart = Clock::now();
                    const bool pushed = output.push(std::move(*out), stopToken);
                    local.pushNs += nanosSince(pushStart);
                    if (!pushed) {
                        break;
                    }
                }
            } catch (const std::exception& error) {
                const std::lock_guard lk(errorMutex);
                if (!firstError) {
                    firstError = workerCrashError(stageName, error.what());
                }
                stopSource.request_stop();
            } catch (...) {
                const std::lock_guard lk(errorMutex);
                if (!firstError) {
                    firstError = workerCrashError(stageName, "unknown exception");
                }
                stopSource.request_stop();
            }
            timings.merge(local);
        });
    }
    return workers;
}

}  // namespace fqc::pipeline
