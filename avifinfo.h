// Copyright (c) 2021, Alliance for Open Media. All rights reserved
//
// This source code is subject to the terms of the BSD 2 Clause License and
// the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
// was not distributed with this source code in the LICENSE file, you can
// obtain it at www.aomedia.org/license/software. If the Alliance for Open
// Media Patent License 1.0 was not distributed with this source code in the
// PATENTS file, you can obtain it at www.aomedia.org/license/patent.

#ifndef AVIFINFO_H_
#define AVIFINFO_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------

typedef enum {
  kAvifInfoOk,             // The file was correctly parsed and the requested
                           // information was extracted. It is not guaranteed
                           // that the input bitstream is a valid complete
                           // AVIF file.
  kAvifInfoNotEnoughData,  // The input bitstream was correctly parsed until
                           // now but bytes are missing. The request should be
                           // repeated with more input bytes.
  kAvifInfoTooComplex,     // The input bitstream was correctly parsed until
                           // now but it is too complex. The parsing was
                           // stopped to avoid any timeout or crash.
  kAvifInfoInvalidFile,    // The input bitstream is not a valid AVIF file,
                           // truncated or not.
} AvifInfoStatus;

typedef struct {
  uint32_t width, height;  // In number of pixels. Ignores mirror and rotation.
  uint32_t bit_depth;      // Likely 8, 10 or 12 bits per channel per pixel.
  uint32_t num_channels;   // Likely 1, 2, 3 or 4 channels:
                           //   (1 monochrome or 3 colors) + (0 or 1 alpha)
} AvifInfoFeatures;

// Parses the AVIF 'data' and extracts its 'features'.
// 'data' can be partial but must point to the beginning of the AVIF file.
// The 'features' can be parsed in the first 450 bytes of most AVIF files.
// 'features' are set to 0 unless kAvifInfoOk is returned.
AvifInfoStatus AvifInfoGet(const uint8_t* data, size_t data_size,
                           AvifInfoFeatures* features);

// Same as above with an extra argument 'file_size'. If the latter is known,
// please use this version for extra bitstream validation.
AvifInfoStatus AvifInfoGetWithSize(const uint8_t* data, size_t data_size,
                                   AvifInfoFeatures* features,
                                   size_t file_size);

//------------------------------------------------------------------------------

// If needed, avifinfo.h and avifinfo.c can be merged into a single file:
//   1. Replace this block comment by the content of avifinfo.c
//   2. Discard #include "./avifinfo.h" and move other includes to the top
//   3. Mark AvifInfoGet*() declarations and definitions as static
// This procedure can be useful when only one translation unit uses avifinfo,
// whether it includes the merged .h or the merged code is inserted into a file.

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AVIFINFO_H_
