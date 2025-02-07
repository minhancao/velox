/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/exec/Exchange.h"
#include "velox/exec/OutputBufferManager.h"
#include "velox/exec/Task.h"
#include "velox/exec/tests/utils/LocalExchangeSource.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/QueryAssertions.h"
#include "velox/exec/tests/utils/SerializedPageUtil.h"
#include "velox/serializers/PrestoSerializer.h"
#include "velox/vector/tests/utils/VectorTestBase.h"

namespace facebook::velox::exec {

namespace {

class ExchangeClientTest : public testing::Test,
                           public velox::test::VectorTestBase {
 protected:
  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance({});
  }

  void SetUp() override {
    test::testingStartLocalExchangeSource();
    executor_ = std::make_unique<folly::CPUThreadPoolExecutor>(16);
    exec::ExchangeSource::factories().clear();
    exec::ExchangeSource::registerFactory(test::createLocalExchangeSource);
    if (!isRegisteredVectorSerde()) {
      velox::serializer::presto::PrestoVectorSerde::registerVectorSerde();
    }
    bufferManager_ = OutputBufferManager::getInstance().lock();

    common::testutil::TestValue::enable();
  }

  void TearDown() override {
    exec::test::waitForAllTasksToBeDeleted();
    test::testingShutdownLocalExchangeSource();
  }

  std::shared_ptr<Task> makeTask(
      const std::string& taskId,
      const std::optional<uint64_t> maxOutputBufferSizeInBytes = {}) {
    std::unordered_map<std::string, std::string> config;
    if (maxOutputBufferSizeInBytes.has_value()) {
      config[core::QueryConfig::kMaxOutputBufferSize] =
          std::to_string(maxOutputBufferSizeInBytes.value());
    }
    auto queryCtx = core::QueryCtx::create(
        executor_.get(), core::QueryConfig{std::move(config)});
    queryCtx->testingOverrideMemoryPool(
        memory::memoryManager()->addRootPool(queryCtx->queryId()));
    auto plan = test::PlanBuilder().values({}).planNode();
    return Task::create(
        taskId,
        core::PlanFragment{plan},
        0,
        std::move(queryCtx),
        Task::ExecutionMode::kParallel);
  }

  int32_t enqueue(
      const std::string& taskId,
      int32_t destination,
      const RowVectorPtr& data) {
    auto page = test::toSerializedPage(data, bufferManager_, pool());
    const auto pageSize = page->size();
    ContinueFuture unused;
    auto blocked =
        bufferManager_->enqueue(taskId, destination, std::move(page), &unused);
    VELOX_CHECK(!blocked);
    return pageSize;
  }

  std::vector<std::unique_ptr<SerializedPage>> fetchPages(
      ExchangeClient& client,
      int32_t numPages) {
    std::vector<std::unique_ptr<SerializedPage>> allPages;
    for (auto i = 0; i < numPages; ++i) {
      bool atEnd;
      ContinueFuture future;
      auto pages = client.next(1, &atEnd, &future);
      if (pages.empty()) {
        auto& exec = folly::QueuedImmediateExecutor::instance();
        std::move(future).via(&exec).wait();
        pages = client.next(1, &atEnd, &future);
      }
      EXPECT_EQ(1, pages.size());
      allPages.push_back(std::move(pages.at(0)));
    }
    return allPages;
  }

  static void addSources(ExchangeQueue& queue, int32_t numSources) {
    {
      std::lock_guard<std::mutex> l(queue.mutex());
      for (auto i = 0; i < numSources; ++i) {
        queue.addSourceLocked();
      }
    }
    queue.noMoreSources();
  }

  static void enqueue(
      ExchangeQueue& queue,
      std::unique_ptr<SerializedPage> page) {
    std::vector<ContinuePromise> promises;
    {
      std::lock_guard<std::mutex> l(queue.mutex());
      queue.enqueueLocked(std::move(page), promises);
    }
    for (auto& promise : promises) {
      promise.setValue();
    }
  }

  static std::unique_ptr<SerializedPage> makePage(uint64_t size) {
    auto ioBuf = folly::IOBuf::create(size);
    ioBuf->append(size);
    return std::make_unique<SerializedPage>(std::move(ioBuf), nullptr, 1);
  }

  folly::Executor* executor() const {
    return executor_.get();
  }

  std::unique_ptr<folly::CPUThreadPoolExecutor> executor_;
  std::shared_ptr<OutputBufferManager> bufferManager_;
};

TEST_F(ExchangeClientTest, nonVeloxCreateExchangeSourceException) {
  ExchangeSource::registerFactory(
      [](const auto& taskId, auto destination, auto queue, auto pool)
          -> std::shared_ptr<ExchangeSource> {
        throw std::runtime_error("Testing error");
      });

  auto client = std::make_shared<ExchangeClient>(
      "t", 1, ExchangeClient::kDefaultMaxQueuedBytes, pool(), executor());

  VELOX_ASSERT_THROW(
      client->addRemoteTaskId("task.1.2.3"),
      "Failed to create ExchangeSource: Testing error. Task ID: task.1.2.3.");

  // Test with a very long task ID. Make sure it is truncated.
  VELOX_ASSERT_THROW(
      client->addRemoteTaskId(std::string(1024, 'x')),
      "Failed to create ExchangeSource: Testing error. "
      "Task ID: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.");

  client->close();
}

TEST_F(ExchangeClientTest, stats) {
  auto data = {
      makeRowVector({makeFlatVector<int32_t>({1, 2, 3})}),
      makeRowVector({makeFlatVector<int32_t>({1, 2, 3, 4, 5})}),
      makeRowVector({makeFlatVector<int32_t>({1, 2})}),
  };

  auto taskId = "local://t1";
  auto task = makeTask(taskId);

  bufferManager_->initializeTask(
      task, core::PartitionedOutputNode::Kind::kPartitioned, 100, 16);

  auto client = std::make_shared<ExchangeClient>(
      "t", 17, ExchangeClient::kDefaultMaxQueuedBytes, pool(), executor());
  client->addRemoteTaskId(taskId);

  // Enqueue 3 pages.
  std::vector<uint64_t> pageBytes;
  uint64_t totalBytes = 0;
  for (auto vector : data) {
    const auto pageSize = enqueue(taskId, 17, vector);
    totalBytes += pageSize;
    pageBytes.push_back(pageSize);
  }

  fetchPages(*client, 3);

  auto stats = client->stats();
  // Since we run exchange source response callback in an executor, then we
  // might start to fetch from the client before all the source buffers are
  // enqueued.
  ASSERT_GE(totalBytes, stats.at("peakBytes").sum);
  ASSERT_EQ(data.size(), stats.at("numReceivedPages").sum);
  ASSERT_EQ(totalBytes / data.size(), stats.at("averageReceivedPageBytes").sum);

  task->requestCancel();
  bufferManager_->removeTask(taskId);

  client->close();
}

// Test scenario where fetching data from all sources at once would exceed queue
// size. Verify that ExchangeClient is fetching data only from a few sources at
// a time to avoid exceeding the limit.
TEST_F(ExchangeClientTest, flowControl) {
  auto data = makeRowVector({
      makeFlatVector<int64_t>(10'000, [](auto row) { return row; }),
  });

  auto page = test::toSerializedPage(data, bufferManager_, pool());

  // Set limit at 3.5 pages.
  auto client = std::make_shared<ExchangeClient>(
      "flow.control", 17, page->size() * 3.5, pool(), executor());

  // Make 10 tasks.
  std::vector<std::shared_ptr<Task>> tasks;
  for (auto i = 0; i < 10; ++i) {
    auto taskId = fmt::format("local://t{}", i);
    auto task = makeTask(taskId);

    bufferManager_->initializeTask(
        task, core::PartitionedOutputNode::Kind::kPartitioned, 100, 16);

    // Enqueue 3 pages.
    for (auto j = 0; j < 3; ++j) {
      enqueue(taskId, 17, data);
    }

    tasks.push_back(task);
    client->addRemoteTaskId(taskId);
  }

  fetchPages(*client, 3 * tasks.size());

  const auto stats = client->stats();
  EXPECT_LE(stats.at("peakBytes").sum, page->size() * 4);
  EXPECT_EQ(30, stats.at("numReceivedPages").sum);
  EXPECT_EQ(page->size(), stats.at("averageReceivedPageBytes").sum);

  for (auto& task : tasks) {
    task->requestCancel();
    bufferManager_->removeTask(task->taskId());
  }

  client->close();
}

TEST_F(ExchangeClientTest, largeSinglePage) {
  auto data = {
      makeRowVector({makeFlatVector<int64_t>(10000, folly::identity)}),
      makeRowVector({makeFlatVector<int64_t>(1, folly::identity)}),
  };
  auto client =
      std::make_shared<ExchangeClient>("test", 1, 1000, pool(), executor());
  auto task = makeTask("local://producer");
  bufferManager_->initializeTask(
      task, core::PartitionedOutputNode::Kind::kArbitrary, 1, 1);
  for (auto& batch : data) {
    enqueue(task->taskId(), 0, batch);
  }
  client->addRemoteTaskId(task->taskId());
  auto pages = fetchPages(*client, 1);
  ASSERT_EQ(pages.size(), 1);
  ASSERT_GT(pages[0]->size(), 1000);
  pages = fetchPages(*client, 1);
  ASSERT_EQ(pages.size(), 1);
  ASSERT_LT(pages[0]->size(), 1000);
  task->requestCancel();
  bufferManager_->removeTask(task->taskId());
  client->close();
}

TEST_F(ExchangeClientTest, multiPageFetch) {
  auto client =
      std::make_shared<ExchangeClient>("test", 17, 1 << 20, pool(), executor());

  {
    bool atEnd;
    ContinueFuture future = ContinueFuture::makeEmpty();
    auto pages = client->next(1, &atEnd, &future);
    ASSERT_EQ(0, pages.size());
    ASSERT_FALSE(atEnd);
    ASSERT_TRUE(future.valid());
  }

  const auto& queue = client->queue();
  addSources(*queue, 1);

  for (auto i = 0; i < 10; ++i) {
    enqueue(*queue, makePage(1'000 + i));
  }

  // Fetch one page.
  bool atEnd;
  ContinueFuture future = ContinueFuture::makeEmpty();
  auto pages = client->next(1, &atEnd, &future);
  ASSERT_EQ(1, pages.size());
  ASSERT_FALSE(atEnd);
  ASSERT_FALSE(future.valid());

  // Fetch multiple pages. Each page is slightly larger than 1K bytes, hence,
  // only 4 pages fit.
  pages = client->next(5'000, &atEnd, &future);
  ASSERT_EQ(4, pages.size());
  ASSERT_FALSE(atEnd);
  ASSERT_FALSE(future.valid());

  // Fetch the rest of the pages.
  pages = client->next(10'000, &atEnd, &future);
  ASSERT_EQ(5, pages.size());
  ASSERT_FALSE(atEnd);
  ASSERT_FALSE(future.valid());

  // Signal no-more-data.
  enqueue(*queue, nullptr);

  pages = client->next(10'000, &atEnd, &future);
  ASSERT_EQ(0, pages.size());
  ASSERT_TRUE(atEnd);
  ASSERT_FALSE(future.valid());

  client->close();
}

TEST_F(ExchangeClientTest, sourceTimeout) {
  constexpr int32_t kNumSources = 3;
  auto client =
      std::make_shared<ExchangeClient>("test", 17, 1 << 20, pool(), executor());

  bool atEnd;
  ContinueFuture future;
  auto pages = client->next(1, &atEnd, &future);
  ASSERT_EQ(0, pages.size());
  ASSERT_FALSE(atEnd);

  for (auto i = 0; i < kNumSources; ++i) {
    client->addRemoteTaskId(fmt::format("local://{}", i));
  }
  client->noMoreRemoteTasks();

  // Fetch a page. No page is found. All sources are fetching.
  pages = client->next(1, &atEnd, &future);
  EXPECT_TRUE(pages.empty());

  std::mutex mutex;
  std::unordered_set<void*> sourcesWithTimeout;
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::test::LocalExchangeSource::timeout",
      std::function<void(void*)>(([&](void* source) {
        std::lock_guard<std::mutex> l(mutex);
        sourcesWithTimeout.insert(source);
        LOG(INFO) << "inside lambda" << source
                  << " n=" << sourcesWithTimeout.size();
      })));

#ifndef NDEBUG
  // Wait until all sources have timed out at least once.
  auto deadline = std::chrono::system_clock::now() +
      3 * kNumSources *
          std::chrono::seconds(ExchangeClient::kRequestDataSizesMaxWait);
  while (std::chrono::system_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> l(mutex);
      if (sourcesWithTimeout.size() >= kNumSources) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  EXPECT_LT(std::chrono::system_clock::now(), deadline);
#endif

  const auto& queue = client->queue();
  for (auto i = 0; i < 10; ++i) {
    enqueue(*queue, makePage(1'000 + i));
  }

  // Fetch one page.
  pages = client->next(1, &atEnd, &future);
  EXPECT_EQ(1, pages.size());
  EXPECT_FALSE(atEnd);

  // Fetch multiple pages. Each page is slightly larger than 1K bytes, hence,
  // only 4 pages fit.
  pages = client->next(5'000, &atEnd, &future);
  EXPECT_EQ(4, pages.size());
  EXPECT_FALSE(atEnd);

  // Fetch the rest of the pages.
  pages = client->next(10'000, &atEnd, &future);
  EXPECT_EQ(5, pages.size());
  EXPECT_FALSE(atEnd);

  // Signal no-more-data for all sources.
  for (auto i = 0; i < kNumSources; ++i) {
    enqueue(*queue, nullptr);
  }
  pages = client->next(10'000, &atEnd, &future);
  EXPECT_EQ(0, pages.size());
  EXPECT_TRUE(atEnd);

  client->close();
}

TEST_F(ExchangeClientTest, callNextAfterClose) {
  constexpr int32_t kNumSources = 3;
  common::testutil::TestValue::enable();
  auto client =
      std::make_shared<ExchangeClient>("test", 17, 1 << 20, pool(), executor());

  bool atEnd;
  ContinueFuture future;
  auto pages = client->next(1, &atEnd, &future);
  ASSERT_EQ(0, pages.size());
  ASSERT_FALSE(atEnd);

  for (auto i = 0; i < kNumSources; ++i) {
    client->addRemoteTaskId(fmt::format("local://{}", i));
  }
  client->noMoreRemoteTasks();

  // Fetch a page. No page is found. All sources are fetching.
  pages = client->next(1, &atEnd, &future);
  EXPECT_TRUE(pages.empty());

  const auto& queue = client->queue();
  for (auto i = 0; i < 10; ++i) {
    enqueue(*queue, makePage(1'000 + i));
  }

  // Fetch multiple pages. Each page is slightly larger than 1K bytes, hence,
  // only 4 pages fit.
  pages = client->next(5'000, &atEnd, &future);
  EXPECT_EQ(4, pages.size());
  EXPECT_FALSE(atEnd);

  // Close the client and try calling next again.
  client->close();

  // Here we should have no pages returned, be at end (we are closed) and the
  // future should be invalid (not based on a valid promise).
  ContinueFuture futureFinal{ContinueFuture::makeEmpty()};
  pages = client->next(10'000, &atEnd, &futureFinal);
  EXPECT_EQ(0, pages.size());
  EXPECT_TRUE(atEnd);
  EXPECT_FALSE(futureFinal.valid());

  client->close();
}

TEST_F(ExchangeClientTest, acknowledge) {
  const int64_t pageSize = 1024;
  const int64_t clientBufferSize = pageSize;
  const int64_t serverBufferSize = 2 * pageSize;

  const auto sourceTaskId = "local://test-acknowledge-source-task";
  const auto task = makeTask(sourceTaskId, serverBufferSize);
  auto taskRemoveGuard =
      folly::makeGuard([bufferManager = bufferManager_, task]() {
        task->requestCancel();
        bufferManager->removeTask(task->taskId());
      });

  bufferManager_->initializeTask(
      task, core::PartitionedOutputNode::Kind::kPartitioned, 2, 1);

  auto client = std::make_shared<ExchangeClient>(
      "local://test-acknowledge-client-task",
      1,
      clientBufferSize,
      pool(),
      executor());
  auto clientCloseGuard = folly::makeGuard([client]() { client->close(); });

  std::atomic_int numberOfAcknowledgeRequests{0};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::test::LocalExchangeSource::pause",
      std::function<void(void*)>(([&numberOfAcknowledgeRequests](void*) {
        numberOfAcknowledgeRequests++;
      })));

  {
    // adding the first page should not block as there is enough space in
    // the output buffer for two pages
    ContinueFuture future;
    bufferManager_->enqueue(sourceTaskId, 1, makePage(pageSize), &future);
    ASSERT_TRUE(future.isReady());
  }

  {
    // adding the second page may block but will get unblocked once the
    // client fetches a single page
    ContinueFuture future;
    bufferManager_->enqueue(sourceTaskId, 1, makePage(pageSize), &future);

    // start fetching
    client->addRemoteTaskId(sourceTaskId);
    client->noMoreRemoteTasks();

    ASSERT_TRUE(std::move(future)
                    .via(executor())
                    .wait(std::chrono::seconds{10})
                    .isReady());

#ifndef NDEBUG
    // The client knew there is more data available but could not fetch any more
    // Explicit acknowledge was required
    EXPECT_EQ(numberOfAcknowledgeRequests, 1);
#endif
  }

  {
    // adding the third page should block (one page is in the exchange queue,
    // another two pages are in the output buffer)
    ContinueFuture enqueueDetachedFuture;
    bufferManager_->enqueue(
        sourceTaskId, 1, makePage(pageSize), &enqueueDetachedFuture);
    ASSERT_FALSE(enqueueDetachedFuture.isReady());

    auto enqueueFuture = std::move(enqueueDetachedFuture)
                             .via(executor())
                             .wait(std::chrono::milliseconds{100});
    ASSERT_FALSE(enqueueFuture.isReady());

    // removing one page from the exchange queue should trigger a fetch and
    // a subsequent acknowledge to release the output buffer memory
    bool atEnd;
    ContinueFuture dequeueDetachedFuture;
    auto pages = client->next(1, &atEnd, &dequeueDetachedFuture);
    ASSERT_EQ(1, pages.size());
    ASSERT_FALSE(atEnd);
    ASSERT_TRUE(dequeueDetachedFuture.isReady());

    ASSERT_TRUE(
        std::move(enqueueFuture).wait(std::chrono::seconds{10}).isReady());
#ifndef NDEBUG
    // The client knew there is more data available but could not fetch any more
    // Explicit acknowledge was required
    EXPECT_EQ(numberOfAcknowledgeRequests, 2);
#endif
  }

  // one page is still in the buffer at this point
  ASSERT_EQ(bufferManager_->getUtilization(sourceTaskId), 0.5);

  auto pages = fetchPages(*client, 1);
  ASSERT_EQ(1, pages.size());

  {
    // at this point the output buffer is expected to be empty
    int attempts = 100;
    bool outputBuffersEmpty;
    while (attempts > 0) {
      attempts--;
      outputBuffersEmpty = bufferManager_->getUtilization(sourceTaskId) == 0;
      if (outputBuffersEmpty) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    ASSERT_TRUE(outputBuffersEmpty);
#ifndef NDEBUG
    // The output buffer is empty now
    // Explicit acknowledge is not necessary as a blocking getDataSize is sent
    // right away
    EXPECT_EQ(numberOfAcknowledgeRequests, 2);
#endif
  }

  pages = fetchPages(*client, 1);
  ASSERT_EQ(1, pages.size());

  bufferManager_->noMoreData(sourceTaskId);

  bool atEnd;
  ContinueFuture dequeueEndOfDataFuture;
  pages = client->next(1, &atEnd, &dequeueEndOfDataFuture);
  ASSERT_EQ(0, pages.size());

  ASSERT_TRUE(std::move(dequeueEndOfDataFuture)
                  .via(executor())
                  .wait(std::chrono::seconds{10})
                  .isReady());
  pages = client->next(1, &atEnd, &dequeueEndOfDataFuture);
  ASSERT_EQ(0, pages.size());
  ASSERT_TRUE(atEnd);
}

} // namespace
} // namespace facebook::velox::exec
