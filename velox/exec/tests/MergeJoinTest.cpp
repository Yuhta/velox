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

#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/HiveConnectorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"

#include "folly/experimental/EventCount.h"

using namespace facebook::velox;
using namespace facebook::velox::common::testutil;
using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;

class MergeJoinTest : public HiveConnectorTestBase {
 protected:
  using OperatorTestBase::assertQuery;

  CursorParameters makeCursorParameters(
      const std::shared_ptr<const core::PlanNode>& planNode,
      uint32_t preferredOutputBatchSize) {
    auto queryCtx = core::QueryCtx::create(executor_.get());

    CursorParameters params;
    params.planNode = planNode;
    params.queryCtx = queryCtx;
    params.queryCtx->testingOverrideConfigUnsafe(
        {{core::QueryConfig::kPreferredOutputBatchRows,
          std::to_string(preferredOutputBatchSize)}});
    return params;
  }

  template <typename T>
  void testJoin(
      std::function<T(vector_size_t /*row*/)> leftKeyAt,
      std::function<T(vector_size_t /*row*/)> rightKeyAt) {
    // Single batch on the left and right sides of the join.
    {
      auto leftKeys = makeFlatVector<T>(1'234, leftKeyAt);
      auto rightKeys = makeFlatVector<T>(1'234, rightKeyAt);

      testJoin({leftKeys}, {rightKeys});
    }

    // Multiple batches on one side. Single batch on the other side.
    {
      std::vector<VectorPtr> leftKeys = {
          makeFlatVector<T>(1024, leftKeyAt),
          makeFlatVector<T>(
              1024, [&](auto row) { return leftKeyAt(1024 + row); }),
      };
      std::vector<VectorPtr> rightKeys = {makeFlatVector<T>(2048, rightKeyAt)};

      testJoin(leftKeys, rightKeys);

      // Swap left and right side keys.
      testJoin(rightKeys, leftKeys);
    }

    // Multiple batches on each side.
    {
      std::vector<VectorPtr> leftKeys = {
          makeFlatVector<T>(512, leftKeyAt),
          makeFlatVector<T>(
              1024, [&](auto row) { return leftKeyAt(512 + row); }),
          makeFlatVector<T>(
              16, [&](auto row) { return leftKeyAt(512 + 1024 + row); }),
      };
      std::vector<VectorPtr> rightKeys = {
          makeFlatVector<T>(123, rightKeyAt),
          makeFlatVector<T>(
              1024, [&](auto row) { return rightKeyAt(123 + row); }),
          makeFlatVector<T>(
              1234, [&](auto row) { return rightKeyAt(123 + 1024 + row); }),
      };

      testJoin(leftKeys, rightKeys);

      // Swap left and right side keys.
      testJoin(rightKeys, leftKeys);
    }
  }

  void testJoin(
      const std::vector<VectorPtr>& leftKeys,
      const std::vector<VectorPtr>& rightKeys) {
    std::vector<RowVectorPtr> left;
    left.reserve(leftKeys.size());
    vector_size_t startRow = 0;
    for (const auto& key : leftKeys) {
      auto payload = makeFlatVector<int32_t>(
          key->size(), [startRow](auto row) { return (startRow + row) * 10; });
      left.push_back(makeRowVector({key, payload}));
      startRow += key->size();
    }

    std::vector<RowVectorPtr> right;
    right.reserve(rightKeys.size());
    startRow = 0;
    for (const auto& key : rightKeys) {
      auto payload = makeFlatVector<int32_t>(
          key->size(), [startRow](auto row) { return (startRow + row) * 20; });
      right.push_back(makeRowVector({key, payload}));
      startRow += key->size();
    }

    createDuckDbTable("t", left);
    createDuckDbTable("u", right);

    // Test INNER join.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto plan = PlanBuilder(planNodeIdGenerator)
                    .values(left)
                    .mergeJoin(
                        {"c0"},
                        {"u_c0"},
                        PlanBuilder(planNodeIdGenerator)
                            .values(right)
                            .project({"c1 AS u_c1", "c0 AS u_c0"})
                            .planNode(),
                        "",
                        {"c0", "c1", "u_c1"},
                        core::JoinType::kInner)
                    .planNode();

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(plan, 16),
        "SELECT t.c0, t.c1, u.c1 FROM t, u WHERE t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(plan, 1024),
        "SELECT t.c0, t.c1, u.c1 FROM t, u WHERE t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(plan, 10'000),
        "SELECT t.c0, t.c1, u.c1 FROM t, u WHERE t.c0 = u.c0");

    // Test LEFT join.
    planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto leftPlan = PlanBuilder(planNodeIdGenerator)
                        .values(left)
                        .mergeJoin(
                            {"c0"},
                            {"u_c0"},
                            PlanBuilder(planNodeIdGenerator)
                                .values(right)
                                .project({"c1 as u_c1", "c0 as u_c0"})
                                .planNode(),
                            "",
                            {"c0", "c1", "u_c1"},
                            core::JoinType::kLeft)
                        .planNode();

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(leftPlan, 16),
        "SELECT t.c0, t.c1, u.c1 FROM t LEFT JOIN u ON t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(leftPlan, 1024),
        "SELECT t.c0, t.c1, u.c1 FROM t LEFT JOIN u ON t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(leftPlan, 10'000),
        "SELECT t.c0, t.c1, u.c1 FROM t LEFT JOIN u ON t.c0 = u.c0");

    // Test RIGHT join.
    planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto rightPlan = PlanBuilder(planNodeIdGenerator)
                         .values(right)
                         .mergeJoin(
                             {"c0"},
                             {"u_c0"},
                             PlanBuilder(planNodeIdGenerator)
                                 .values(left)
                                 .project({"c1 as u_c1", "c0 as u_c0"})
                                 .planNode(),
                             "",
                             {"u_c0", "u_c1", "c1"},
                             core::JoinType::kRight)
                         .planNode();

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(rightPlan, 16),
        "SELECT t.c0, t.c1, u.c1 FROM u RIGHT JOIN t ON t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(rightPlan, 1024),
        "SELECT t.c0, t.c1, u.c1 FROM u RIGHT JOIN t ON t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(rightPlan, 10'000),
        "SELECT t.c0, t.c1, u.c1 FROM u RIGHT JOIN t ON t.c0 = u.c0");

    // Test right join and left join with same result.
    auto expectedResult = AssertQueryBuilder(leftPlan).copyResults(pool_.get());
    AssertQueryBuilder(rightPlan).assertResults(expectedResult);

    // Test FULL join.
    planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto fullPlan = PlanBuilder(planNodeIdGenerator)
                        .values(right)
                        .mergeJoin(
                            {"c0"},
                            {"u_c0"},
                            PlanBuilder(planNodeIdGenerator)
                                .values(left)
                                .project({"c1 as u_c1", "c0 as u_c0"})
                                .planNode(),
                            "",
                            {"u_c0", "u_c1", "c1"},
                            core::JoinType::kFull)
                        .planNode();

    // Use very small output batch size.
    assertQuery(
        makeCursorParameters(fullPlan, 16),
        "SELECT t.c0, t.c1, u.c1 FROM u FULL OUTER JOIN t ON t.c0 = u.c0");

    // Use regular output batch size.
    assertQuery(
        makeCursorParameters(fullPlan, 1024),
        "SELECT t.c0, t.c1, u.c1 FROM u FULL OUTER JOIN t ON t.c0 = u.c0");

    // Use very large output batch size.
    assertQuery(
        makeCursorParameters(fullPlan, 10'000),
        "SELECT t.c0, t.c1, u.c1 FROM u FULL OUTER JOIN t ON t.c0 = u.c0");
  }
};

TEST_F(MergeJoinTest, oneToOneAllMatch) {
  testJoin<int32_t>([](auto row) { return row; }, [](auto row) { return row; });
}

TEST_F(MergeJoinTest, someDontMatch) {
  testJoin<int32_t>(
      [](auto row) { return row % 5 == 0 ? row - 1 : row; },
      [](auto row) { return row % 7 == 0 ? row - 1 : row; });
}

TEST_F(MergeJoinTest, fewMatch) {
  testJoin<int32_t>(
      [](auto row) { return row * 5; }, [](auto row) { return row * 7; });
}

TEST_F(MergeJoinTest, duplicateMatch) {
  testJoin<int32_t>(
      [](auto row) { return row / 2; }, [](auto row) { return row / 3; });
}

TEST_F(MergeJoinTest, allRowsMatch) {
  std::vector<VectorPtr> leftKeys = {
      makeFlatVector<int32_t>(2, [](auto /* row */) { return 5; }),
      makeFlatVector<int32_t>(3, [](auto /* row */) { return 5; }),
      makeFlatVector<int32_t>(4, [](auto /* row */) { return 5; }),
  };
  std::vector<VectorPtr> rightKeys = {
      makeFlatVector<int32_t>(7, [](auto /* row */) { return 5; })};

  testJoin(leftKeys, rightKeys);

  testJoin(rightKeys, leftKeys);
}

TEST_F(MergeJoinTest, aggregationOverJoin) {
  auto left =
      makeRowVector({"t_c0"}, {makeFlatVector<int32_t>({1, 2, 3, 4, 5})});
  auto right = makeRowVector({"u_c0"}, {makeFlatVector<int32_t>({2, 4, 6})});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t_c0"},
              {"u_c0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t_c0", "u_c0"},
              core::JoinType::kInner)
          .singleAggregation({}, {"count(1)"})
          .planNode();

  auto result = readSingleValue(plan);
  ASSERT_FALSE(result.isNull());
  ASSERT_EQ(2, result.value<int64_t>());
}

TEST_F(MergeJoinTest, nonFirstJoinKeys) {
  auto left = makeRowVector(
      {"t_data", "t_key"},
      {
          makeFlatVector<int32_t>({50, 40, 30, 20, 10}),
          makeFlatVector<int32_t>({1, 2, 3, 4, 5}),
      });
  auto right = makeRowVector(
      {"u_data", "u_key"},
      {
          makeFlatVector<int32_t>({23, 22, 21}),
          makeFlatVector<int32_t>({2, 4, 6}),
      });

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t_key"},
              {"u_key"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t_key", "t_data", "u_data"},
              core::JoinType::kInner)
          .planNode();

  assertQuery(plan, "VALUES (2, 40, 23), (4, 20, 22)");
}

TEST_F(MergeJoinTest, innerJoinFilter) {
  vector_size_t size = 1'000;
  // Join keys on the left side: 0, 10, 20,..
  // Payload on the left side: 0, 1, 2, 3,..
  auto left = makeRowVector(
      {"t_c0", "t_c1"},
      {
          makeFlatVector<int32_t>(size, [](auto row) { return row * 10; }),
          makeFlatVector<int64_t>(
              size, [](auto row) { return row; }, nullEvery(13)),
      });

  // Join keys on the right side: 0, 5, 10, 15, 20,..
  // Payload on the right side: 0, 1, 2, 3, 4, 5, 6, 0, 1, 2,..
  auto right = makeRowVector(
      {"u_c0", "u_c1"},
      {
          makeFlatVector<int32_t>(size, [](auto row) { return row * 5; }),
          makeFlatVector<int64_t>(
              size, [](auto row) { return row % 7; }, nullEvery(17)),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto plan = [&](const std::string& filter) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    return PlanBuilder(planNodeIdGenerator)
        .values({left})
        .mergeJoin(
            {"t_c0"},
            {"u_c0"},
            PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
            filter,
            {"t_c0", "u_c0", "u_c1"},
            core::JoinType::kInner)
        .planNode();
  };

  assertQuery(
      plan("(t_c1 + u_c1) % 2 = 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");

  assertQuery(
      plan("(t_c1 + u_c1) % 2 = 1"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 1");

  // No rows pass filter.
  assertQuery(
      plan("(t_c1 + u_c1) % 2 < 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 < 0");

  // All rows pass filter.
  assertQuery(
      plan("(t_c1 + u_c1) % 2 >= 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 >= 0");

  // Filter expressions over join keys.
  assertQuery(
      plan("(t_c0 + u_c1) % 2 = 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c0 + u_c1) % 2 = 0");

  assertQuery(
      plan("(t_c1 + u_c0) % 2 = 0"),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c0) % 2 = 0");

  // Very small output batch size.
  assertQuery(
      makeCursorParameters(plan("(t_c1 + u_c1) % 2 = 0"), 16),
      "SELECT t_c0, u_c0, u_c1 FROM t, u WHERE t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");
}

TEST_F(MergeJoinTest, leftAndRightJoinFilter) {
  // Each row on the left side has at most one match on the right side.
  auto left = makeRowVector(
      {"t_c0", "t_c1"},
      {
          makeFlatVector<int32_t>({0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50}),
          makeFlatVector<int32_t>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}),
      });

  auto right = makeRowVector(
      {"u_c0", "u_c1"},
      {
          makeFlatVector<int32_t>({0, 10, 20, 30, 40, 50}),
          makeFlatVector<int32_t>({0, 1, 2, 3, 4, 5}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto leftPlan = [&](const std::string& filter) {
    return PlanBuilder(planNodeIdGenerator)
        .values({left})
        .mergeJoin(
            {"t_c0"},
            {"u_c0"},
            PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
            filter,
            {"t_c0", "t_c1", "u_c1"},
            core::JoinType::kLeft)
        .planNode();
  };

  auto rightPlan = [&](const std::string& filter) {
    return PlanBuilder(planNodeIdGenerator)
        .values({right})
        .mergeJoin(
            {"u_c0"},
            {"t_c0"},
            PlanBuilder(planNodeIdGenerator).values({left}).planNode(),
            filter,
            {"t_c0", "t_c1", "u_c1"},
            core::JoinType::kRight)
        .planNode();
  };

  // Test with different output batch sizes.
  for (auto batchSize : {1, 3, 16}) {
    assertQuery(
        makeCursorParameters(leftPlan("(t_c1 + u_c1) % 2 = 0"), batchSize),
        "SELECT t_c0, t_c1, u_c1 FROM t LEFT JOIN u ON t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");

    assertQuery(
        makeCursorParameters(rightPlan("(t_c1 + u_c1) % 2 = 0"), batchSize),
        "SELECT t_c0, t_c1, u_c1 FROM u RIGHT JOIN t ON t_c0 = u_c0 AND (t_c1 + u_c1) % 2 = 0");
  }

  // A left-side row with multiple matches on the right side.
  left = makeRowVector(
      {"t_c0", "t_c1"},
      {
          makeFlatVector<int32_t>({5, 10}),
          makeFlatVector<int32_t>({0, 0}),
      });

  right = makeRowVector(
      {"u_c0", "u_c1"},
      {
          makeFlatVector<int32_t>({10, 10, 10, 10, 10, 10}),
          makeFlatVector<int32_t>({0, 1, 2, 3, 4, 5}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Test with different filters and output batch sizes.
  for (auto batchSize : {1, 3, 16}) {
    for (auto filter :
         {"t_c1 + u_c1 > 3",
          "t_c1 + u_c1 < 3",
          "t_c1 + u_c1 > 100",
          "t_c1 + u_c1 < 100"}) {
      assertQuery(
          makeCursorParameters(leftPlan(filter), batchSize),
          fmt::format(
              "SELECT t_c0, t_c1, u_c1 FROM t LEFT JOIN u ON t_c0 = u_c0 AND {}",
              filter));
      assertQuery(
          makeCursorParameters(rightPlan(filter), batchSize),
          fmt::format(
              "SELECT t_c0, t_c1, u_c1 FROM u RIGHT JOIN t ON t_c0 = u_c0 AND {}",
              filter));
    }
  }
}

TEST_F(MergeJoinTest, rightJoinWithDuplicateMatch) {
  // Each row on the left side has at most one match on the right side.
  auto left = makeRowVector(
      {"a", "b"},
      {
          makeNullableFlatVector<int32_t>({1, 2, 2, 2, 3, 5, 6, std::nullopt}),
          makeNullableFlatVector<double>(
              {2.0, 100.0, 1.0, 1.0, 3.0, 1.0, 6.0, std::nullopt}),
      });

  auto right = makeRowVector(
      {"c", "d"},
      {
          makeNullableFlatVector<int32_t>(
              {0, 2, 2, 2, 2, 3, 4, 5, 7, std::nullopt}),
          makeNullableFlatVector<double>(
              {0.0, 3.0, -1.0, -1.0, 3.0, 2.0, 1.0, 3.0, 7.0, std::nullopt}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();

  auto rightPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b", "c", "d"},
              core::JoinType::kRight)
          .planNode();
  AssertQueryBuilder(rightPlan, duckDbQueryRunner_)
      .assertResults("SELECT * from t RIGHT JOIN u ON a = c AND b < d");
}

TEST_F(MergeJoinTest, rightJoinFilterWithNull) {
  auto left = makeRowVector(
      {"a", "b"},
      {
          makeNullableFlatVector<int32_t>({std::nullopt, std::nullopt}),
          makeNullableFlatVector<double>({std::nullopt, std::nullopt}),
      });

  auto right = makeRowVector(
      {"c", "d"},
      {
          makeNullableFlatVector<int32_t>(
              {std::nullopt, std::nullopt, std::nullopt}),
          makeNullableFlatVector<double>(
              {std::nullopt, std::nullopt, std::nullopt}),
      });

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();

  auto rightPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b", "c", "d"},
              core::JoinType::kRight)
          .planNode();
  AssertQueryBuilder(rightPlan, duckDbQueryRunner_)
      .assertResults("SELECT * from t RIGHT JOIN u ON a = c AND b < d");
}

// Verify that both left-side and right-side pipelines feeding the merge join
// always run single-threaded.
TEST_F(MergeJoinTest, numDrivers) {
  auto left = makeRowVector({"t_c0"}, {makeFlatVector<int32_t>({1, 2, 3})});
  auto right = makeRowVector({"u_c0"}, {makeFlatVector<int32_t>({0, 2, 5})});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left}, true)
          .mergeJoin(
              {"t_c0"},
              {"u_c0"},
              PlanBuilder(planNodeIdGenerator).values({right}, true).planNode(),
              "",
              {"t_c0", "u_c0"},
              core::JoinType::kInner)
          .planNode();

  auto task = AssertQueryBuilder(plan, duckDbQueryRunner_)
                  .maxDrivers(5)
                  .assertResults("SELECT 2, 2");

  // We have two pipelines in the task and each must have 1 driver.
  EXPECT_EQ(2, task->numTotalDrivers());
  EXPECT_EQ(2, task->numFinishedDrivers());
}

TEST_F(MergeJoinTest, lazyVectors) {
  // a dataset of multiple row groups with multiple columns. We create
  // different dictionary wrappings for different columns and load the
  // rows in scope at different times.  We make 11000 repeats of 300
  // followed by ascending rows. These will hits one 300 from the
  // right side and cover more than one batch, so that we test lazy
  // loading where we buffer multiple batches of input.
  auto leftVectors = makeRowVector(
      {makeFlatVector<int32_t>(
           30'000, [](auto row) { return row < 11000 ? 300 : row; }),
       makeFlatVector<int64_t>(30'000, [](auto row) { return row % 23; }),
       makeFlatVector<int32_t>(30'000, [](auto row) { return row % 31; }),
       makeFlatVector<StringView>(30'000, [](auto row) {
         return StringView::makeInline(fmt::format("{}   string", row % 43));
       })});

  auto rightVectors = makeRowVector(
      {"rc0", "rc1"},
      {makeFlatVector<int32_t>(10'000, [](auto row) { return row * 3; }),
       makeFlatVector<int64_t>(10'000, [](auto row) { return row % 31; })});

  auto leftFile = TempFilePath::create();
  writeToFile(leftFile->getPath(), leftVectors);
  createDuckDbTable("t", {leftVectors});

  auto rightFile = TempFilePath::create();
  writeToFile(rightFile->getPath(), rightVectors);
  createDuckDbTable("u", {rightVectors});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  core::PlanNodeId leftScanId;
  core::PlanNodeId rightScanId;
  auto op = PlanBuilder(planNodeIdGenerator)
                .tableScan(
                    ROW({"c0", "c1", "c2", "c3"},
                        {INTEGER(), BIGINT(), INTEGER(), VARCHAR()}))
                .capturePlanNodeId(leftScanId)
                .mergeJoin(
                    {"c0"},
                    {"rc0"},
                    PlanBuilder(planNodeIdGenerator)
                        .tableScan(ROW({"rc0", "rc1"}, {INTEGER(), BIGINT()}))
                        .capturePlanNodeId(rightScanId)
                        .planNode(),
                    "c1 + rc1 < 30",
                    {"c0", "rc0", "c1", "rc1", "c2", "c3"})
                .planNode();

  AssertQueryBuilder(op, duckDbQueryRunner_)
      .split(rightScanId, makeHiveConnectorSplit(rightFile->getPath()))
      .split(leftScanId, makeHiveConnectorSplit(leftFile->getPath()))
      .assertResults(
          "SELECT c0, rc0, c1, rc1, c2, c3  FROM t, u WHERE t.c0 = u.rc0 and c1 + rc1 < 30");
}

// Ensures the output of merge joins are dictionaries.
TEST_F(MergeJoinTest, dictionaryOutput) {
  auto left =
      makeRowVector({"t_c0"}, {makeFlatVector<int32_t>({1, 2, 3, 4, 5})});
  auto right = makeRowVector({"u_c0"}, {makeFlatVector<int32_t>({2, 4, 6})});

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t_c0"},
              {"u_c0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t_c0", "u_c0"},
              core::JoinType::kInner)
          .planFragment();

  // Run task with special callback so we can capture results without them being
  // copied/flattened.
  RowVectorPtr output;
  auto task = Task::create(
      "0",
      std::move(plan),
      0,
      core::QueryCtx::create(driverExecutor_.get()),
      Task::ExecutionMode::kParallel,
      [&](const RowVectorPtr& vector, ContinueFuture* future) {
        if (vector) {
          output = vector;
        }
        return BlockingReason::kNotBlocked;
      });

  task->start(2);
  waitForTaskCompletion(task.get());

  for (const auto& child : output->children()) {
    EXPECT_TRUE(isDictionary(child->encoding()));
  }

  // Output can't outlive the task.
  output.reset();
}

TEST_F(MergeJoinTest, semiJoin) {
  auto left = makeRowVector(
      {"t0"}, {makeNullableFlatVector<int64_t>({1, 2, 2, 6, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, 2, 7, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  auto testSemiJoin = [&](const std::string& filter,
                          const std::string& sql,
                          const std::vector<std::string>& outputLayout,
                          core::JoinType joinType) {
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto plan =
        PlanBuilder(planNodeIdGenerator)
            .values({left})
            .mergeJoin(
                {"t0"},
                {"u0"},
                PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
                filter,
                outputLayout,
                joinType)
            .planNode();
    AssertQueryBuilder(plan, duckDbQueryRunner_).assertResults(sql);
  };

  testSemiJoin(
      "t0 >1",
      "SELECT t0 FROM t where t0 IN (SELECT u0 from u) and t0 > 1",
      {"t0"},
      core::JoinType::kLeftSemiFilter);
  testSemiJoin(
      "u0 > 1",
      "SELECT u0 FROM u where u0 IN (SELECT t0 from t) and u0 > 1",
      {"u0"},
      core::JoinType::kRightSemiFilter);
}

TEST_F(MergeJoinTest, rightJoin) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, std::nullopt, 5, 6, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 8, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Right join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto rightPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "t0 > 2",
              {"t0", "u0"},
              core::JoinType::kRight)
          .planNode();
  AssertQueryBuilder(rightPlan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t RIGHT JOIN u ON t.t0 = u.u0 AND t.t0 > 2");

  auto leftPlan =
      PlanBuilder(planNodeIdGenerator)
          .values({right})
          .mergeJoin(
              {"u0"},
              {"t0"},
              PlanBuilder(planNodeIdGenerator).values({left}).planNode(),
              "t0 > 2",
              {"t0", "u0"},
              core::JoinType::kLeft)
          .planNode();
  auto expectedResult = AssertQueryBuilder(leftPlan).copyResults(pool_.get());
  AssertQueryBuilder(rightPlan).assertResults(expectedResult);
}

TEST_F(MergeJoinTest, nullKeys) {
  auto left = makeRowVector(
      {"t0"}, {makeNullableFlatVector<int64_t>({1, 2, 5, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>({1, 5, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Inner join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0", "u0"},
              core::JoinType::kInner)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults("SELECT * FROM t, u WHERE t.t0 = u.u0");

  // Left join.
  plan = PlanBuilder(planNodeIdGenerator)
             .values({left})
             .mergeJoin(
                 {"t0"},
                 {"u0"},
                 PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
                 "",
                 {"t0", "u0"},
                 core::JoinType::kLeft)
             .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults("SELECT * FROM t LEFT JOIN u ON t.t0 = u.u0");
}

TEST_F(MergeJoinTest, antiJoinWithFilter) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, 4, 5, 8, 9, std::nullopt, 10, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 7, std::nullopt, std::nullopt, 8, 9, 10})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "t0 > 2",
              {"t0"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0 FROM t WHERE NOT exists (select 1 from u where t0 = u0 AND t.t0 > 2 ) ");
}

TEST_F(MergeJoinTest, antiJoinFailed) {
  auto size = 1'00;
  auto left = makeRowVector(
      {"t0"}, {makeFlatVector<int64_t>(size, [](auto row) { return row; })});

  auto right = makeRowVector(
      {"u0"}, {makeFlatVector<int64_t>(size, [](auto row) { return row; })});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values(split(left, 10))
          .orderBy({"t0"}, false)
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .config(core::QueryConfig::kMaxOutputBatchRows, "10")
      .assertResults(
          "SELECT t0 FROM t WHERE NOT exists (select 1 from u where t0 = u0) ");
}

TEST_F(MergeJoinTest, antiJoinWithTwoJoinKeys) {
  auto left = makeRowVector(
      {"a", "b"},
      {makeNullableFlatVector<int32_t>(
           {1, 1, 2, 2, 3, std::nullopt, std::nullopt, 6}),
       makeNullableFlatVector<double>(
           {2.0, 2.0, 1.0, 1.0, 3.0, std::nullopt, 5.0, std::nullopt})});

  auto right = makeRowVector(
      {"c", "d"},
      {makeNullableFlatVector<int32_t>(
           {2, 2, 3, 4, std::nullopt, std::nullopt, 6}),
       makeNullableFlatVector<double>(
           {3.0, 3.0, 2.0, 1.0, std::nullopt, 5.0, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t WHERE NOT exists (select * from u where t.a = u.c and t.b < u.d)");
}

TEST_F(MergeJoinTest, antiJoinWithUniqueJoinKeys) {
  auto left = makeRowVector(
      {"a", "b"},
      {makeNullableFlatVector<int32_t>(
           {1, 1, 2, 2, 3, std::nullopt, std::nullopt, 6}),
       makeNullableFlatVector<double>(
           {2.0, 2.0, 1.0, 1.0, 3.0, std::nullopt, 5.0, std::nullopt})});

  auto right = makeRowVector(
      {"c", "d"},
      {makeNullableFlatVector<int32_t>({2, 3, 4, std::nullopt, 6}),
       makeNullableFlatVector<double>({3.0, 2.0, 1.0, 5.0, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"a"},
              {"c"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "b < d",
              {"a", "b"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t WHERE NOT exists (select * from u where t.a = u.c and t.b < u.d)");
}

TEST_F(MergeJoinTest, antiJoinNoFilter) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, 4, 5, 8, 9, std::nullopt, 10, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 7, std::nullopt, std::nullopt, 8, 9, 10})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Anti join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0"},
              core::JoinType::kAnti)
          .planNode();

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0 FROM t WHERE NOT exists (select 1 from u where t0 = u0)");
}

TEST_F(MergeJoinTest, fullOuterJoin) {
  auto left = makeRowVector(
      {"t0"},
      {makeNullableFlatVector<int64_t>(
          {1, 2, std::nullopt, 5, 6, std::nullopt})});

  auto right = makeRowVector(
      {"u0"},
      {makeNullableFlatVector<int64_t>(
          {1, 5, 6, 8, std::nullopt, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Full outer join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0"},
              {"u0"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "t0 > 2",
              {"t0", "u0"},
              core::JoinType::kFull)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT * FROM t FULL OUTER JOIN u ON t.t0 = u.u0 AND t.t0 > 2");
}

TEST_F(MergeJoinTest, fullOuterJoinNoFilter) {
  auto left = makeRowVector(
      {"t0", "t1", "t2", "t3"},
      {makeNullableFlatVector<int64_t>(
           {7854252584298216695,
            5874550437257860379,
            6694700278390749883,
            6952978413716179087,
            2785313305792069690,
            5306984336093303849,
            2249699434807719017,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            8814597374860168988}),
       makeNullableFlatVector<int64_t>(
           {1, 2, 3, 4, 5, 6, 7, std::nullopt, 8, 9, 10}),
       makeNullableFlatVector<bool>(
           {false,
            true,
            false,
            false,
            false,
            true,
            true,
            false,
            true,
            false,
            false}),
       makeNullableFlatVector<int64_t>(
           {58, 112, 125, 52, 69, 39, 73, 29, 101, std::nullopt, 51})});

  auto right = makeRowVector(
      {"u0", "u1", "u2", "u3"},
      {makeNullableFlatVector<int64_t>({std::nullopt}),
       makeNullableFlatVector<int64_t>({11}),
       makeNullableFlatVector<bool>({false}),
       makeNullableFlatVector<int64_t>({77})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Full outer join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0", "t1", "t2", "t3"},
              {"u0", "u1", "u2", "u3"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0", "t1"},
              core::JoinType::kFull)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0, t1 FROM t FULL OUTER JOIN u ON t3 = u3 and t2 = u2 and t1 = u1 and t.t0 = u.u0");
}

TEST_F(MergeJoinTest, fullOuterJoinWithNullCompare) {
  auto right = makeRowVector(
      {"u0", "u1"},
      {makeNullableFlatVector<bool>({false, true}),
       makeNullableFlatVector<int64_t>({std::nullopt, std::nullopt})});

  auto left = makeRowVector(
      {"t0", "t1"},
      {makeNullableFlatVector<bool>({false, false, std::nullopt}),
       makeNullableFlatVector<int64_t>(
           {std::nullopt, 1195665568, std::nullopt})});

  createDuckDbTable("t", {left});
  createDuckDbTable("u", {right});

  // Full outer join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan =
      PlanBuilder(planNodeIdGenerator)
          .values({left})
          .mergeJoin(
              {"t0", "t1"},
              {"u0", "u1"},
              PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
              "",
              {"t0", "t1", "u0", "u1"},
              core::JoinType::kFull)
          .planNode();
  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .assertResults(
          "SELECT t0, t1, u0, u1 FROM t FULL OUTER JOIN u ON t.t0 = u.u0 and t1 = u1");
}

TEST_F(MergeJoinTest, complexTypedFilter) {
  constexpr vector_size_t size{1000};

  auto right = makeRowVector(
      {"u_c0"},
      {makeFlatVector<int32_t>(size, [](auto row) { return row * 2; })});

  auto testComplexTypedFilter =
      [&](const std::vector<RowVectorPtr>& left,
          const std::string& filter,
          const std::string& queryFilter,
          const std::vector<std::string>& outputLayout) {
        createDuckDbTable("t", left);
        createDuckDbTable("u", {right});
        auto planNodeIdGenerator =
            std::make_shared<core::PlanNodeIdGenerator>();
        auto plan =
            PlanBuilder(planNodeIdGenerator)
                .values(left)
                .mergeJoin(
                    {"t_c0"},
                    {"u_c0"},
                    PlanBuilder(planNodeIdGenerator).values({right}).planNode(),
                    filter,
                    outputLayout,
                    core::JoinType::kLeft)
                .planNode();

        std::string outputs;
        for (auto i = 0; i < outputLayout.size(); ++i) {
          outputs += std::move(outputLayout[i]);
          if (i + 1 < outputLayout.size()) {
            outputs += ", ";
          }
        }

        for (size_t outputBatchSize : {1000, 1024, 13}) {
          assertQuery(
              makeCursorParameters(plan, outputBatchSize),
              fmt::format(
                  "SELECT {} FROM t LEFT JOIN u ON t_c0 = u_c0 AND {}",
                  outputs,
                  queryFilter));
        }
      };

  std::vector<std::vector<std::string>> outputLayouts{
      {"t_c0", "u_c0"}, {"t_c0", "u_c0", "t_c1"}};

  {
    const std::vector<std::vector<int32_t>> pattern{
        {1},
        {1, 2},
        {1, 2, 4},
        {1, 2, 4, 8},
        {1, 2, 4, 8, 16},
    };
    std::vector<std::vector<int32_t>> arrayVector;
    arrayVector.reserve(size);
    for (auto i = 0; i < size / pattern.size(); ++i) {
      arrayVector.insert(arrayVector.end(), pattern.begin(), pattern.end());
    }
    auto left = {
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(size, [](auto row) { return row; }),
             makeArrayVector<int32_t>(arrayVector)}),
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(
                 size, [size](auto row) { return size + row * 2; }),
             makeArrayVector<int32_t>(arrayVector)})};

    for (const auto& outputLayout : outputLayouts) {
      testComplexTypedFilter(
          left, "array_max(t_c1) >= 8", "list_max(t_c1) >= 8", outputLayout);
    }
  }

  {
    auto sizeAt = [](vector_size_t row) { return row % 5; };
    auto keyAt = [](vector_size_t row) { return row % 11; };
    auto valueAt = [](vector_size_t row) { return row % 13; };
    auto keys = makeArrayVector<int64_t>(size, sizeAt, keyAt);
    auto values = makeArrayVector<int32_t>(size, sizeAt, valueAt);

    auto mapVector =
        makeMapVector<int64_t, int32_t>(size, sizeAt, keyAt, valueAt);

    auto left = {
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(size, [](auto row) { return row; }),
             mapVector}),
        makeRowVector(
            {"t_c0", "t_c1"},
            {makeFlatVector<int32_t>(
                 size, [size](auto row) { return size + row * 2; }),
             mapVector})};

    for (const auto& outputLayout : outputLayouts) {
      testComplexTypedFilter(
          left, "cardinality(t_c1) > 4", "cardinality(t_c1) > 4", outputLayout);
    }
  }
}

DEBUG_ONLY_TEST_F(MergeJoinTest, failureOnRightSide) {
  // Test that the Task terminates cleanly when the right side of the join
  // throws an exception.

  auto leftKeys = makeFlatVector<int32_t>(1'234, [](auto row) { return row; });
  auto rightKeys = makeFlatVector<int32_t>(1'234, [](auto row) { return row; });
  std::vector<RowVectorPtr> left;
  auto payload = makeFlatVector<int32_t>(
      leftKeys->size(), [](auto row) { return row * 10; });
  left.push_back(makeRowVector({leftKeys, payload}));

  std::vector<RowVectorPtr> right;
  payload = makeFlatVector<int32_t>(
      rightKeys->size(), [](auto row) { return row * 20; });
  right.push_back(makeRowVector({rightKeys, payload}));

  createDuckDbTable("t", left);
  createDuckDbTable("u", right);

  // Test INNER join.
  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  auto plan = PlanBuilder(planNodeIdGenerator)
                  .values(left)
                  .mergeJoin(
                      {"c0"},
                      {"u_c0"},
                      PlanBuilder(planNodeIdGenerator)
                          .values(right)
                          .project({"c1 AS u_c1", "c0 AS u_c0"})
                          .planNode(),
                      "",
                      {"c0", "c1", "u_c1"},
                      core::JoinType::kInner)
                  .planNode();

  std::atomic_bool nextCalled = false;
  folly::EventCount nextCalledWait;
  std::atomic_bool enqueueCalled = false;

  // The left side will call next to fetch data from the right side.  We want
  // this to be called at least once to ensure consumerPromise_ is created in
  // the MergeSource.
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::MergeSource::next",
      std::function<void(const MergeJoinSource*)>([&](const MergeJoinSource*) {
        nextCalled = true;
        nextCalledWait.notifyAll();
      }));

  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::MergeSource::enqueue",
      std::function<void(const MergeJoinSource*)>([&](const MergeJoinSource*) {
        // Only call this the first time, otherwise if we throw an exception
        // during Driver.close the process will crash.
        if (!enqueueCalled.load()) {
          // The first time the right side calls enqueue, wait for the left side
          // to call next.  Since enqueue never finished executing there won't
          // be any data available and enqueue will create a consumerPromise_.
          enqueueCalled = true;
          nextCalledWait.await([&]() { return nextCalled.load(); });
          // Throw an exception so that the task terminates and consumerPromise_
          // is not fulfilled.
          VELOX_FAIL("Expected");
        }
      }));

  // Use very small output batch size.
  VELOX_ASSERT_THROW(
      assertQuery(
          makeCursorParameters(plan, 16),
          "SELECT t.c0, t.c1, u.c1 FROM t, u WHERE t.c0 = u.c0"),
      "Expected");

  waitForAllTasksToBeDeleted();
}
