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

#include "velox/dwio/common/DataBuffer.h"
#include "velox/dwio/common/DataSink.h"

#include "velox/vector/ComplexVector.h"

#include <parquet/arrow/writer.h>

namespace facebook::velox::parquet {

class DataBufferSink : public arrow::io::OutputStream {
 public:
  DataBufferSink(memory::MemoryPool& pool) : buffer_(pool) {}

  arrow::Status Write(const std::shared_ptr<arrow::Buffer>& data) {
    buffer_.append(
        buffer_.size(),
        reinterpret_cast<const char*>(data->data()),
        data->size());
    return arrow::Status::OK();
  }

  arrow::Status Write(const void* data, int64_t nbytes) override {
    buffer_.append(buffer_.size(), reinterpret_cast<const char*>(data), nbytes);
    return arrow::Status::OK();
  }

  arrow::Status Flush() override {
    return arrow::Status::OK();
  }

  arrow::Status Write(arrow::util::string_view data) {
    buffer_.append(buffer_.size(), data.data(), data.size());
    return arrow::Status::OK();
  }

  arrow::Result<int64_t> Tell() const override {
    return buffer_.size();
  }

  arrow::Status Close() override {
    return arrow::Status::OK();
  }

  bool closed() const override {
    return false;
  }

  dwio::common::DataBuffer<char>& dataBuffer() {
    return buffer_;
  }

 private:
  dwio::common::DataBuffer<char> buffer_;
};

class Writer {
 public:
  Writer(
      std::unique_ptr<dwio::common::DataSink> sink,
      memory::MemoryPool& pool,
      int32_t rowsInRowGroup,
      std::shared_ptr<::parquet::WriterProperties> properties =
          ::parquet::WriterProperties::Builder().build())
      : rowsInRowGroup_(rowsInRowGroup),
        pool_(pool),
        finalSink_(std::move(sink)),
        properties_(std::move(properties)) {}

  void write(const RowVectorPtr& data);

  void newRowGroup(int32_t numRows) {
    arrowWriter_->NewRowGroup(numRows);
  }

  void close();

 private:
  const int32_t rowsInRowGroup_;
  int32_t rowsInCurrentGroup_{0};
  memory::MemoryPool& pool_;
  std::unique_ptr<dwio::common::DataSink> finalSink_;
  std::shared_ptr<DataBufferSink> stream_;
  std::unique_ptr<::parquet::arrow::FileWriter> arrowWriter_;
  std::shared_ptr<::parquet::WriterProperties> properties_;
};

} // namespace facebook::velox::parquet
