// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include <string_view>

#include "gtest/gtest.h"
#include "pw_kvs/crc16_checksum.h"
#include "pw_kvs/in_memory_fake_flash.h"
#include "pw_kvs/internal/hash.h"
#include "pw_kvs/key_value_store.h"
#include "pw_kvs_private/byte_utils.h"

namespace pw::kvs {
namespace {

using std::byte;
using std::string_view;

constexpr size_t kMaxEntries = 256;
constexpr size_t kMaxUsableSectors = 256;

constexpr uint32_t SimpleChecksum(span<const byte> data, uint32_t state = 0) {
  for (byte b : data) {
    state += uint32_t(b);
  }
  return state;
}

class SimpleChecksumAlgorithm final : public ChecksumAlgorithm {
 public:
  SimpleChecksumAlgorithm()
      : ChecksumAlgorithm(as_bytes(span(&checksum_, 1))) {}

  void Reset() override { checksum_ = 0; }

  void Update(span<const byte> data) override {
    checksum_ = SimpleChecksum(data, checksum_);
  }

 private:
  uint32_t checksum_;
} checksum;

// Returns a buffer containing the necessary padding for an entry.
template <size_t kAlignmentBytes, size_t kKeyLength, size_t kValueSize>
constexpr auto EntryPadding() {
  constexpr size_t content =
      sizeof(internal::EntryHeader) + kKeyLength + kValueSize;
  return std::array<byte, Padding(content, kAlignmentBytes)>{};
}

// Creates a buffer containing a valid entry at compile time.
template <size_t kAlignmentBytes = sizeof(internal::EntryHeader),
          size_t kKeyLengthWithNull,
          size_t kValueSize>
constexpr auto MakeValidEntry(uint32_t magic,
                              uint32_t id,
                              const char (&key)[kKeyLengthWithNull],
                              const std::array<byte, kValueSize>& value) {
  constexpr size_t kKeyLength = kKeyLengthWithNull - 1;

  auto data = AsBytes(magic,
                      uint32_t(0),
                      uint8_t(kAlignmentBytes / 16 - 1),
                      uint8_t(kKeyLength),
                      uint16_t(kValueSize),
                      id,
                      ByteStr(key),
                      span(value),
                      EntryPadding<kAlignmentBytes, kKeyLength, kValueSize>());

  // Calculate the checksum
  uint32_t checksum = SimpleChecksum(data);
  for (size_t i = 0; i < sizeof(checksum); ++i) {
    data[4 + i] = byte(checksum & 0xff);
    checksum >>= 8;
  }

  return data;
}

constexpr uint32_t kMagic = 0xc001beef;
constexpr EntryFormat kFormat{.magic = kMagic, .checksum = &checksum};

constexpr auto kEntry1 = MakeValidEntry(kMagic, 1, "key1", ByteStr("value1"));
constexpr auto kEntry2 = MakeValidEntry(kMagic, 3, "k2", ByteStr("value2"));

class KvsErrorHandling : public ::testing::Test {
 protected:
  KvsErrorHandling()
      : flash_(internal::Entry::kMinAlignmentBytes),
        partition_(&flash_),
        kvs_(&partition_, kFormat) {}

  void InitFlashTo(span<const byte> contents) {
    partition_.Erase();
    std::memcpy(flash_.buffer().data(), contents.data(), contents.size());
  }

  FakeFlashBuffer<512, 4> flash_;
  FlashPartition partition_;
  KeyValueStoreBuffer<kMaxEntries, kMaxUsableSectors> kvs_;
};

TEST_F(KvsErrorHandling, Init_Ok) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  EXPECT_EQ(Status::OK, kvs_.Init());
  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorHandling, Init_DuplicateEntries_ReturnsDataLossButReadsEntry) {
  InitFlashTo(AsBytes(kEntry1, kEntry1));

  EXPECT_EQ(Status::DATA_LOSS, kvs_.Init());
  byte buffer[64];
  EXPECT_EQ(Status::OK, kvs_.Get("key1", buffer).status());
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("k2", buffer).status());
}

TEST_F(KvsErrorHandling, Init_CorruptEntry_FindsSubsequentValidEntry) {
  // Corrupt each byte in the first entry once.
  for (size_t i = 0; i < kEntry1.size(); ++i) {
    InitFlashTo(AsBytes(kEntry1, kEntry2));
    flash_.buffer()[i] = byte(int(flash_.buffer()[i]) + 1);

    ASSERT_EQ(Status::DATA_LOSS, kvs_.Init());
    byte buffer[64];
    ASSERT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
    ASSERT_EQ(Status::OK, kvs_.Get("k2", buffer).status());
  }
}

TEST_F(KvsErrorHandling, Init_ReadError_IsNotInitialized) {
  InitFlashTo(AsBytes(kEntry1, kEntry2));

  flash_.InjectReadError(
      FlashError::InRange(Status::UNAUTHENTICATED, kEntry1.size()));

  EXPECT_EQ(Status::UNKNOWN, kvs_.Init());
  EXPECT_FALSE(kvs_.initialized());
}

TEST_F(KvsErrorHandling, Put_WriteFailure_EntryNotAdded) {
  ASSERT_EQ(Status::OK, kvs_.Init());
  flash_.InjectWriteError(FlashError::Unconditional(Status::UNAVAILABLE));

  EXPECT_EQ(Status::UNAVAILABLE, kvs_.Put("key1", ByteStr("value1")));

  byte buffer[64];
  EXPECT_EQ(Status::NOT_FOUND, kvs_.Get("key1", buffer).status());
  ASSERT_TRUE(kvs_.empty());
}

}  // namespace
}  // namespace pw::kvs
