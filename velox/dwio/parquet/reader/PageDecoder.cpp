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

#include "velox/dwio/parquet/reader/PageDecoder.h"
#include "velox/dwio/dwrf/reader/ColumnReader.h"

#include <thrift/protocol/TCompactProtocol.h>
#include "ThriftTransport.h"

namespace facebook::velox::parquet {

void PageDecoder::readNextPage(int64_t row) {
  defineDecoder_.reset();
  repeatDecoder_.reset();
  // 'rowOfPage_' is the row number of the first row of the next page.
  rowOfPage_ += numRowsInPage_;
  for (;;) {
    PageHeader pageHeader = readPageHeader(chunkSize_ - pageStart_);
    pageStart_ = pageDataStart_ + pageHeader.compressed_page_size;

    switch (pageHeader.type) {
      case PageType::DATA_PAGE:
        prepareDataPageV1(pageHeader, row);
        break;
      case PageType::DATA_PAGE_V2:
        prepareDataPageV2(pageHeader, row);
        break;
      case PageType::DICTIONARY_PAGE:
        prepareDictionary(pageHeader);
        continue;
      default:
        break; // ignore INDEX page type and any other custom extensions
    }
    if (row < rowOfPage_ + numRowsInPage_) {
      break;
    }
    rowOfPage_ += numRowsInPage_;
    dwio::common::skipBytes(
        pageHeader.compressed_page_size,
        inputStream_.get(),
        bufferStart_,
        bufferEnd_);
  }
}

PageHeader PageDecoder::readPageHeader(int64_t remainingSize) {
  // Note that sizeof(PageHeader) may be longer than actually read
  std::shared_ptr<ThriftBufferedTransport> transport;
  std::unique_ptr<
      apache::thrift::protocol::TCompactProtocolT<ThriftBufferedTransport>>
      protocol;
  char copy[sizeof(PageHeader)];
  bool wasInBuffer = false;
  if (bufferEnd_ == bufferStart_) {
    const void* buffer;
    int32_t size;
    inputStream_->Next(&buffer, &size);
    bufferStart_ = reinterpret_cast<const char*>(buffer);
    bufferEnd_ = bufferStart_ + size;
  }
  if (bufferEnd_ - bufferStart_ >= sizeof(PageHeader)) {
    wasInBuffer = true;
    transport = std::make_shared<ThriftBufferedTransport>(
        bufferStart_, sizeof(PageHeader));
    protocol = std::make_unique<
        apache::thrift::protocol::TCompactProtocolT<ThriftBufferedTransport>>(
        transport);
  } else {
    dwio::common::readBytes(
        std::min<int64_t>(remainingSize, sizeof(PageHeader)),
        inputStream_.get(),
        &copy,
        bufferStart_,
        bufferEnd_);

    transport =
        std::make_shared<ThriftBufferedTransport>(copy, sizeof(PageHeader));
    protocol = std::make_unique<
        apache::thrift::protocol::TCompactProtocolT<ThriftBufferedTransport>>(
        transport);
  }
  PageHeader pageHeader;
  uint64_t readBytes = pageHeader.read(protocol.get());
  pageDataStart_ = pageStart_ + readBytes;
  // Unread the bytes that were not consumed.
  if (wasInBuffer) {
    bufferStart_ += readBytes;
  } else {
    std::vector<uint64_t> start = {pageDataStart_};
    dwio::common::PositionProvider position(start);
    inputStream_->seekToPosition(position);
    bufferStart_ = bufferEnd_ = nullptr;
  }
  return pageHeader;
}

const char* PageDecoder::readBytes(int32_t size, BufferPtr& copy) {
  if (bufferEnd_ == bufferStart_) {
    const void* buffer = nullptr;
    int32_t size = 0;
    if (!inputStream_->Next(&buffer, &size)) {
      VELOX_FAIL("Read past end");
    }
    bufferStart_ = reinterpret_cast<const char*>(buffer);
    bufferEnd_ = bufferStart_ + size;
  }
  if (bufferEnd_ - bufferStart_ >= size) {
    bufferStart_ += size;
    return bufferStart_ - size;
  }
  dwrf::detail::ensureCapacity<char>(copy, size, &pool_);
  dwio::common::readBytes(
      size,
      inputStream_.get(),
      copy->asMutable<char>(),
      bufferStart_,
      bufferEnd_);
  return copy->as<char>();
}

const char* FOLLY_NONNULL PageDecoder::uncompressData(
    const char* pageData,
    uint32_t compressedSize,
    uint32_t uncompressedSize) {
  switch (codec_) {
    case CompressionCodec::UNCOMPRESSED:
      return pageData;
    case CompressionCodec::GZIP:
    case CompressionCodec::ZSTD:
    default:
      VELOX_FAIL("Unsupported Parquet compression type ", codec_);
  }
}

void PageDecoder::prepareDataPageV1(const PageHeader& pageHeader, int64_t row) {
  VELOX_CHECK(
      pageHeader.type == PageType::DATA_PAGE &&
      pageHeader.__isset.data_page_header);
  numRowsInPage_ = pageHeader.data_page_header.num_values;
  if (numRowsInPage_ + rowOfPage_ <= row) {
    return;
  }
  pageData_ = readBytes(pageHeader.compressed_page_size, pageBuffer_);
  if (pageHeader.compressed_page_size != pageHeader.uncompressed_page_size) {
    pageData_ = uncompressData(
        pageData_,
        pageHeader.compressed_page_size,
        pageHeader.uncompressed_page_size);
  }
  auto pageEnd = pageData_ + pageHeader.uncompressed_page_size;
  if (maxRepeat_ > 0) {
    uint32_t repeatLength = readField<int32_t>(pageData_);
    pageData_ += repeatLength;
    repeatDecoder_ = std::make_unique<arrow::util::RleDecoder>(
        reinterpret_cast<const uint8_t*>(pageData_),
        repeatLength,
        arrow::bit_util::NumRequiredBits(maxRepeat_));
    pageData_ += repeatLength;
  }

  if (maxDefine_ > 0) {
    auto defineLength = readField<uint32_t>(pageData_);
    defineDecoder_ = std::make_unique<arrow::util::RleDecoder>(
        reinterpret_cast<const uint8_t*>(pageData_),
        defineLength,
        arrow::bit_util::NumRequiredBits(maxDefine_));
    pageData_ += defineLength;
  }
  encodedDataSize_ = pageEnd - pageData_;

  encoding_ = pageHeader.data_page_header.encoding;
  makeDecoder();
}

void PageDecoder::prepareDataPageV2(const PageHeader& pageHeader, int64_t row) {
  VELOX_CHECK(pageHeader.__isset.data_page_header_v2);
  numRowsInPage_ = pageHeader.data_page_header_v2.num_values;
  if (numRowsInPage_ + rowOfPage_ <= row) {
    return;
  }

  uint32_t defineLength = maxDefine_ > 0
      ? pageHeader.data_page_header_v2.definition_levels_byte_length
      : 0;
  uint32_t repeatLength = maxRepeat_ > 0
      ? pageHeader.data_page_header_v2.repetition_levels_byte_length
      : 0;
  auto bytes = pageHeader.compressed_page_size;
  pageData_ = readBytes(bytes, pageBuffer_);

  if (repeatLength) {
    repeatDecoder_ = std::make_unique<arrow::util::RleDecoder>(
        reinterpret_cast<const uint8_t*>(pageData_),
        repeatLength,
        arrow::bit_util::NumRequiredBits(maxRepeat_));
  }

  if (maxDefine_ > 0) {
    defineDecoder_ = std::make_unique<arrow::util::RleDecoder>(
        reinterpret_cast<const uint8_t*>(pageData_ + repeatLength),
        defineLength,
        arrow::bit_util::NumRequiredBits(maxDefine_));
  }
  auto levelsSize = repeatLength + defineLength;
  pageData_ += levelsSize;
  if (pageHeader.data_page_header_v2.__isset.is_compressed ||
      pageHeader.data_page_header_v2.is_compressed) {
    pageData_ = uncompressData(
        pageData_,
        pageHeader.compressed_page_size - levelsSize,
        pageHeader.uncompressed_page_size - levelsSize);
  }
  encodedDataSize_ = pageHeader.uncompressed_page_size - levelsSize;
  encoding_ = pageHeader.data_page_header_v2.encoding;
  makeDecoder();
}

void PageDecoder::prepareDictionary(const PageHeader& pageHeader) {
  VELOX_NYI();
}

namespace {
int32_t parquetTypeBytes(parquet::Type::type type) {
  switch (type) {
    case parquet::Type::INT32:
    case parquet::Type::FLOAT:
      return 4;
    case parquet::Type::INT64:
    case parquet::Type::DOUBLE:
      return 8;
    default:
      VELOX_FAIL("Type does not have a byte width {}", type);
  }
}
} // namespace

void PageDecoder::makeDecoder() {
  switch (encoding_) {
    case Encoding::RLE_DICTIONARY:
    case Encoding::PLAIN_DICTIONARY:
    case Encoding::DELTA_BINARY_PACKED:
      VELOX_UNSUPPORTED("Encoding not supported yet");
      break;
    case Encoding::PLAIN:
      directDecoder_ = std::make_unique<dwio::common::DirectDecoder<true>>(
          std::make_unique<dwio::common::SeekableArrayInputStream>(
              pageData_, encodedDataSize_),
          false,
          parquetTypeBytes(type_->parquetType_.value()));
      break;
    default:
      throw std::runtime_error("Unsupported page encoding");
  }
}

void PageDecoder::skip(int64_t numRows) {
  if (!numRows && firstUnvisited_ != rowOfPage_ + numRowsInPage_) {
    // Return if no skip and position not at end of page or before first page.
    return;
  }
  int rowInPage = firstUnvisited_ - rowOfPage_;
  auto toSkip = numRows;
  if (firstUnvisited_ + numRows >= rowOfPage_ + numRowsInPage_) {
    readNextPage(firstUnvisited_ + numRows);
    toSkip -= rowOfPage_ - firstUnvisited_;
  }
  firstUnvisited_ += numRows;

  // Skip nulls
  toSkip = skipNulls(toSkip);

  // Skip the decoder
  if (directDecoder_) {
    directDecoder_->skip(toSkip);
  }
}

int32_t PageDecoder::skipNulls(int32_t numValues) {
  if (!defineDecoder_) {
    return numValues;
  }
  dwrf::detail::ensureCapacity<char>(tempNulls_, numValues, &pool_);
  tempNulls_->setSize(0);
  defineDecoder_->GetBatch<uint8_t>(
      tempNulls_->asMutable<uint8_t>(), numValues);
  auto bytes = tempNulls_->as<uint8_t>();
  int32_t numPresent = 0;
  for (auto i = 0; i < numValues; ++i) {
    numPresent += bytes[i] == 1;
  }
  return numPresent;
}

void PageDecoder::readNullsOnly(int64_t numValues, BufferPtr& buffer) {
  auto toRead = numValues;
  if (buffer) {
    dwrf::detail::ensureCapacity<bool>(buffer, numValues, &pool_);
  }
  nullConcatenation_.reset(buffer);
  while (toRead) {
    auto availableOnPage = rowOfPage_ + numRowsInPage_ - firstUnvisited_;
    if (!availableOnPage) {
      readNextPage(firstUnvisited_);
      availableOnPage = numRowsInPage_;
    }
    auto numRead = std::min(availableOnPage, toRead);
    auto nulls = readNulls(numRead, nullsInReadRange_);
    toRead -= numRead;
    nullConcatenation_.append(nulls, 0, numRead);
  }
}

const uint64_t* PageDecoder::readNulls(int32_t numValues, BufferPtr& buffer) {
  if (!defineDecoder_) {
    buffer = nullptr;
    return nullptr;
  }
  dwrf::detail::ensureCapacity<char>(tempNulls_, numValues, &pool_);
  dwrf::detail::ensureCapacity<bool>(buffer, numValues, &pool_);
  tempNulls_->setSize(0);
  defineDecoder_->GetBatch<uint8_t>(
      tempNulls_->asMutable<uint8_t>(), numValues);
  auto nullBytes = tempNulls_->as<uint8_t>();
  auto intNulls = buffer->asMutable<int32_t>();
  int32_t nullsIndex = 0;
  for (auto i = 0; i < numValues; i += 32) {
    auto flags = xsimd::load_unaligned(nullBytes + i);
    intNulls[nullsIndex++] = simd::toBitMask(flags != 0);
  }
  return buffer->as<uint64_t>();
}

void PageDecoder::startVisit(folly::Range<const vector_size_t*> rows) {
  visitorRows_ = rows.data();
  numVisitorRows_ = rows.size();
  currentVisitorRow_ = 0;
  initialRowOfPage_ = rowOfPage_;
  visitBase_ = firstUnvisited_;
}

bool PageDecoder::rowsForPage(
    dwrf::SelectiveColumnReader& reader,
    folly::Range<const vector_size_t*>& rows,
    const uint64_t* FOLLY_NULLABLE& nulls) {
  if (currentVisitorRow_ == numVisitorRows_) {
    return false;
  }
  int32_t numToVisit;
  // Check if the first row to go to is in the current page. If not, seek to the
  // page that contains the row.
  auto rowZero = visitBase_ + visitorRows_[currentVisitorRow_];
  if (rowZero >= rowOfPage_ + numRowsInPage_) {
    readNextPage(rowZero);
  }
  // Then check how many of the rows to visit are on the same page as the
  // current one.
  int32_t firstOnNextPage = rowOfPage_ + numRowsInPage_ - visitBase_;
  if (firstOnNextPage > visitorRows_[numVisitorRows_ - 1]) {
    // All the remaining rows are on this page.
    numToVisit = numVisitorRows_ - currentVisitorRow_;
  } else {
    // Find the last row in the rows to visit that is on this page.
    auto rangeLeft = folly::Range<const int32_t*>(
        visitorRows_ + currentVisitorRow_,
        numVisitorRows_ - currentVisitorRow_);
    auto it =
        std::lower_bound(rangeLeft.begin(), rangeLeft.end(), firstOnNextPage);
    assert(it != rangeLeft.end());
    assert(it != rangeLeft.begin());
    numToVisit = it - (visitorRows_ + currentVisitorRow_);
  }
  // If the page did not change and this is the first call, we can return a view
  // on the original visitor rows.
  if (rowOfPage_ == initialRowOfPage_ && currentVisitorRow_ == 0) {
    nulls =
        readNulls(visitorRows_[numToVisit - 1] + 1, reader.nullsInReadRange());
    rowNumberBias_ = 0;
    rows = folly::Range<const vector_size_t*>(visitorRows_, numToVisit);
  } else {
    // We scale row numbers to be relative to first on this page.
    auto pageOffset = rowOfPage_ - visitBase_;
    rowNumberBias_ = visitorRows_[currentVisitorRow_];
    auto offsetOnPage = rowNumberBias_ + visitorRows_[currentVisitorRow_];
    skip(rowNumberBias_ - pageOffset);
    // The decoder is positioned at 'visitorRows_[currentVisitorRow_']'
    // We copy the rows to visit with a bias, so that the first to visit has
    // offset 0.
    rowsCopy_->resize(numToVisit);
    for (auto i = 0; i < numToVisit; ++i) {
      (*rowsCopy_)[i] = visitorRows_[i + currentVisitorRow_] - rowNumberBias_;
    }
    nulls = readNulls(rowsCopy_->back() + 1, reader.nullsInReadRange());
    rows = folly::Range<const vector_size_t*>(
        rowsCopy_->data(), rowsCopy_->size());
  }
  reader.prepareNulls(rows, nulls != nullptr, currentVisitorRow_);
  currentVisitorRow_ += numToVisit;
  firstUnvisited_ = visitBase_ + visitorRows_[currentVisitorRow_ - 1] + 1;
  return true;
}

} // namespace facebook::velox::parquet
