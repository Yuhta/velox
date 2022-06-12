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

//
// Created by Ying Su on 2/14/22.
//

#pragma once

#include "velox/dwio/parquet/reader/ParquetData.h"

namespace facebook::velox::parquet {

// Wrapper for static functions for Parquet columns.
class ParquetColumnReader {
 public:
  static std::unique_ptr<ParquetColumnReader> build(
      const std::shared_ptr<const dwio::common::TypeWithId>& dataType,
      ParquetParams& params,
      common::ScanSpec* scanSpec);
};

class ParquetStructColumnReader : public dwrf::SelectiveStructColumnReader {
 public:
  ParquetStructColumnReader(
      const std::shared_ptr<const dwio::common::TypeWithId>& dataType,
      ParquetParams& params,
      common::ScanSpec* scanSpec)
      : SelectiveStructColumnReader(dataType, params, scanSpec, dataType->type) {
    auto& childSpecs = scanSpec->children();
    for (auto i = 0; i < childSpecs.size(); ++i) {
      if (childSpecs[i]->isConstant()) {
        continue;
      }
      auto childDataType = nodeType_->childByName(childSpecs[i]->fieldName());

      children_.push_back(ParquetColumnReader::build(
          childDataType, params, childSpecs[i].get(), input_, memoryPool_));
      childSpecs[i]->setSubscript(children_.size() - 1);
    }
  }

  std::vector<uint32_t> filterRowGroups(
    uint64_t rowGroupSize,
    const StatsContext& context) const override {
    if (!scanSpec_->filter_) {
      return {};
    }
    return formatData_->as<ParquetData>().filterRowGroups(*scanSpec_->filter());
  }

  bool filterMatches(const RowGroup& rowGroup);

  void seekToRowGroup(uint32_t index) override;
};

class IntegerColumnReader : public dwrf::SelectiveIntegerColumnReader {
 public:
  IntegerColumnReader(
      std::shared_ptr<const dwio::common::TypeWithId> requestedType,
      const std::shared_ptr<const dwio::common::TypeWithId>& dataType,
      ParquetParams& params,
      uint32_t numBytes,
      common::ScanSpec* scanSpec)
      : SelectiveIntegerColumnReader(
            std::move(requestedType),
            params,
            scanSpec,
            dataType->type) {}

  bool hasBulkPath() const override {
    return true;
  }

  void seekToRowGroup(uint32_t index) override {
    formatData_->as<ParquetData>.seekToRowGroup(index);
  }

  uint64_t skip(uint64_t numValues) override {
    formatData_->as<ParquetData>().skip(numValues);
  }

  void read(vector_size_t offset, RowSet rows, const uint64_t* incomingNulls)
      override {
    auto& data = formatData_->as<ParquetData>();
    VELOX_WIDTH_DISPATCH(
			 dwrf::sizeOfIntKind(type_->type->kind()), prepareRead, offset, rows, nullptr);

    readCommon<IntegerColumnReader>(rows);
  }

  template <typename ColumnVisitor>
  void readWithVisitor(RowSet rows, ColumnVisitor visitor) {
    formatData<ParquetData>().readWithVisitor(visitor);
  }
}

} // namespace facebook::velox::parquet
