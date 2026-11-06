// =============================================================================
// fq-compressor - Shared Worker Stage Tests
// =============================================================================

#include "fqc/pipeline/worker_stage.h"

#include "fqc/common/error.h"
#include "fqc/pipeline/mpmc_queue.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using fqc::Error;
using fqc::ErrorCode;
using fqc::makeError;
using fqc::Result;
using fqc::pipeline::MpmcQueue;
using fqc::pipeline::startWorkerStage;
using fqc::pipeline::WorkerStageTimings;
using fqc::pipeline::WorkerStageTimingSink;

namespace {

constexpr std::size_t kInputCapacity = 8;
constexpr std::size_t kOutputCapacity = 64;

struct WorkerRig {
    MpmcQueue<int, kInputCapacity> input;
    MpmcQueue<int, kOutputCapacity> output;
    std::stop_source stopSource;
    std::stop_token stopToken = stopSource.get_token();
    std::mutex errorMutex;
    std::optional<Error> firstError;
    WorkerStageTimingSink timings;
};

[[nodiscard]] auto drainOutput(WorkerRig& rig) -> std::vector<int> {
    rig.output.close();
    std::vector<int> values;
    while (auto value = rig.output.pop()) {
        values.push_back(*value);
    }
    return values;
}

}  // namespace

TEST(WorkerStageTest, ProcessesAllItemsAndMergesTimings) {
    WorkerRig rig;
    constexpr int kItemCount = 10;

    auto workers = startWorkerStage(
        std::size_t{2},
        rig.input,
        rig.output,
        rig.stopToken,
        rig.stopSource,
        rig.errorMutex,
        rig.firstError,
        "test stage",
        [](const int& value, WorkerStageTimings& timings) -> Result<int> {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            timings.computeNs += 1;
            return value * 2;
        },
        rig.timings);

    for (int i = 0; i < kItemCount; ++i) {
        ASSERT_TRUE(rig.input.push(i));
    }
    rig.input.close();
    for (auto& worker : workers) {
        worker.join();
    }

    auto values = drainOutput(rig);
    std::sort(values.begin(), values.end());
    ASSERT_EQ(values.size(), kItemCount);
    for (int i = 0; i < kItemCount; ++i) {
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i * 2);
    }
    EXPECT_FALSE(rig.firstError.has_value());
    EXPECT_FALSE(rig.stopSource.stop_requested());
    EXPECT_EQ(rig.timings.computeNs.load(), kItemCount);
}

TEST(WorkerStageTest, HandlerErrorIsRecordedAndStopsPipeline) {
    WorkerRig rig;

    auto workers = startWorkerStage(
        std::size_t{1},
        rig.input,
        rig.output,
        rig.stopToken,
        rig.stopSource,
        rig.errorMutex,
        rig.firstError,
        "test stage",
        [](const int& value, WorkerStageTimings&) -> Result<int> {
            if (value == 2) {
                return makeError<int>(ErrorCode::kUsageError, "boom");
            }
            return value;
        },
        rig.timings);

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(rig.input.push(i));
    }
    rig.input.close();
    for (auto& worker : workers) {
        worker.join();
    }

    ASSERT_TRUE(rig.firstError.has_value());
    EXPECT_EQ(rig.firstError->code, ErrorCode::kUsageError);
    EXPECT_EQ(rig.firstError->message, "boom");
    EXPECT_TRUE(rig.stopSource.stop_requested());
}

TEST(WorkerStageTest, HandlerExceptionBecomesInternalError) {
    WorkerRig rig;

    auto workers = startWorkerStage(
        std::size_t{1},
        rig.input,
        rig.output,
        rig.stopToken,
        rig.stopSource,
        rig.errorMutex,
        rig.firstError,
        "test stage",
        [](const int&, WorkerStageTimings&) -> Result<int> { throw std::runtime_error("boom"); },
        rig.timings);

    ASSERT_TRUE(rig.input.push(1));
    rig.input.close();
    for (auto& worker : workers) {
        worker.join();
    }

    ASSERT_TRUE(rig.firstError.has_value());
    EXPECT_EQ(rig.firstError->code, ErrorCode::kInternalError);
    EXPECT_NE(rig.firstError->message.find("test stage worker terminated: boom"),
              std::string::npos);
    EXPECT_TRUE(rig.stopSource.stop_requested());
}

TEST(WorkerStageTest, StopTokenReleasesBlockedWorker) {
    WorkerRig rig;

    auto workers = startWorkerStage(
        std::size_t{1},
        rig.input,
        rig.output,
        rig.stopToken,
        rig.stopSource,
        rig.errorMutex,
        rig.firstError,
        "test stage",
        [](const int& value, WorkerStageTimings&) -> Result<int> { return value; },
        rig.timings);

    rig.stopSource.request_stop();
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_FALSE(rig.firstError.has_value());
    EXPECT_TRUE(drainOutput(rig).empty());
}
