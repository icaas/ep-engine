/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
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

#include "dcp/dcpconnmap.h"
#include "evp_store_test.h"
#include "evp_store_single_threaded_test.h"
#include "fakes/fake_executorpool.h"
#include "makestoreddockey.h"
#include "taskqueue.h"
#include "../mock/mock_dcp_producer.h"
#include "../mock/mock_dcp_consumer.h"
#include "../mock/mock_stream.h"
#include "programs/engine_testapp/mock_server.h"
#include <string_utilities.h>
#include <xattr/blob.h>
#include <xattr/utils.h>

#include <thread>

ProcessClock::time_point SingleThreadedEPStoreTest::runNextTask(
        TaskQueue& taskQ, const std::string& expectedTaskName) {
    CheckedExecutor executor(task_executor, taskQ);

    // Run the task
    executor.runCurrentTask(expectedTaskName);
    return executor.completeCurrentTask();
}

ProcessClock::time_point SingleThreadedEPStoreTest::runNextTask(TaskQueue& taskQ) {
    CheckedExecutor executor(task_executor, taskQ);

    // Run the task
    executor.runCurrentTask();
    return executor.completeCurrentTask();
}

void SingleThreadedEPStoreTest::SetUp() {
    SingleThreadedExecutorPool::replaceExecutorPoolWithFake();
    EPBucketTest::SetUp();

    task_executor = reinterpret_cast<SingleThreadedExecutorPool*>
    (ExecutorPool::get());
}

void SingleThreadedEPStoreTest::TearDown() {
    shutdownAndPurgeTasks();
    EPBucketTest::TearDown();
}

void SingleThreadedEPStoreTest::setVBucketStateAndRunPersistTask(uint16_t vbid,
                                                                 vbucket_state_t
                                                                 newState) {
    // Change state - this should add 1 set_vbucket_state op to the
    //VBuckets' persistence queue.
    EXPECT_EQ(ENGINE_SUCCESS,
              store->setVBucketState(vbid, newState, /*transfer*/false));

    // Trigger the flusher to flush state to disk.
    EXPECT_EQ(0, store->flushVBucket(vbid));
}

void SingleThreadedEPStoreTest::shutdownAndPurgeTasks() {
    engine->getEpStats().isShutdown = true;
    task_executor->cancelAndClearAll();

    for (task_type_t t :
         {WRITER_TASK_IDX, READER_TASK_IDX, AUXIO_TASK_IDX, NONIO_TASK_IDX}) {

        // Define a lambda to drive all tasks from the queue, if hpTaskQ
        // is implemented then trivial to add a second call to runTasks.
        auto runTasks = [=](TaskQueue& queue) {
            while (queue.getFutureQueueSize() > 0 || queue.getReadyQueueSize() > 0) {
                runNextTask(queue);
            }
        };
        runTasks(*task_executor->getLpTaskQ()[t]);
        task_executor->stopTaskGroup(engine->getTaskable().getGID(), t,
                                     engine->getEpStats().forceShutdown);
    }
}

void SingleThreadedEPStoreTest::cancelAndPurgeTasks() {
    task_executor->cancelAll();
    for (task_type_t t :
        {WRITER_TASK_IDX, READER_TASK_IDX, AUXIO_TASK_IDX, NONIO_TASK_IDX}) {

        // Define a lambda to drive all tasks from the queue, if hpTaskQ
        // is implemented then trivial to add a second call to runTasks.
        auto runTasks = [=](TaskQueue& queue) {
            while (queue.getFutureQueueSize() > 0 || queue.getReadyQueueSize() > 0) {
                runNextTask(queue);
            }
        };
        runTasks(*task_executor->getLpTaskQ()[t]);
        task_executor->stopTaskGroup(engine->getTaskable().getGID(), t,
                                     engine->getEpStats().forceShutdown);
    }
}


/**
 * Regression test for MB-22451: When handleSlowStream is called and in
 * STREAM_BACKFILLING state and currently have a backfill scheduled (or running)
 * ensure that when the backfill completes the new backfill is scheduled and
 * the backfilling flag remains true.
 */
TEST_F(SingleThreadedEPStoreTest, test_mb22451) {
    // Make vbucket active.
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);
    // Store a single Item
    store_item(vbid, makeStoredDocKey("key"), "value");
    // Ensure that it has persisted to disk
    flush_vbucket_to_disk(vbid);

    // Create a Mock Dcp producer
    dcp_producer_t producer = new MockDcpProducer(*engine,
                                                  cookie,
                                                  "test_producer",
                                                  /*notifyOnly*/false);
    // Create a Mock Active Stream
    stream_t stream = new MockActiveStream(
            static_cast<EventuallyPersistentEngine*>(engine.get()),
            producer,
            producer->getName(),
            /*flags*/0,
            /*opaque*/0, vbid,
            /*st_seqno*/0,
            /*en_seqno*/~0,
            /*vb_uuid*/0xabcd,
            /*snap_start_seqno*/0,
            /*snap_end_seqno*/~0);

    MockActiveStream* mock_stream =
            static_cast<MockActiveStream*>(stream.get());

    /**
      * The core of the test follows:
      * Call completeBackfill whilst we are in the state of STREAM_BACKFILLING
      * and the pendingBackfill flag is set to true.
      * We expect that on leaving completeBackfill the isBackfillRunning flag is
      * set to true.
      */
    mock_stream->public_setBackfillTaskRunning(true);
    mock_stream->public_transitionState(STREAM_BACKFILLING);
    mock_stream->handleSlowStream();
    // The call to handleSlowStream should result in setting pendingBackfill
    // flag to true
    EXPECT_TRUE(mock_stream->public_getPendingBackfill())
        << "pendingBackfill is not true";
    mock_stream->completeBackfill();
    EXPECT_TRUE(mock_stream->public_isBackfillTaskRunning())
        << "isBackfillRunning is not true";

    // Required to ensure that the backfillMgr is deleted
    producer->closeAllStreams();
}

/* Regression / reproducer test for MB-19695 - an exception is thrown
 * (and connection disconnected) if a couchstore file hasn't been re-created
 * yet when doTapVbTakeoverStats() is called as part of
 * tapNotify / TAP_OPAQUE_INITIAL_VBUCKET_STREAM.
 */
TEST_F(SingleThreadedEPStoreTest, MB19695_doTapVbTakeoverStats) {
    auto* task_executor = reinterpret_cast<SingleThreadedExecutorPool*>
        (ExecutorPool::get());

    // Should start with no tasks registered on any queues.
    for (auto& queue : task_executor->getLpTaskQ()) {
        ASSERT_EQ(0, queue->getFutureQueueSize());
        ASSERT_EQ(0, queue->getReadyQueueSize());
    }

    // [[1] Set our state to replica.
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_replica);

    auto& lpWriterQ = *task_executor->getLpTaskQ()[WRITER_TASK_IDX];
    auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];

    // [[2]] Perform a vbucket reset. This will perform some work synchronously,
    // but also created 2 tasks and notifies the flusher:
    //   1. vbucket memory deletion (NONIO)
    //   2. vbucket disk deletion (WRITER)
    //   3. FlusherTask notified (WRITER)
    // MB-19695: If we try to get the number of persisted deletes between
    // steps (2) and (3) running then an exception is thrown (and client
    // disconnected).
    EXPECT_TRUE(store->resetVBucket(vbid));

    runNextTask(lpNonioQ, "Removing (dead) vb:0 from memory");
    runNextTask(lpWriterQ, "Deleting VBucket:0");

    // [[2]] Ok, let's see if we can get TAP takeover stats. This will
    // fail with MB-19695.
    // Dummy callback to pass into the stats function below.
    auto dummy_cb = [](const char *key, const uint16_t klen,
                          const char *val, const uint32_t vlen,
                          const void *cookie) {};
    std::string key{"MB19695_doTapVbTakeoverStats"};
    EXPECT_NO_THROW(engine->public_doTapVbTakeoverStats
                    (nullptr, dummy_cb, key, vbid));

    // Also check DCP variant (MB-19815)
    EXPECT_NO_THROW(engine->public_doDcpVbTakeoverStats
                    (nullptr, dummy_cb, key, vbid));

    // Cleanup - run flusher.
    EXPECT_EQ(0, store->flushVBucket(vbid));
}

/*
 * Test that
 * 1. We cannot create a stream against a dead vb (MB-17230)
 * 2. No tasks are scheduled as a side-effect of the streamRequest attempt.
 */
TEST_F(SingleThreadedEPStoreTest, MB19428_no_streams_against_dead_vbucket) {
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    store_item(vbid, makeStoredDocKey("key"), "value");

    // Directly flush the vbucket
    EXPECT_EQ(1, store->flushVBucket(vbid));

    setVBucketStateAndRunPersistTask(vbid, vbucket_state_dead);
    auto& lpAuxioQ = *task_executor->getLpTaskQ()[AUXIO_TASK_IDX];

    {
        // Create a Mock Dcp producer
        dcp_producer_t producer = new MockDcpProducer(*engine,
                                                      cookie,
                                                      "test_producer",
                                                      /*notifyOnly*/false);

        // Creating a producer will schedule one ActiveStreamCheckpointProcessorTask
        // that task though sleeps forever, so won't run until woken.
        EXPECT_EQ(1, lpAuxioQ.getFutureQueueSize());

        uint64_t rollbackSeqno;
        auto err = producer->streamRequest(/*flags*/0,
                                           /*opaque*/0,
                                           /*vbucket*/vbid,
                                           /*start_seqno*/0,
                                           /*end_seqno*/-1,
                                           /*vb_uuid*/0xabcd,
                                           /*snap_start*/0,
                                           /*snap_end*/0,
                                           &rollbackSeqno,
                                           SingleThreadedEPStoreTest::fakeDcpAddFailoverLog);

        EXPECT_EQ(ENGINE_NOT_MY_VBUCKET, err) << "Unexpected error code";

        // The streamRequest failed and should not of created anymore tasks.
        EXPECT_EQ(1, lpAuxioQ.getFutureQueueSize());
    }
}

/*
 * Test that TaskQueue::wake results in a sensible ExecutorPool work count
 * Incorrect counting can result in the run loop spinning for many threads.
 */
TEST_F(SingleThreadedEPStoreTest, MB20235_wake_and_work_count) {
    class TestTask : public GlobalTask {
    public:
        TestTask(EventuallyPersistentEngine *e, double s) :
                 GlobalTask(e, TaskId::ActiveStreamCheckpointProcessorTask, s) {}
        bool run() {
            return false;
        }

        std::string getDescription() {
            return "Test MB20235";
        }
    };

    auto& lpAuxioQ = *task_executor->getLpTaskQ()[AUXIO_TASK_IDX];

    // New task with a massive sleep
    ExTask task = new TestTask(engine.get(), 99999.0);
    EXPECT_EQ(0, lpAuxioQ.getFutureQueueSize());

    // schedule the task, futureQueue grows
    task_executor->schedule(task, AUXIO_TASK_IDX);
    EXPECT_EQ(lpAuxioQ.getReadyQueueSize(), task_executor->getTotReadyTasks());
    EXPECT_EQ(lpAuxioQ.getReadyQueueSize(),
              task_executor->getNumReadyTasks(AUXIO_TASK_IDX));
    EXPECT_EQ(1, lpAuxioQ.getFutureQueueSize());

    // Wake task, but stays in futureQueue (fetch can now move it)
    task_executor->wake(task->getId());
    EXPECT_EQ(lpAuxioQ.getReadyQueueSize(), task_executor->getTotReadyTasks());
    EXPECT_EQ(lpAuxioQ.getReadyQueueSize(),
              task_executor->getNumReadyTasks(AUXIO_TASK_IDX));
    EXPECT_EQ(1, lpAuxioQ.getFutureQueueSize());
    EXPECT_EQ(0, lpAuxioQ.getReadyQueueSize());

    runNextTask(lpAuxioQ);
    EXPECT_EQ(lpAuxioQ.getReadyQueueSize(), task_executor->getTotReadyTasks());
    EXPECT_EQ(lpAuxioQ.getReadyQueueSize(),
              task_executor->getNumReadyTasks(AUXIO_TASK_IDX));
    EXPECT_EQ(0, lpAuxioQ.getFutureQueueSize());
    EXPECT_EQ(0, lpAuxioQ.getReadyQueueSize());
}

// Check that in-progress disk backfills (`CouchKVStore::backfill`) are
// correctly deleted when we delete a bucket. If not then we leak vBucket file
// descriptors, which can prevent ns_server from cleaning up old vBucket files
// and consequently re-adding a node to the cluster.
//
TEST_F(SingleThreadedEPStoreTest, MB19892_BackfillNotDeleted) {
    // Make vbucket active.
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    // Perform one SET, then close it's checkpoint. This means that we no
    // longer have all sequence numbers in memory checkpoints, forcing the
    // DCP stream request to go to disk (backfill).
    store_item(vbid, makeStoredDocKey("key"), "value");

    // Force a new checkpoint.
    auto vb = store->getVbMap().getBucket(vbid);
    auto& ckpt_mgr = vb->checkpointManager;
    ckpt_mgr.createNewCheckpoint();

    // Directly flush the vbucket, ensuring data is on disk.
    //  (This would normally also wake up the checkpoint remover task, but
    //   as that task was never registered with the ExecutorPool in this test
    //   environment, we need to manually remove the prev checkpoint).
    EXPECT_EQ(1, store->flushVBucket(vbid));

    bool new_ckpt_created;
    EXPECT_EQ(1, ckpt_mgr.removeClosedUnrefCheckpoints(*vb, new_ckpt_created));

    // Create a DCP producer, and start a stream request.
    std::string name{"test_producer"};
    EXPECT_EQ(ENGINE_SUCCESS,
              engine->dcpOpen(cookie, /*opaque:unused*/{}, /*seqno:unused*/{},
                              DCP_OPEN_PRODUCER, name.data(), name.size()));

    uint64_t rollbackSeqno;
    auto dummy_dcp_add_failover_cb = [](vbucket_failover_t* entry,
                                       size_t nentries, const void *cookie) {
        return ENGINE_SUCCESS;
    };

    // Actual stream request method (EvpDcpStreamReq) is static, so access via
    // the engine_interface.
    EXPECT_EQ(ENGINE_SUCCESS,
              engine.get()->dcp.stream_req(
                      &engine.get()->interface, cookie, /*flags*/0,
                      /*opaque*/0, /*vbucket*/vbid, /*start_seqno*/0,
                      /*end_seqno*/-1, /*vb_uuid*/0xabcd, /*snap_start*/0,
                      /*snap_end*/0, &rollbackSeqno,
                      dummy_dcp_add_failover_cb));
}

/*
 * Test that the DCP processor returns a 'yield' return code when
 * working on a large enough buffer size.
 */
TEST_F(SingleThreadedEPStoreTest, MB18452_yield_dcp_processor) {

    // We need a replica VB
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_replica);

    // Create a MockDcpConsumer
    dcp_consumer_t consumer = new MockDcpConsumer(*engine, cookie, "test");

    // Add the stream
    EXPECT_EQ(ENGINE_SUCCESS,
              consumer->addStream(/*opaque*/0, vbid, /*flags*/0));

    // The processBufferedItems should yield every "yield * batchSize"
    // So add '(n * (yield * batchSize)) + 1' messages and we should see
    // processBufferedMessages return 'more_to_process' 'n' times and then
    // 'all_processed' once.
    const int n = 4;
    const int yield = engine->getConfiguration().getDcpConsumerProcessBufferedMessagesYieldLimit();
    const int batchSize = engine->getConfiguration().getDcpConsumerProcessBufferedMessagesBatchSize();
    const int messages = n * (batchSize * yield);

    // Force the stream to buffer rather than process messages immediately
    const ssize_t queueCap = engine->getEpStats().replicationThrottleWriteQueueCap;
    engine->getEpStats().replicationThrottleWriteQueueCap = 0;

    // 1. Add the first message, a snapshot marker.
    consumer->snapshotMarker(/*opaque*/1, vbid, /*startseq*/0,
                             /*endseq*/messages, /*flags*/0);

    // 2. Now add the rest as mutations.
    for (int ii = 0; ii <= messages; ii++) {
        const std::string key = "key" + std::to_string(ii);
        const DocKey docKey{key, DocNamespace::DefaultCollection};
        std::string value = "value";

        consumer->mutation(1/*opaque*/,
                           docKey,
                           {(const uint8_t*)value.c_str(), value.length()},
                           0, // privileged bytes
                           PROTOCOL_BINARY_RAW_BYTES, // datatype
                           0, // cas
                           vbid, // vbucket
                           0, // flags
                           ii, // bySeqno
                           0, // revSeqno
                           0, // exptime
                           0, // locktime
                           {}, // meta
                           0); // nru
    }

    // Set the throttle back to the original value
    engine->getEpStats().replicationThrottleWriteQueueCap = queueCap;

    // Get our target stream ready.
    static_cast<MockDcpConsumer*>(consumer.get())->public_notifyVbucketReady(vbid);

    // 3. processBufferedItems returns more_to_process n times
    for (int ii = 0; ii < n; ii++) {
        EXPECT_EQ(more_to_process, consumer->processBufferedItems());
    }

    // 4. processBufferedItems returns a final all_processed
    EXPECT_EQ(all_processed, consumer->processBufferedItems());

    // Drop the stream
    consumer->closeStream(/*opaque*/0, vbid);
}

/*
 * Background thread used by MB20054_onDeleteItem_during_bucket_deletion
 */
static void MB20054_run_backfill_task(EventuallyPersistentEngine* engine,
                                      CheckedExecutor& backfill,
                                      SyncObject& backfill_cv,
                                      SyncObject& destroy_cv,
                                      TaskQueue* lpAuxioQ) {
    std::unique_lock<std::mutex> destroy_lh(destroy_cv);
    ObjectRegistry::onSwitchThread(engine);

    // Run the BackfillManagerTask task to push items to readyQ. In sherlock
    // upwards this runs multiple times - so should return true.
    backfill.runCurrentTask("Backfilling items for a DCP Connection");

    // Notify the main thread that it can progress with destroying the
    // engine [A].
    {
        // if we can get the lock, then we know the main thread is waiting
        std::lock_guard<std::mutex> backfill_lock(backfill_cv);
        backfill_cv.notify_one(); // move the main thread along
    }

    // Now wait ourselves for destroy to be completed [B].
    destroy_cv.wait(destroy_lh);

    // This is the only "hacky" part of the test - we need to somehow
    // keep the DCPBackfill task 'running' - i.e. not call
    // completeCurrentTask - until the main thread is in
    // ExecutorPool::_stopTaskGroup. However we have no way from the test
    // to properly signal that we are *inside* _stopTaskGroup -
    // called from EVPStore's destructor.
    // Best we can do is spin on waiting for the DCPBackfill task to be
    // set to 'dead' - and only then completeCurrentTask; which will
    // cancel the task.
    while (!backfill.getCurrentTask()->isdead()) {
        // spin.
    }
    backfill.completeCurrentTask();
}

static ENGINE_ERROR_CODE dummy_dcp_add_failover_cb(vbucket_failover_t* entry,
                                                   size_t nentries,
                                                   const void *cookie) {
    return ENGINE_SUCCESS;
}

// Test performs engine deletion interleaved with tasks so redefine TearDown
// for this tests needs.
class MB20054_SingleThreadedEPStoreTest : public SingleThreadedEPStoreTest {
public:
    void SetUp() {
        SingleThreadedEPStoreTest::SetUp();
        engine->initializeConnmaps();
    }

    void TearDown() {
        ExecutorPool::shutdown();
    }
};

// Check that if onDeleteItem() is called during bucket deletion, we do not
// abort due to not having a valid thread-local 'engine' pointer. This
// has been observed when we have a DCPBackfill task which is deleted during
// bucket shutdown, which has a non-zero number of Items which are destructed
// (and call onDeleteItem).
TEST_F(MB20054_SingleThreadedEPStoreTest, MB20054_onDeleteItem_during_bucket_deletion) {

    // [[1] Set our state to active.
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    // Perform one SET, then close it's checkpoint. This means that we no
    // longer have all sequence numbers in memory checkpoints, forcing the
    // DCP stream request to go to disk (backfill).
    store_item(vbid, makeStoredDocKey("key"), "value");

    // Force a new checkpoint.
    RCPtr<VBucket> vb = store->getVbMap().getBucket(vbid);
    CheckpointManager& ckpt_mgr = vb->checkpointManager;
    ckpt_mgr.createNewCheckpoint();
    auto lpWriterQ = task_executor->getLpTaskQ()[WRITER_TASK_IDX];
    EXPECT_EQ(0, lpWriterQ->getFutureQueueSize());
    EXPECT_EQ(0, lpWriterQ->getReadyQueueSize());

    auto lpAuxioQ = task_executor->getLpTaskQ()[AUXIO_TASK_IDX];
    EXPECT_EQ(0, lpAuxioQ->getFutureQueueSize());
    EXPECT_EQ(0, lpAuxioQ->getReadyQueueSize());

    // Directly flush the vbucket, ensuring data is on disk.
    //  (This would normally also wake up the checkpoint remover task, but
    //   as that task was never registered with the ExecutorPool in this test
    //   environment, we need to manually remove the prev checkpoint).
    EXPECT_EQ(1, store->flushVBucket(vbid));

    bool new_ckpt_created;
    EXPECT_EQ(1, ckpt_mgr.removeClosedUnrefCheckpoints(*vb, new_ckpt_created));
    vb.reset();

    EXPECT_EQ(0, lpAuxioQ->getFutureQueueSize());
    EXPECT_EQ(0, lpAuxioQ->getReadyQueueSize());

    // Create a DCP producer, and start a stream request.
    std::string name("test_producer");
    EXPECT_EQ(ENGINE_SUCCESS,
              engine->dcpOpen(cookie, /*opaque:unused*/{}, /*seqno:unused*/{},
                              DCP_OPEN_PRODUCER, name.data(), name.size()));

    // Expect to have an ActiveStreamCheckpointProcessorTask, which is
    // initially snoozed (so we can't run it).
    EXPECT_EQ(1, lpAuxioQ->getFutureQueueSize());
    EXPECT_EQ(0, lpAuxioQ->getReadyQueueSize());

    uint64_t rollbackSeqno;
    // Actual stream request method (EvpDcpStreamReq) is static, so access via
    // the engine_interface.
    EXPECT_EQ(ENGINE_SUCCESS,
              engine->dcp.stream_req(&engine->interface, cookie, /*flags*/0,
                                     /*opaque*/0, /*vbucket*/vbid,
                                     /*start_seqno*/0, /*end_seqno*/-1,
                                     /*vb_uuid*/0xabcd, /*snap_start*/0,
                                     /*snap_end*/0, &rollbackSeqno,
                                     dummy_dcp_add_failover_cb));

    // FutureQ should now have an additional DCPBackfill task.
    EXPECT_EQ(2, lpAuxioQ->getFutureQueueSize());
    EXPECT_EQ(0, lpAuxioQ->getReadyQueueSize());

    // Create an executor 'thread' to obtain shared ownership of the next
    // AuxIO task (which should be BackfillManagerTask). As long as this
    // object has it's currentTask set to BackfillManagerTask, the task
    // will not be deleted.
    // Essentially we are simulating a concurrent thread running this task.
    CheckedExecutor backfill(task_executor, *lpAuxioQ);

    // This is the one action we really need to perform 'concurrently' - delete
    // the engine while a DCPBackfill task is still running. We spin up a
    // separate thread which will run the DCPBackfill task
    // concurrently with destroy - specifically DCPBackfill must start running
    // (and add items to the readyQ) before destroy(), it must then continue
    // running (stop after) _stopTaskGroup is invoked.
    // To achieve this we use a couple of condition variables to synchronise
    // between the two threads - the timeline needs to look like:
    //
    //  auxIO thread:  [------- DCPBackfill ----------]
    //   main thread:          [destroy()]       [ExecutorPool::_stopTaskGroup]
    //
    //  --------------------------------------------------------> time
    //
    SyncObject backfill_cv;
    SyncObject destroy_cv;
    std::thread concurrent_task_thread;

    {
        // scope for the backfill lock
        std::unique_lock<std::mutex> backfill_lh(backfill_cv);

        concurrent_task_thread = std::thread(MB20054_run_backfill_task,
                                             engine.get(),
                                             std::ref(backfill),
                                             std::ref(backfill_cv),
                                             std::ref(destroy_cv),
                                             lpAuxioQ);
        // [A] Wait for DCPBackfill to complete.
        backfill_cv.wait(backfill_lh);
    }

    ObjectRegistry::onSwitchThread(engine.get());
    // 'Destroy' the engine - this doesn't delete the object, just shuts down
    // connections, marks streams as dead etc.
    engine->destroy(/*force*/false);

    {
        // If we can get the lock we know the thread is waiting for destroy.
        std::lock_guard<std::mutex> lh(destroy_cv);
        destroy_cv.notify_one(); // move the thread on.
    }

    // Force all tasks to cancel (so we can shutdown)
    cancelAndPurgeTasks();

    // Mark the connection as dead for clean shutdown
    destroy_mock_cookie(cookie);
    engine->getDcpConnMap().manageConnections();

    // Nullify TLS engine and reset the smart pointer to force destruction.
    // We need null as the engine to stop ~CheckedExecutor path from trying
    // to touch the engine
    ObjectRegistry::onSwitchThread(nullptr);
    engine.reset();
    destroy_mock_event_callbacks();
    concurrent_task_thread.join();
}

/*
 * MB-18953 is triggered by the executorpool wake path moving tasks directly
 * into the readyQueue, thus allowing for high-priority tasks to dominiate
 * a taskqueue.
 */
TEST_F(SingleThreadedEPStoreTest, MB18953_taskWake) {
    auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];

    class TestTask : public GlobalTask {
    public:
        TestTask(EventuallyPersistentEngine* e, TaskId id)
          : GlobalTask(e, id, 0.0, false) {}

        // returning true will also drive the ExecutorPool::reschedule path.
        bool run() { return true; }

        std::string getDescription() {
            return std::string("TestTask ") + GlobalTask::getTaskName(getTypeId());
        }
    };

    ExTask hpTask = new TestTask(engine.get(),
                                 TaskId::PendingOpsNotification);
    task_executor->schedule(hpTask, NONIO_TASK_IDX);

    ExTask lpTask = new TestTask(engine.get(),
                                 TaskId::DefragmenterTask);
    task_executor->schedule(lpTask, NONIO_TASK_IDX);

    runNextTask(lpNonioQ, "TestTask PendingOpsNotification"); // hptask goes first
    // Ensure that a wake to the hpTask doesn't mean the lpTask gets ignored
    lpNonioQ.wake(hpTask);

    // Check 1 task is ready
    EXPECT_EQ(1, task_executor->getTotReadyTasks());
    EXPECT_EQ(1, task_executor->getNumReadyTasks(NONIO_TASK_IDX));

    runNextTask(lpNonioQ, "TestTask DefragmenterTask"); // lptask goes second

    // Run the tasks again to check that coming from ::reschedule our
    // expectations are still met.
    runNextTask(lpNonioQ, "TestTask PendingOpsNotification"); // hptask goes first

    // Ensure that a wake to the hpTask doesn't mean the lpTask gets ignored
    lpNonioQ.wake(hpTask);

    // Check 1 task is ready
    EXPECT_EQ(1, task_executor->getTotReadyTasks());
    EXPECT_EQ(1, task_executor->getNumReadyTasks(NONIO_TASK_IDX));
    runNextTask(lpNonioQ, "TestTask DefragmenterTask"); // lptask goes second
}

/*
 * MB-20735 waketime is not correctly picked up on reschedule
 */
TEST_F(SingleThreadedEPStoreTest, MB20735_rescheduleWaketime) {
    auto& lpNonioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];

    class TestTask : public GlobalTask {
    public:
        TestTask(EventuallyPersistentEngine* e, TaskId id)
          : GlobalTask(e, id, 0.0, false) {}

        bool run() {
            snooze(0.1); // snooze for 100milliseconds only
            // Rescheduled to run 100 milliseconds later..
            return true;
        }

        std::string getDescription() {
            return std::string("TestTask ") + GlobalTask::getTaskName(getTypeId());
        }
    };

    TestTask *task = new TestTask(engine.get(),
                                 TaskId::PendingOpsNotification);
    ExTask hpTask = task;
    task_executor->schedule(hpTask, NONIO_TASK_IDX);

    ProcessClock::time_point waketime = runNextTask(lpNonioQ,
                                    "TestTask PendingOpsNotification");
    EXPECT_EQ(waketime, task->getWaketime()) <<
                           "Rescheduled to much later time!";
}

/*
 * Tests that we stream from only active vbuckets for DCP clients with that
 * preference
 */
TEST_F(SingleThreadedEPStoreTest, stream_from_active_vbucket_only) {
    std::map<vbucket_state_t, bool> states;
    states[vbucket_state_active] = true; /* Positive test case */
    states[vbucket_state_replica] = false; /* Negative test case */
    states[vbucket_state_pending] = false; /* Negative test case */
    states[vbucket_state_dead] = false; /* Negative test case */

    for (auto& it : states) {
        setVBucketStateAndRunPersistTask(vbid, it.first);

        /* Create a Mock Dcp producer */
        dcp_producer_t producer = new MockDcpProducer(*engine,
                                                      cookie,
                                                      "test_producer",
                                                      /*notifyOnly*/false);

        /* Try to open stream on replica vb with
           DCP_ADD_STREAM_ACTIVE_VB_ONLY flag */
        uint64_t rollbackSeqno;
        auto err = producer->streamRequest(/*flags*/
                                           DCP_ADD_STREAM_ACTIVE_VB_ONLY,
                                           /*opaque*/0,
                                           /*vbucket*/vbid,
                                           /*start_seqno*/0,
                                           /*end_seqno*/-1,
                                           /*vb_uuid*/0xabcd,
                                           /*snap_start*/0,
                                           /*snap_end*/0,
                                           &rollbackSeqno,
                                           SingleThreadedEPStoreTest::fakeDcpAddFailoverLog);

        if (it.second) {
            EXPECT_EQ(ENGINE_SUCCESS, err) << "Unexpected error code";
            producer->closeStream(/*opaque*/0, /*vbucket*/vbid);
        } else {
            EXPECT_EQ(ENGINE_NOT_MY_VBUCKET, err) << "Unexpected error code";
        }
    }
}

TEST_F(SingleThreadedEPStoreTest, pre_expiry_xattrs) {
    auto& kvbucket = *engine->getKVBucket();

    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    cb::xattr::Blob blob;

    //Add a few values
    blob.set(to_const_byte_buffer("user"),
             to_const_byte_buffer("{\"author\":\"bubba\"}"));
    blob.set(to_const_byte_buffer("_sync"),
             to_const_byte_buffer("{\"cas\":\"0xdeadbeefcafefeed\"}"));
    blob.set(to_const_byte_buffer("meta"),
             to_const_byte_buffer("{\"content-type\":\"text\"}"));

    auto xattr_value = blob.finalize();

    auto xattr_data = to_string(xattr_value);

    auto itm = store_item(vbid, makeStoredDocKey("key"), xattr_data, 1,
                          PROTOCOL_BINARY_DATATYPE_XATTR);

    ItemMetaData metadata;
    uint32_t deleted;
    kvbucket.getMetaData(makeStoredDocKey("key"), vbid, nullptr, metadata,
                         deleted);
    auto prev_revseqno = metadata.revSeqno;
    EXPECT_EQ(1, prev_revseqno) << "Unexpected revision sequence number";

    kvbucket.deleteExpiredItem(vbid, makeStoredDocKey("key"),
                               ep_real_time() + 1, 1, ExpireBy::Pager);
    get_options_t options = static_cast<get_options_t>(QUEUE_BG_FETCH |
                                                       HONOR_STATES |
                                                       TRACK_REFERENCE |
                                                       DELETE_TEMP |
                                                       HIDE_LOCKED_CAS |
                                                       TRACK_STATISTICS |
                                                       GET_DELETED_VALUE);
    GetValue gv = kvbucket.get(makeStoredDocKey("key"), vbid, cookie, options);
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());

    auto get_itm = gv.getValue();
    auto get_data = const_cast<char*>(get_itm->getData());

    cb::byte_buffer value_buf{reinterpret_cast<uint8_t*>(get_data),
                              get_itm->getNBytes()};
    cb::xattr::Blob new_blob(value_buf);

    const std::string& cas_str{"{\"cas\":\"0xdeadbeefcafefeed\"}"};
    const std::string& sync_str = to_string(blob.get(to_const_byte_buffer("_sync")));

    EXPECT_EQ(cas_str, sync_str) << "Unexpected system xattrs";

    kvbucket.getMetaData(makeStoredDocKey("key"), vbid, nullptr, metadata,
                         deleted);
    EXPECT_EQ(prev_revseqno + 1, metadata.revSeqno) <<
             "Unexpected revision sequence number";

    delete get_itm;
}
