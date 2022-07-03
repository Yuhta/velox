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

#include "velox/buffer/Buffer.h"

namespace facebook::velox::dwio::common {

// Concatenates multiple bit vectors and produces these in a single
// Buffer at the end. If only one bits were added, sets the output
// buffer to nullptr.
class BitConcatenation {
 public:
  BitConcatenation(memory::MemoryPool& pool) : pool_(pool) {}

  // Prepares to concatenate bits given to append() or appendOnes()
  // into 'buffer'. 'buffer' is allocated and resized as needed. If
  // 'buffer' is initially nullptr and only ones are appended, buffer
  // may stay nullptr. The size() of 'buffer' is set to the next byte,
  // so the caller must  use numBits() to get the bit count.
  void reset(BufferPtr& outBuffer) {
    buffer_ = &outBuffer;
    numBits_ = 0;
  }

  // Appends 'numBits' consecutive bits from 'bits' starting at bit offset
  // 'begin' to the buffer.
  void
  append(const uint64_t* FOLLY_NULLABLE bits, int32_t begin, int32_t numBits);

  // Appends 'numOnes' ones.
  void appendOnes(int32_t numOnes);

  int32_t numBits() const {
    return numBits_;
  }

 private:
  // Allocates or reallocates '*buffer' to have space for 'numBits_ + newBits'
  // bits.
  void ensureSpace(int32_t newBits);

  void setSize() {
    if (*buffer_) {
      (*buffer_)->setSize(bits::roundUp(numBits_, 8) / 8);
    }
  }

  memory::MemoryPool& pool_;
  BufferPtr* FOLLY_NULLABLE buffer_{nullptr};
  int32_t numBits_{0};
};

} // namespace facebook::velox::dwio::common
