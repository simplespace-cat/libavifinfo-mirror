// Copyright (c) 2021, Alliance for Open Media. All rights reserved
//
// This source code is subject to the terms of the BSD 2 Clause License and
// the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
// was not distributed with this source code in the LICENSE file, you can
// obtain it at www.aomedia.org/license/software. If the Alliance for Open
// Media Patent License 1.0 was not distributed with this source code in the
// PATENTS file, you can obtain it at www.aomedia.org/license/patent.

#include "avifinfo.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

//------------------------------------------------------------------------------

// Status returned when reading the content of a box (or file).
typedef enum {
  kFound,     // Input correctly parsed and information retrieved.
  kNotFound,  // Input correctly parsed but information is missing or elsewhere.
  kTruncated,  // Input correctly parsed until missing bytes to continue.
  kAborted,  // Input correctly parsed until stopped to avoid timeout or crash.
  kInvalid,  // Input incorrectly parsed.
} AvifInfoInternalStatus;

// uint32_t is used everywhere in this file. It is unlikely to be insufficient
// to parse AVIF headers. Clamp any input to 2^32-1 for simplicity.
static const uint32_t kAvifInfoInternalMaxSize = UINT32_MAX;

// Reads an unsigned integer from 'input' with most significant bits first.
// 'input' must be at least 'num_bytes'-long.
static uint32_t AvifInfoInternalReadBigEndian(const uint8_t* input,
                                              uint32_t num_bytes) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < num_bytes; ++i) {
    value = (value << 8) | input[i];
  }
  return value;
}

//------------------------------------------------------------------------------
// Convenience macros.

#if defined(AVIFINFO_LOG_ERROR)  // Toggle to log encountered issues.
static void AvifInfoInternalLogError(const char* file, int line,
                                     AvifInfoInternalStatus status) {
  const char* kStr[] = {"Found", "NotFound", "Truncated", "Invalid", "Aborted"};
  fprintf(stderr, "  %s:%d: %s\n", file, line, kStr[status]);
  // Set a breakpoint here to catch the first detected issue.
}
#define AVIFINFO_RETURN(check_status)                               \
  do {                                                              \
    const AvifInfoInternalStatus status_checked = (check_status);   \
    if (status_checked != kFound && status_checked != kNotFound) {  \
      AvifInfoInternalLogError(__FILE__, __LINE__, status_checked); \
    }                                                               \
    return status_checked;                                          \
  } while (0)
#else
#define AVIFINFO_RETURN(check_status) \
  do {                                \
    return (check_status);            \
  } while (0)
#endif

#define AVIFINFO_CHECK(check_condition, check_status)      \
  do {                                                     \
    if (!(check_condition)) AVIFINFO_RETURN(check_status); \
  } while (0)
#define AVIFINFO_CHECK_STATUS_IS(check_status, expected_status)            \
  do {                                                                     \
    const AvifInfoInternalStatus status_returned = (check_status);         \
    AVIFINFO_CHECK(status_returned == (expected_status), status_returned); \
  } while (0)
#define AVIFINFO_CHECK_FOUND(check_status) \
  AVIFINFO_CHECK_STATUS_IS((check_status), kFound)
#define AVIFINFO_CHECK_NOT_FOUND(check_status) \
  AVIFINFO_CHECK_STATUS_IS((check_status), kNotFound)

//------------------------------------------------------------------------------
// Box header parsing and various size checks.

typedef struct {
  uint32_t size;              // In bytes.
  const uint8_t* type;        // Points to four characters.
  uint32_t version;           // 0 or actual version if this is a full box.
  uint32_t flags;             // 0 or actual value if this is a full box.
  uint32_t content_size;      // 'size' minus the header size.
  uint32_t content_position;  // Position in bytes of the 'content' of this box
                              // relative to its container.
  const uint8_t* content;     // Content bytes of this box (after its header).
} AvifInfoInternalBox;

// Reads the header of a 'box' starting at 'bytes + position'.
// 'num_bytes' is the number of available 'bytes'.
// 'max_num_bytes' is the size of the container of the 'box' (either the file
// itself or the content of the parent of the 'box').
static AvifInfoInternalStatus AvifInfoInternalParseBox(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t max_num_bytes,
    uint32_t position, uint32_t* num_parsed_boxes, AvifInfoInternalBox* box) {
  // See ISO/IEC 14496-12:2012(E) 4.2
  AVIFINFO_CHECK(position <= kAvifInfoInternalMaxSize - 8, kAborted);
  AVIFINFO_CHECK(position + 8 <= max_num_bytes, kInvalid);  // box size+type
  AVIFINFO_CHECK(position + 4 <= num_bytes, kTruncated);    // 32b size
  box->size = AvifInfoInternalReadBigEndian(bytes + position, sizeof(uint32_t));
  // Note: 'box->size==1' means 64b size should be read.
  //       'box->size==0' means this box extends to all remaining bytes.
  //       These two use cases are not handled here for simplicity.
  AVIFINFO_CHECK(box->size >= 2, kAborted);
  AVIFINFO_CHECK(box->size >= 8, kInvalid);  // box 32b size + 32b type
  AVIFINFO_CHECK(box->size <= kAvifInfoInternalMaxSize - position, kAborted);
  AVIFINFO_CHECK(position + box->size <= max_num_bytes, kInvalid);
  AVIFINFO_CHECK(position + 8 <= num_bytes, kTruncated);
  box->type = bytes + position + 4;

  const int has_fullbox_header =
      !memcmp(box->type, "meta", 4) || !memcmp(box->type, "pitm", 4) ||
      !memcmp(box->type, "ipma", 4) || !memcmp(box->type, "ispe", 4) ||
      !memcmp(box->type, "pixi", 4) || !memcmp(box->type, "iref", 4) ||
      !memcmp(box->type, "auxC", 4);
  const uint32_t box_header_size = (has_fullbox_header ? 12 : 8);
  AVIFINFO_CHECK(box->size >= box_header_size, kInvalid);
  box->content_position = position + box_header_size;
  AVIFINFO_CHECK(box->content_position <= num_bytes, kTruncated);
  box->content_size = box->size - box_header_size;
  box->content = bytes + box->content_position;
  // Avoid timeouts. The maximum number of parsed boxes is arbitrary.
  ++*num_parsed_boxes;
  AVIFINFO_CHECK(*num_parsed_boxes < 4096, kAborted);

  box->version = 0;
  box->flags = 0;
  if (has_fullbox_header) {
    box->version = AvifInfoInternalReadBigEndian(bytes + position + 8, 1);
    box->flags = AvifInfoInternalReadBigEndian(bytes + position + 9, 3);
    // See AV1 Image File Format (AVIF) 8.1
    // at https://aomediacodec.github.io/av1-avif/#avif-boxes (available when
    // https://github.com/AOMediaCodec/av1-avif/pull/170 is merged).
    uint32_t is_parsable = 1;
    if (!memcmp(box->type, "meta", 4)) is_parsable = (box->version <= 0);
    if (!memcmp(box->type, "pitm", 4)) is_parsable = (box->version <= 1);
    if (!memcmp(box->type, "ipma", 4)) is_parsable = (box->version <= 1);
    if (!memcmp(box->type, "ispe", 4)) is_parsable = (box->version <= 0);
    if (!memcmp(box->type, "pixi", 4)) is_parsable = (box->version <= 0);
    if (!memcmp(box->type, "iref", 4)) is_parsable = (box->version <= 1);
    if (!memcmp(box->type, "auxC", 4)) is_parsable = (box->version <= 0);
    // Instead of considering this file as invalid, skip unparsable boxes.
    if (!is_parsable) box->type = (const uint8_t*)"\0skip";
  }
  return kFound;
}

// Returns kFound if 'min_size' bytes can be read from the 'box.content' now.
// 'num_bytes' is the number of available bytes of the parent of the 'box'.
static AvifInfoInternalStatus AccessContent(AvifInfoInternalBox* box,
                                            uint32_t num_bytes,
                                            uint32_t min_size) {
  AVIFINFO_CHECK(box->content_size >= min_size, kInvalid);
  AVIFINFO_CHECK(box->content_position + min_size <= num_bytes, kTruncated);
  return kFound;
}

//------------------------------------------------------------------------------
// Search if the file identifies itself as AVIF through an "ftyp" box.

static AvifInfoInternalStatus ParseFileForBrand(const uint8_t* bytes,
                                                uint32_t num_bytes,
                                                uint32_t file_size,
                                                uint32_t* num_parsed_boxes) {
  uint32_t position = 0;  // Within 'bytes' (that points to first byte of file).
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, file_size, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "ftyp", 4)) {
      // Iterate over brands. See ISO/IEC 14496-12:2012(E) 4.3.1
      AVIFINFO_CHECK(box.content_size >= 8, kInvalid);  // major_brand + version
      for (uint32_t i = 0; i < box.content_size; i += 4) {
        AVIFINFO_CHECK_FOUND(AccessContent(&box, num_bytes, i + 4));
        if (i == 4) continue;  // Skip minor_version.
        if (!memcmp(box.content + i, "avif", 4) ||
            !memcmp(box.content + i, "avis", 4)) {
          return kFound;
        }
      }
      AVIFINFO_RETURN(kInvalid);  // Only one "ftyp" allowed per file.
    }
    position += box.size;
    // File is valid only if the end of at least one box is at the same position
    // as the end of the container. Oddities are caught when parsing further.
  } while (position != file_size);
  AVIFINFO_RETURN(kInvalid);  // There should be one "ftyp" box.
}

//------------------------------------------------------------------------------
// Search the primary item ID through "meta > pitm" boxes.

static AvifInfoInternalStatus ParseMetaForPrimaryItemId(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t max_num_bytes,
    uint32_t* num_parsed_boxes, uint32_t* primary_item_id) {
  uint32_t position = 0;  // Within 'bytes' (first byte of "meta" box content).
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "pitm", 4)) {
      // See ISO/IEC 14496-12:2015(E) 8.11.4.2
      const uint32_t num_bytes_per_id = (box.version == 0) ? 2 : 4;
      AVIFINFO_CHECK_FOUND(AccessContent(&box, num_bytes, num_bytes_per_id));
      *primary_item_id =
          AvifInfoInternalReadBigEndian(box.content + 0, num_bytes_per_id);
      return kFound;
    }
    position += box.size;
  } while (position != max_num_bytes);

  // According to ISO/IEC 14496-12:2012(E) 8.11.1.1, there is at most one "meta"
  // per file. No "pitm" until now means never.
  AVIFINFO_RETURN(kInvalid);
}

static AvifInfoInternalStatus ParseFileForPrimaryItemId(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t file_size,
    uint32_t* num_parsed_boxes, uint32_t* primary_item_id) {
  uint32_t position = 0;  // Within 'bytes' (that points to first byte of file).
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, file_size, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "meta", 4)) {
      return ParseMetaForPrimaryItemId(
          box.content, num_bytes - box.content_position, box.content_size,
          num_parsed_boxes, primary_item_id);
    }
    position += box.size;
  } while (position != file_size);
  AVIFINFO_RETURN(kInvalid);  // No "meta" is an issue.
}

//------------------------------------------------------------------------------
// Search the features of an item given its ID through "meta > iprp" boxes.

static AvifInfoInternalStatus ParseIpcoForFeaturesInProperty(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t max_num_bytes,
    uint32_t target_property_index, uint32_t* num_parsed_boxes,
    AvifInfoFeatures* features) {
  uint32_t position = 0;
  uint32_t box_index = 1;  // 1-based index. Used for iterating over properties.
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (box_index != target_property_index) {
      // Skip.
    } else if (features->width == 0 && !memcmp(box.type, "ispe", 4)) {
      // See ISO/IEC 23008-12:2017(E) 6.5.3.2
      AVIFINFO_CHECK_FOUND(AccessContent(&box, num_bytes, 4 + 4));
      features->width = AvifInfoInternalReadBigEndian(box.content + 0, 4);
      features->height = AvifInfoInternalReadBigEndian(box.content + 4, 4);
      AVIFINFO_CHECK(features->width != 0 && features->height != 0, kInvalid);
      return kFound;
    } else if (features->num_channels == 0 && !memcmp(box.type, "pixi", 4)) {
      // See ISO/IEC 23008-12:2017(E) 6.5.6.2
      AVIFINFO_CHECK_FOUND(AccessContent(&box, num_bytes, 1));
      features->num_channels =
          AvifInfoInternalReadBigEndian(box.content + 0, 1);
      AVIFINFO_CHECK(features->num_channels >= 1, kInvalid);
      AVIFINFO_CHECK_FOUND(
          AccessContent(&box, num_bytes, 1 + features->num_channels));
      features->bit_depth = AvifInfoInternalReadBigEndian(box.content + 1, 1);
      AVIFINFO_CHECK(features->bit_depth >= 1, kInvalid);
      for (uint32_t i = 1; i < features->num_channels; ++i) {
        const uint32_t bit_depth =
            AvifInfoInternalReadBigEndian(box.content + 1 + i, 1);
        // Bit depth should be the same for all channels.
        AVIFINFO_CHECK(bit_depth == features->bit_depth, kInvalid);
      }
      return kFound;
    } else if (features->num_channels == 0 && !memcmp(box.type, "av1C", 4)) {
      // See AV1 Codec ISO Media File Format Binding 2.3.1
      // at https://aomediacodec.github.io/av1-isobmff/#av1c
      // Only parse the necessary third byte. Assume that the others are valid.
      AVIFINFO_CHECK_FOUND(AccessContent(&box, num_bytes, 3));
      const uint32_t fields = AvifInfoInternalReadBigEndian(box.content + 2, 1);
      const int high_bitdepth = (fields & 0x40) != 0;
      const int twelve_bit = (fields & 0x20) != 0;
      const int monochrome = (fields & 0x10) != 0;
      if (twelve_bit) {
        AVIFINFO_CHECK(high_bitdepth, kInvalid);
      }
      features->num_channels = monochrome ? 1 : 3;
      features->bit_depth = high_bitdepth ? twelve_bit ? 12 : 10 : 8;
      return kFound;
    }
    ++box_index;
    position += box.size;
  } while (position != max_num_bytes && box_index <= target_property_index);
  AVIFINFO_RETURN(kNotFound);
}

static AvifInfoInternalStatus ParseIprpForFeaturesInProperty(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t max_num_bytes,
    uint32_t target_property_index, uint32_t* num_parsed_boxes,
    AvifInfoFeatures* features) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "ipco", 4)) {
      return ParseIpcoForFeaturesInProperty(
          box.content, num_bytes - box.content_position, box.content_size,
          target_property_index, num_parsed_boxes, features);
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kInvalid);  // No "ipco" in "iprp" is an issue.
}

static AvifInfoInternalStatus ParseIprpForFeatures(const uint8_t* bytes,
                                                   uint32_t num_bytes,
                                                   uint32_t max_num_bytes,
                                                   uint32_t target_item_id,
                                                   uint32_t* num_parsed_boxes,
                                                   AvifInfoFeatures* features) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "ipma", 4)) {
      // See ISO/IEC 23008-12:2017(E) 9.3.2
      AVIFINFO_CHECK_FOUND(AccessContent(&box, num_bytes, 4));
      const uint32_t entry_count =
          AvifInfoInternalReadBigEndian(box.content + 0, 4);
      uint32_t offset = 4;
      const uint32_t id_num_bytes = (box.version < 1) ? 2 : 4;
      const uint32_t index_num_bytes = (box.flags & 1) ? 2 : 1;
      const uint32_t essential_bit_mask = (box.flags & 1) ? 0x8000 : 0x80;

      for (uint32_t entry = 0; entry < entry_count; ++entry) {
        AVIFINFO_CHECK_FOUND(
            AccessContent(&box, num_bytes, offset + id_num_bytes + 1));
        const uint32_t item_id =
            AvifInfoInternalReadBigEndian(box.content + offset, id_num_bytes);

        offset += id_num_bytes;
        const uint32_t association_count =
            AvifInfoInternalReadBigEndian(box.content + offset, 1);
        offset += 1;

        for (uint32_t property = 0; property < association_count; ++property) {
          AVIFINFO_CHECK_FOUND(
              AccessContent(&box, num_bytes, offset + index_num_bytes));
          const uint32_t value = AvifInfoInternalReadBigEndian(
              box.content + offset, index_num_bytes);
          offset += index_num_bytes;

          if (item_id == target_item_id) {
            // const int essential = (value & essential_bit_mask);  // Unused.
            const uint32_t property_index = (value & ~essential_bit_mask);

            // Parse again at the same "iprp" level to find the associated
            // "ipco" and the "ispe", "pixi" or "av1C" within.
            const AvifInfoInternalStatus status =
                ParseIprpForFeaturesInProperty(bytes, num_bytes, max_num_bytes,
                                               property_index, num_parsed_boxes,
                                               features);
            if (status != kFound) {
              // Stop in case of error, carry on if not found.
              AVIFINFO_CHECK_NOT_FOUND(status);
            } else if (features->width != 0 && features->height != 0 &&
                       features->num_channels != 0 &&
                       features->bit_depth != 0) {
              return kFound;  // Found everything. Otherwise carry on.
            }
          }
        }
      }

      // According to ISO/IEC 14496-12:2012(E) 8.11.1.1, there is at most one
      // "meta" per file. According to ISO/IEC 23008-12:2017(E) 9.3.1, there is
      // exactly one "ipma" per "iprp" and at most one "iprp" per "meta".
      // The primary properties shall have been found now.
      if (features->width != 0 && features->height != 0) {
        // Exception: The bit depth and number of channels may be referenced
        //            in a tile and not in the primary item of item type "grid".
        return kNotFound;  // Continue the search at a higher level.
      }
      AVIFINFO_RETURN(kInvalid);
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kInvalid);  // No "ipma" in "iprp" is an issue.
}

static AvifInfoInternalStatus ParseMetaForFeatures(const uint8_t* bytes,
                                                   uint32_t num_bytes,
                                                   uint32_t max_num_bytes,
                                                   uint32_t target_item_id,
                                                   uint32_t* num_parsed_boxes,
                                                   AvifInfoFeatures* features) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "iprp", 4)) {
      return ParseIprpForFeatures(box.content, num_bytes - box.content_position,
                                  box.content_size, target_item_id,
                                  num_parsed_boxes, features);
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kInvalid);  // No "iprp" in "meta" is an issue.
}

static AvifInfoInternalStatus ParseFileForFeatures(const uint8_t* bytes,
                                                   uint32_t num_bytes,
                                                   uint32_t file_size,
                                                   uint32_t target_item_id,
                                                   uint32_t* num_parsed_boxes,
                                                   AvifInfoFeatures* features) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, file_size, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "meta", 4)) {
      return ParseMetaForFeatures(box.content, num_bytes - box.content_position,
                                  box.content_size, target_item_id,
                                  num_parsed_boxes, features);
    }
    position += box.size;
  } while (position != file_size);
  AVIFINFO_RETURN(kInvalid);  // No "meta" is an issue.
}

//------------------------------------------------------------------------------
// Search if a tile contains features through "meta > iref > dimg" boxes.

static AvifInfoInternalStatus ParseIrefForFeaturesInTiles(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t max_num_bytes,
    const uint8_t* meta_bytes, uint32_t meta_num_bytes,
    uint32_t meta_max_num_bytes, uint32_t primary_item_id,
    uint32_t* num_parsed_boxes, AvifInfoFeatures* features) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "dimg", 4)) {
      // See ISO/IEC 14496-12:2015(E) 8.11.12.2
      const uint32_t num_bytes_per_id = (box.version == 0) ? 2 : 4;
      uint32_t offset = 0;
      AVIFINFO_CHECK_FOUND(
          AccessContent(&box, num_bytes, num_bytes_per_id + 2));
      const uint32_t from_item_id =
          AvifInfoInternalReadBigEndian(box.content + offset, num_bytes_per_id);
      offset += num_bytes_per_id;
      if (from_item_id == primary_item_id) {
        const uint32_t reference_count =
            AvifInfoInternalReadBigEndian(box.content + offset, 2);
        offset += 2;
        for (uint32_t i = 0; i < reference_count; ++i) {
          AVIFINFO_CHECK_FOUND(
              AccessContent(&box, num_bytes, offset + num_bytes_per_id));
          const uint32_t to_item_id = AvifInfoInternalReadBigEndian(
              box.content + offset, num_bytes_per_id);
          offset += num_bytes_per_id;
          AVIFINFO_CHECK(meta_bytes != NULL && meta_bytes < bytes, kInvalid);
          AVIFINFO_CHECK(meta_max_num_bytes > 0, kInvalid);
          // Go up one level: from "dimg" among "iref" to boxes among "meta".
          AVIFINFO_CHECK_NOT_FOUND(ParseMetaForFeatures(
              meta_bytes, meta_num_bytes, meta_max_num_bytes, to_item_id,
              num_parsed_boxes, features));
          // Trying the first tile should be enough. Check others just in case.
        }
      }
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kNotFound);  // No "dimg" in "iref" is not an issue.
}

static AvifInfoInternalStatus ParseMetaForFeaturesInTiles(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t max_num_bytes,
    uint32_t primary_item_id, uint32_t* num_parsed_boxes,
    AvifInfoFeatures* features) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "iref", 4)) {
      return ParseIrefForFeaturesInTiles(
          box.content, num_bytes - box.content_position, box.content_size,
          bytes, num_bytes, max_num_bytes, primary_item_id, num_parsed_boxes,
          features);
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kNotFound);  // No "iref" in "meta" is not an issue.
}

static AvifInfoInternalStatus ParseFileForFeaturesInTiles(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t file_size,
    uint32_t primary_item_id, uint32_t* num_parsed_boxes,
    AvifInfoFeatures* features) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, file_size, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "meta", 4)) {
      return ParseMetaForFeaturesInTiles(
          box.content, num_bytes - box.content_position, box.content_size,
          primary_item_id, num_parsed_boxes, features);
    }
    position += box.size;
  } while (position != file_size);
  AVIFINFO_RETURN(kInvalid);  // No "meta" is an issue.
}

//------------------------------------------------------------------------------
// Search if there is an alpha layer through "meta > iprp > ipco > auxC" boxes.

static AvifInfoInternalStatus ParseIpcoForAlpha(const uint8_t* bytes,
                                                uint32_t num_bytes,
                                                uint32_t max_num_bytes,
                                                uint32_t* num_parsed_boxes) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "auxC", 4)) {
      // See AV1 Image File Format (AVIF) 4
      // at https://aomediacodec.github.io/av1-avif/#auxiliary-images
      const char* kAlphaStr = "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
      const uint32_t kAlphaStrLength = 44;  // Includes terminating character.
      if (box.content_size >= kAlphaStrLength) {
        AVIFINFO_CHECK(box.content_position + kAlphaStrLength <= num_bytes,
                       kTruncated);
        const char* const aux_type = (const char*)box.content;
        if (strcmp(aux_type, kAlphaStr) == 0) {
          // Note: It is unlikely but it is possible that this alpha plane does
          //       not belong to the primary item or a tile. Ignore this issue.
          return kFound;
        }
      }
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kNotFound);  // No "auxC" in "ipco" is not an issue.
}

static AvifInfoInternalStatus ParseIprpForAlpha(const uint8_t* bytes,
                                                uint32_t num_bytes,
                                                uint32_t max_num_bytes,
                                                uint32_t* num_parsed_boxes) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "ipco", 4)) {
      return ParseIpcoForAlpha(box.content, num_bytes - box.content_position,
                               box.content_size, num_parsed_boxes);
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kInvalid);  // No "ipco" in "iprp" is an issue.
}

static AvifInfoInternalStatus ParseMetaForAlpha(const uint8_t* bytes,
                                                uint32_t num_bytes,
                                                uint32_t max_num_bytes,
                                                uint32_t* num_parsed_boxes) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, max_num_bytes, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "iprp", 4)) {
      return ParseIprpForAlpha(box.content, num_bytes - box.content_position,
                               box.content_size, num_parsed_boxes);
    }
    position += box.size;
  } while (position != max_num_bytes);
  AVIFINFO_RETURN(kInvalid);  // No "iprp" in "meta" is an issue.
}

static AvifInfoInternalStatus ParseFileForAlpha(const uint8_t* bytes,
                                                uint32_t num_bytes,
                                                uint32_t file_size,
                                                uint32_t* num_parsed_boxes) {
  uint32_t position = 0;
  do {
    AvifInfoInternalBox box;
    AVIFINFO_CHECK_FOUND(AvifInfoInternalParseBox(
        bytes, num_bytes, file_size, position, num_parsed_boxes, &box));

    if (!memcmp(box.type, "meta", 4)) {
      return ParseMetaForAlpha(box.content, num_bytes - box.content_position,
                               box.content_size, num_parsed_boxes);
    }
    position += box.size;
  } while (position != file_size);
  AVIFINFO_RETURN(kInvalid);  // No "meta" is an issue.
}

//------------------------------------------------------------------------------
// Parsing starting point.

static AvifInfoInternalStatus AvifInfoInternalParseFile(
    const uint8_t* bytes, uint32_t num_bytes, uint32_t file_size,
    AvifInfoFeatures* features) {
  uint32_t num_parsed_boxes = 0;
  AVIFINFO_CHECK_FOUND(
      ParseFileForBrand(bytes, num_bytes, file_size, &num_parsed_boxes));

  // 'bytes' is an AVIF file. Next step is finding the ID of the primary item.
  uint32_t primary_item_id;
  AVIFINFO_CHECK_FOUND(ParseFileForPrimaryItemId(
      bytes, num_bytes, file_size, &num_parsed_boxes, &primary_item_id));

  // Now find the 'features' of the primary item.
  AvifInfoInternalStatus status =
      ParseFileForFeatures(bytes, num_bytes, file_size, primary_item_id,
                           &num_parsed_boxes, features);
  if (status == kNotFound) {
    // It is possible that some of the 'features' are missing for the primary
    // item. Try to look into tiles in case they are defined there.
    status = ParseFileForFeaturesInTiles(bytes, num_bytes, file_size,
                                         primary_item_id, &num_parsed_boxes,
                                         features);
  }
  AVIFINFO_CHECK_FOUND(status);

  // If there is an alpha plane, add 1 to the number of channels.
  status = ParseFileForAlpha(bytes, num_bytes, file_size, &num_parsed_boxes);
  if (status == kFound) {
    ++features->num_channels;
  } else {
    AVIFINFO_CHECK_NOT_FOUND(status);
  }
  return kFound;
}

//------------------------------------------------------------------------------
// Public API

AvifInfoStatus AvifInfoGet(const uint8_t* data, size_t data_size,
                           AvifInfoFeatures* features) {
  // Consider the file to be of maximum size.
  return AvifInfoGetWithSize(data, data_size, features,
                             /*file_size=*/kAvifInfoInternalMaxSize);
}

AvifInfoStatus AvifInfoGetWithSize(const uint8_t* data, size_t data_size,
                                   AvifInfoFeatures* features,
                                   size_t file_size) {
  if (features != NULL) memset(features, 0, sizeof(*features));
  if (data == NULL) return kAvifInfoNotEnoughData;
  if (data_size > file_size) data_size = file_size;

  AvifInfoFeatures parsed_features;
  memset(&parsed_features, 0, sizeof(parsed_features));
  const AvifInfoInternalStatus status = AvifInfoInternalParseFile(
      data,
      (data_size >= kAvifInfoInternalMaxSize) ? kAvifInfoInternalMaxSize
                                              : (uint32_t)data_size,
      (file_size >= kAvifInfoInternalMaxSize) ? kAvifInfoInternalMaxSize
                                              : (uint32_t)file_size,
      &parsed_features);

  if (status == kNotFound) {
    return (data_size < file_size) ? kAvifInfoNotEnoughData
                                   : kAvifInfoInvalidFile;
  }
  if (status == kTruncated) return kAvifInfoNotEnoughData;
  if (status == kInvalid) return kAvifInfoInvalidFile;
  if (status == kAborted) return kAvifInfoTooComplex;
  if (features != NULL) {
    memcpy(features, &parsed_features, sizeof(*features));
  }
  return kAvifInfoOk;
}

//------------------------------------------------------------------------------

#undef AVIFINFO_RETURN
#undef AVIFINFO_CHECK
#undef AVIFINFO_CHECK_STATUS_IS
#undef AVIFINFO_CHECK_FOUND
#undef AVIFINFO_CHECK_NOT_FOUND
