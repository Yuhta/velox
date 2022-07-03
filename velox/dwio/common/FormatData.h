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

#pragma once

#include "velox/common/memory/Memory.h"
#include "velox/dwio/common/ColumnSelector.h"
#include "velox/dwio/common/ScanSpec.h"
#include "velox/dwio/common/Statistics.h"
#include "velox/dwio/common/TypeWithId.h"

namespace facebook::velox::dwio::common {

// Interface base class for format-specific state in common between all types of
// readers.
class FormatData {
 public:
  virtual ~FormatData() = default;

  template <typename T>
  T& as() {
    return *reinterpret_cast<T*>(this);
  }

  virtual void prepareRead(
      folly::Range<const int32_t*> rows,
      const uint64_t* incomingNulls) {}

  virtual std::vector<uint32_t> filterRowGroups(
      const velox::common::ScanSpec& scanSpec,
      uint64_t rowsPerRowGroup,
      const StatsWriterInfo& context) = 0;
};

// Base class for format-specific initialization arguments.
class FormatParams {
 public:
  explicit FormatParams(memory::MemoryPool& pool) : pool_(pool) {}

  virtual ~FormatParams() = default;

  // Makes format-specific structures for the column of 'type'.
  virtual std::unique_ptr<FormatData> toFormatData(
      const std::shared_ptr<const dwio::common::TypeWithId>& type) = 0;

  memory::MemoryPool& pool() {
    return pool_;
  }

 private:
  memory::MemoryPool& pool_;
};

} // namespace facebook::velox::dwio::common
