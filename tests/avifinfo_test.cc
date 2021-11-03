// Copyright (c) 2021, Alliance for Open Media. All rights reserved
//
// This source code is subject to the terms of the BSD 2 Clause License and
// the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
// was not distributed with this source code in the LICENSE file, you can
// obtain it at www.aomedia.org/license/software. If the Alliance for Open
// Media Patent License 1.0 was not distributed with this source code in the
// PATENTS file, you can obtain it at www.aomedia.org/license/patent.

#include "avifinfo.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include "gtest/gtest.h"

namespace {

using Data = std::vector<uint8_t>;

Data LoadFile(const char file_name[]) {
  std::ifstream file(file_name, std::ios::binary | std::ios::ate);
  if (!file) return Data();
  const auto file_size = file.tellg();
  Data bytes(file_size * sizeof(char));
  file.seekg(0);  // Rewind.
  return file.read(reinterpret_cast<char*>(bytes.data()), file_size) ? bytes
                                                                     : Data();
}

//------------------------------------------------------------------------------
// Positive tests

TEST(AvifInfoGetTest, WithoutFileSize) {
  const Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());

  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGet(input.data(), input.size(), &features), kAvifInfoOk);
  EXPECT_EQ(features.width, 1u);
  EXPECT_EQ(features.height, 1u);
  EXPECT_EQ(features.bit_depth, 8u);
  EXPECT_EQ(features.num_channels, 3u);
}

TEST(AvifInfoGetTest, WithFileSize) {
  const Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());

  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGetWithSize(input.data(), /*data_size=*/input.size(),
                                &features, /*file_size=*/input.size()),
            kAvifInfoOk);
  EXPECT_EQ(features.width, 1u);
  EXPECT_EQ(features.height, 1u);
  EXPECT_EQ(features.bit_depth, 8u);
  EXPECT_EQ(features.num_channels, 3u);
}

TEST(AvifInfoGetTest, WithShorterSize) {
  const Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());

  AvifInfoFeatures features;
  // No more than 'file_size' bytes should be read, even if more are passed.
  EXPECT_EQ(AvifInfoGetWithSize(input.data(), /*data_size=*/input.size() * 10,
                                &features,
                                /*file_size=*/input.size()),
            kAvifInfoOk);
  EXPECT_EQ(features.width, 1u);
  EXPECT_EQ(features.height, 1u);
  EXPECT_EQ(features.bit_depth, 8u);
  EXPECT_EQ(features.num_channels, 3u);
}

TEST(AvifInfoGetTest, EnoughBytes) {
  Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());
  // Truncate 'input' just after the required information (discard AV1 box).
  const uint8_t kMdatTag[] = {'m', 'd', 'a', 't'};
  input.resize(std::search(input.begin(), input.end(), kMdatTag, kMdatTag + 4) -
               input.begin());

  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGet(input.data(), input.size(), &features), kAvifInfoOk);
  EXPECT_EQ(features.width, 1u);
  EXPECT_EQ(features.height, 1u);
  EXPECT_EQ(features.bit_depth, 8u);
  EXPECT_EQ(features.num_channels, 3u);
}

TEST(AvifInfoGetTest, Null) {
  const Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());

  EXPECT_EQ(AvifInfoGet(input.data(), input.size(), nullptr), kAvifInfoOk);
  EXPECT_EQ(
      AvifInfoGetWithSize(input.data(), input.size(), nullptr, input.size()),
      kAvifInfoOk);
}

//------------------------------------------------------------------------------
// Negative tests

TEST(AvifInfoGetTest, Empty) {
  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGet(nullptr, 0, &features), kAvifInfoNotEnoughData);
  EXPECT_EQ(features.width, 0u);
  EXPECT_EQ(features.height, 0u);
  EXPECT_EQ(features.bit_depth, 0u);
  EXPECT_EQ(features.num_channels, 0u);
}

TEST(AvifInfoGetTest, NotEnoughBytes) {
  Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());
  // Truncate 'input' before having all the required information.
  const uint8_t kIpmaTag[] = {'i', 'p', 'm', 'a'};
  input.resize(std::search(input.begin(), input.end(), kIpmaTag, kIpmaTag + 4) -
               input.begin());

  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGet(input.data(), input.size(), &features),
            kAvifInfoNotEnoughData);
}

TEST(AvifInfoGetTest, Broken) {
  Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());
  // Change "ispe" to "aspe".
  const uint8_t kIspeTag[] = {'i', 's', 'p', 'e'};
  std::search(input.begin(), input.end(), kIspeTag, kIspeTag + 4)[0] = 'a';

  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGet(input.data(), input.size(), &features),
            kAvifInfoInvalidFile);
  EXPECT_EQ(features.width, 0u);
  EXPECT_EQ(features.height, 0u);
  EXPECT_EQ(features.bit_depth, 0u);
  EXPECT_EQ(features.num_channels, 0u);
}

TEST(AvifInfoGetTest, MetaBoxIsTooBig) {
  Data input = LoadFile("avifinfo_test_1x1.avif");
  ASSERT_FALSE(input.empty());
  // Change "meta" box size to the maximum size 2^32-1.
  const uint8_t kMetaTag[] = {'m', 'e', 't', 'a'};
  auto meta_tag =
      std::search(input.begin(), input.end(), kMetaTag, kMetaTag + 4);
  meta_tag[-4] = meta_tag[-3] = meta_tag[-2] = meta_tag[-1] = 255;

  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGet(input.data(), input.size(), &features),
            kAvifInfoTooComplex);
  EXPECT_EQ(features.width, 0u);
  EXPECT_EQ(features.height, 0u);
  EXPECT_EQ(features.bit_depth, 0u);
  EXPECT_EQ(features.num_channels, 0u);
}

TEST(AvifInfoGetTest, TooManyBoxes) {
  // Create a valid-ish input with too many boxes to parse.
  Data input = {0,   0,   0,   16,  'f', 't', 'y', 'p',
                'a', 'v', 'i', 'f', 0,   0,   0,   0};
  const uint32_t kNumBoxes = 12345;
  input.reserve(input.size() + kNumBoxes * 8);
  for (uint32_t i = 0; i < kNumBoxes; ++i) {
    const uint8_t kBox[] = {0, 0, 0, 8, 'a', 'b', 'c', 'd'};
    input.insert(input.end(), kBox, kBox + kBox[3]);
  }

  AvifInfoFeatures features;
  EXPECT_EQ(AvifInfoGet(reinterpret_cast<uint8_t*>(input.data()),
                        input.size() * 4, &features),
            kAvifInfoTooComplex);
}

//------------------------------------------------------------------------------

}  // namespace
