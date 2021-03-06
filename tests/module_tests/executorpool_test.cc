/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/*
 * Unit tests for the ExecutorPool class
 */

#include "executorpool_test.h"

class LambdaTask : public GlobalTask {
public:
    LambdaTask(Taskable& t,
               TaskId taskId,
               double sleeptime,
               bool completeBeforeShutdown,
               std::function<bool()> f)
        : GlobalTask(t, taskId, sleeptime, completeBeforeShutdown), func(f) {
    }

    bool run() override {
        return func();
    }

    std::string getDescription(void) override {
        return "Lambda Task";
    }

protected:
    std::function<bool()> func;
};

MockTaskable::MockTaskable() : policy(HIGH_BUCKET_PRIORITY, 1) {
}

const std::string& MockTaskable::getName() const {
    return name;
}

task_gid_t MockTaskable::getGID() const {
    return 0;
}

bucket_priority_t MockTaskable::getWorkloadPriority() const {
    return HIGH_BUCKET_PRIORITY;
}

void MockTaskable::setWorkloadPriority(bucket_priority_t prio) {
}

WorkLoadPolicy& MockTaskable::getWorkLoadPolicy(void) {
    return policy;
}

void MockTaskable::logQTime(TaskId id, const ProcessClock::duration enqTime) {
}

void MockTaskable::logRunTime(TaskId id, const ProcessClock::duration runTime) {
}

ExTask makeTask(Taskable& taskable, ThreadGate& tg, size_t i) {
    return new LambdaTask(taskable, TaskId::StatSnap, 0, true, [&]() -> bool {
        tg.threadUp();
        return false;
    });
}

TEST_F(ExecutorPoolTest, register_taskable_test) {
    TestExecutorPool pool(10, // MaxThreads
                          NUM_TASK_GROUPS,
                          2, // MaxNumReaders
                          2, // MaxNumWriters
                          2, // MaxNumAuxio
                          2 // MaxNumNonio
                          );

    MockTaskable taskable;
    MockTaskable taskable2;

    ASSERT_EQ(0, pool.getNumWorkersStat());
    ASSERT_EQ(0, pool.getNumBuckets());

    pool.registerTaskable(taskable);

    ASSERT_EQ(8, pool.getNumWorkersStat());
    ASSERT_EQ(1, pool.getNumBuckets());

    pool.registerTaskable(taskable2);

    ASSERT_EQ(8, pool.getNumWorkersStat());
    ASSERT_EQ(2, pool.getNumBuckets());

    pool.unregisterTaskable(taskable2, false);

    ASSERT_EQ(8, pool.getNumWorkersStat());
    ASSERT_EQ(1, pool.getNumBuckets());

    pool.unregisterTaskable(taskable, false);

    ASSERT_EQ(0, pool.getNumWorkersStat());
    ASSERT_EQ(0, pool.getNumBuckets());
}

TEST_F(ExecutorPoolDynamicWorkerTest, decrease_workers) {
    EXPECT_EQ(2, pool->getNumWriters());
    /* Will take ~2s (MIN_SLEEP_TIME while the thread being removed sleeps
     * (having found no work) and we wait to join it.
     */
    pool->setMaxWriters(1);
    EXPECT_EQ(1, pool->getNumWriters());
}

TEST_P(ExecutorPoolTestWithParam, max_threads_test_parameterized) {
    ExpectedThreadCounts expected = GetParam();

    MockTaskable taskable;

    TestExecutorPool pool(expected.maxThreads, // MaxThreads
                          NUM_TASK_GROUPS,
                          0, // MaxNumReaders (0 = use default)
                          0, // MaxNumWriters
                          0, // MaxNumAuxio
                          0 // MaxNumNonio
                          );

    pool.registerTaskable(taskable);

    EXPECT_EQ(expected.reader, pool.getNumReaders());
    EXPECT_EQ(expected.writer, pool.getNumWriters());
    EXPECT_EQ(expected.auxIO, pool.getNumAuxIO());
    EXPECT_EQ(expected.nonIO, pool.getNumNonIO());

    pool.unregisterTaskable(taskable, false);
    pool.shutdown();
}

std::vector<ExpectedThreadCounts> threadCountValues = {{1, 4, 4, 1, 2},
                                                       {2, 4, 4, 1, 2},
                                                       {4, 4, 4, 1, 2},
                                                       {8, 4, 4, 1, 2},
                                                       {10, 4, 4, 1, 3},
                                                       {14, 4, 4, 2, 4},
                                                       {20, 6, 4, 2, 6},
                                                       {24, 7, 4, 3, 7},
                                                       {32, 12, 4, 4, 8},
                                                       {48, 12, 4, 5, 8},
                                                       {64, 12, 4, 7, 8},
                                                       {128, 12, 4, 8, 8}};

INSTANTIATE_TEST_CASE_P(ThreadCountTest,
                        ExecutorPoolTestWithParam,
                        ::testing::ValuesIn(threadCountValues),
                        ::testing::PrintToStringParamName());