// Copyright (c) 2021, Alliance for Open Media. All rights reserved
//
// This source code is subject to the terms of the BSD 2 Clause License and
// the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
// was not distributed with this source code in the LICENSE file, you can
// obtain it at www.aomedia.org/license/software. If the Alliance for Open
// Media Patent License 1.0 was not distributed with this source code in the
// PATENTS file, you can obtain it at www.aomedia.org/license/patent.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "avifinfo.h"

// Test a random bitstream of random size, whether it is valid or not.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t data_size) {
  AvifInfoStatus previous_status = kAvifInfoNotEnoughData;
  AvifInfoFeatures previous_features = {0};

  // Check the consistency of the returned status and features:
  // for a given size and a status that is not kAvifInfoNotEnoughData, any
  // bigger size (of the same data) should return the same status and features.
  for (size_t size = 0; size < data_size; ++size) {
    AvifInfoFeatures features;
    const AvifInfoStatus status =
        AvifInfoGetWithSize(data, size, &features, data_size);

    if (previous_status != kAvifInfoNotEnoughData &&
        status != previous_status) {
      std::abort();
    }

    if (status == previous_status) {
      if (features.width != previous_features.width ||
          features.height != previous_features.height ||
          features.bit_depth != previous_features.bit_depth ||
          features.num_channels != previous_features.num_channels) {
        std::abort();
      }
    } else if (status == kAvifInfoOk) {
      if (features.width == 0u || features.height == 0u ||
          features.bit_depth == 0u || features.num_channels == 0u) {
        std::abort();
      }
    } else {
      if (features.width != 0u || features.height != 0u ||
          features.bit_depth != 0u || features.num_channels != 0u) {
        std::abort();
      }
    }

    previous_status = status;
    previous_features = features;
  }
  return 0;
}
