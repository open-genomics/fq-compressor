// =============================================================================
// fq-compressor - Stage Error Guard Tests
// =============================================================================

#include "fqc/pipeline/stage_guard.h"

#include "fqc/common/error.h"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

#include <gtest/gtest.h>

using fqc::Error;
using fqc::ErrorCode;
using fqc::pipeline::runGuarded;

TEST(StageGuardTest, SuccessDoesNotRecordOrStop) {
    std::stop_source stopSource;
    std::optional<Error> firstError;
    bool ran = false;

    runGuarded("test stage", stopSource, firstError, [&] { ran = true; });

    EXPECT_TRUE(ran);
    EXPECT_FALSE(firstError.has_value());
    EXPECT_FALSE(stopSource.stop_requested());
}

TEST(StageGuardTest, ExceptionRecordsFirstErrorAndStops) {
    std::stop_source stopSource;
    std::optional<Error> firstError;

    runGuarded("test stage", stopSource, firstError, [] { throw std::runtime_error("boom"); });

    ASSERT_TRUE(firstError.has_value());
    EXPECT_EQ(firstError->code, ErrorCode::kInternalError);
    EXPECT_NE(firstError->message.find("test stage worker terminated: boom"), std::string::npos);
    EXPECT_TRUE(stopSource.stop_requested());
}

TEST(StageGuardTest, ExistingErrorIsNotOverwritten) {
    std::stop_source stopSource;
    std::optional<Error> firstError = Error{ErrorCode::kUsageError, "first"};

    runGuarded("test stage", stopSource, firstError, [] { throw std::runtime_error("second"); });

    ASSERT_TRUE(firstError.has_value());
    EXPECT_EQ(firstError->code, ErrorCode::kUsageError);
    EXPECT_EQ(firstError->message, "first");
    EXPECT_TRUE(stopSource.stop_requested());
}

TEST(StageGuardTest, LockedVariantRecordsError) {
    std::stop_source stopSource;
    std::mutex errorMutex;
    std::optional<Error> firstError;

    runGuarded("test stage", stopSource, firstError, errorMutex, [] {
        throw std::runtime_error("locked boom");
    });

    ASSERT_TRUE(firstError.has_value());
    EXPECT_EQ(firstError->code, ErrorCode::kInternalError);
    EXPECT_TRUE(stopSource.stop_requested());
}

TEST(StageGuardTest, LockedVariantKeepsFirstError) {
    std::stop_source stopSource;
    std::mutex errorMutex;
    std::optional<Error> firstError = Error{ErrorCode::kUsageError, "first"};

    runGuarded("test stage", stopSource, firstError, errorMutex, [] {
        throw std::runtime_error("second");
    });

    ASSERT_TRUE(firstError.has_value());
    EXPECT_EQ(firstError->code, ErrorCode::kUsageError);
    EXPECT_EQ(firstError->message, "first");
    EXPECT_TRUE(stopSource.stop_requested());
}
