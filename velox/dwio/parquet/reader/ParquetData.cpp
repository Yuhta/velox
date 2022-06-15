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

#include "velox/dwio/parquet/reader/ParquetData.h"
#include "velox/dwio/parquet/reader/Statistics.h"

namespace facebook::velox::parquet {

std::unique_ptr<dwio::common::FormatData> ParquetParams::toFormatData(
    const std::shared_ptr<const dwio::common::TypeWithId>& type) {
  return std::make_unique<ParquetData>(type, metaData_.row_groups, pool());
}

bool ParquetData::filterMatches(
    const RowGroup& rowGroup,
    common::Filter& filter) {
  auto colIdx = type_->column;
  auto type = type_->type;
  if (rowGroup.columns[colIdx].__isset.meta_data &&
      rowGroup.columns[colIdx].meta_data.__isset.statistics) {
    auto columnStats = buildColumnStatisticsFromThrift(
        rowGroup.columns[colIdx].meta_data.statistics,
        *type,
        rowGroup.num_rows);
    if (!testFilter(&filter, columnStats.get(), rowGroup.num_rows, type)) {
      return false;
    }
  }
  return true;
}

void ParquetData::enqueueRowGroup(
    uint32_t index,
    dwio::common::BufferedInput& input) {
  auto& chunk = rowGroups_[index].columns[type_->column];
  streams_.resize(rowGroups_.size());
  DWIO_ENSURE(
      chunk.__isset.meta_data,
      "ColumnMetaData does not exist for schema Id ",
      type_->column);
  auto& columnMetaData = chunk.meta_data;
  DWIO_ENSURE(
      chunk.__isset.meta_data,
      "ColumnMetaData does not exist for schema Id ",
      type_->column);
  auto& metaData = chunk.meta_data;

  uint64_t chunkReadOffset = metaData.data_page_offset;
  if (metaData.__isset.dictionary_page_offset &&
      metaData.dictionary_page_offset >= 4) {
    // this assumes the data pages follow the dict pages directly.
    chunkReadOffset = metaData.dictionary_page_offset;
  }
  VELOX_CHECK_GE(chunkReadOffset, 0);

  uint64_t readSize = std::min(
      metaData.total_compressed_size, metaData.total_uncompressed_size);

  auto id = dwio::common::StreamIdentifier(type_->column);
  streams_[index] = input.enqueue({chunkReadOffset, readSize}, &id);
}

void ParquetData::seekToRowGroup(uint32_t index) {
  VELOX_CHECK_LT(index, streams_.size());
  VELOX_CHECK(streams_[index], "Stream not enqueued for column");
  auto codec = rowGroups_[index].columns[type_->column].meta_data.codec;
  decoder_ = std::make_unique<PageDecoder>(
      std::move(streams_[index]), pool_, maxDefine_, maxRepeat_, codec);
}
} // namespace facebook::velox::parquet
