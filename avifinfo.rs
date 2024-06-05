// Copyright (c) 2024, Alliance for Open Media. All rights reserved
//
// This source code is subject to the terms of the BSD 2 Clause License and
// the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
// was not distributed with this source code in the LICENSE file, you can
// obtain it at www.aomedia.org/license/software. If the Alliance for Open
// Media Patent License 1.0 was not distributed with this source code in the
// PATENTS file, you can obtain it at www.aomedia.org/license/patent.

//-----------------------------------------------------------------------------

#[derive(PartialEq, Debug)]
pub enum AvifInfoError {
    // The input bitstream was correctly parsed until now but bytes are missing. The request
    // should be repeated with more input bytes.
    NotEnoughData,
    // The input bitstream was correctly parsed until now but it is too complex. The parsing was
    // stopped to avoid any timeout or crash.
    TooComplex,
    // The input bitstream is not a valid AVIF file, truncated or not.
    InvalidFile,
}

// Ok means that the file was correctly parsed and the requested information was
// extracted. It is not guaranteed that the input bitstream is a valid complete
// AVIF file.
type AvifInfoResult<T> = Result<T, AvifInfoError>;

#[derive(Debug, Default, PartialEq)]
pub struct Features {
    // In number of pixels. Ignores crop and rotation.
    pub width: u32,
    pub height: u32,

    // Likely 8, 10 or 12 bits per channel per pixel.
    pub bit_depth: u8,

    // Likely 1, 2, 3 or 4 channels: (1 monochrome or 3 colors) + (0 or 1 alpha)
    pub num_channels: u8,

    // True if a gain map was found.
    pub has_gainmap: bool,

    // Id of the gain map item. Assumes there is at most one. If there are several gain map items
    // (e.g. because the main image is tiled and each tile has an independent gain map), then this
    // is one of the ids, arbitrarily chosen.
    pub gainmap_item_id: u8,

    // Start location in bytes of the primary item id, relative to the beginning of the given
    // payload. The primary item id is a big endian number stored on bytes primary_item_id_location
    // to primary_item_id_location+primary_item_id_bytes-1 inclusive.
    pub primary_item_id_location: usize,

    // Number of bytes of the primary item id.
    pub primary_item_id_bytes: u8,
}

//------------------------------------------------------------------------------

// Status returned when reading the content of a box (or file).
#[derive(PartialEq)]
enum InternalError {
    NotFound,  // Input correctly parsed but information is missing or elsewhere.
    Truncated, // Input correctly parsed until missing bytes to continue.
    Aborted,   // Input correctly parsed until stopped to avoid timeout or crash.
    Invalid,   // Input incorrectly parsed.
}

// Ok means "Input correctly parsed and information retrieved".
type InternalResult<T> = Result<T, InternalError>;

impl From<InternalError> for AvifInfoError {
    fn from(error: InternalError) -> Self {
        match error {
            InternalError::NotFound | InternalError::Truncated => AvifInfoError::NotEnoughData,
            InternalError::Aborted => AvifInfoError::TooComplex,
            InternalError::Invalid => AvifInfoError::InvalidFile,
        }
    }
}

// Be reasonable. Avoid timeouts and out-of-memory.
const AVIFINFO_MAX_NUM_BOXES: u32 = 4096;
// InternalFeatures uses u8 to store values.
const AVIFINFO_MAX_VALUE: u8 = u8::MAX;
// Maximum number of stored associations. Past that, they are skipped.
const AVIFINFO_MAX_TILES: usize = 16;
const AVIFINFO_MAX_PROPS: usize = 32;
const AVIFINFO_MAX_FEATURES: usize = 8;
const AVIFINFO_UNDEFINED: u8 = 0;

//------------------------------------------------------------------------------
// Streamed input struct and helper functions.

struct Stream<'a> {
    // The available bytes.
    data: Option<&'a [u8]>,
    // The known size of the parent box, if any.
    size: Option<usize>,
    // How many bytes were read or skipped.
    offset: usize,
}

impl Stream<'_> {
    fn read(&mut self, num_bytes: usize) -> InternalResult<&[u8]> {
        if num_bytes == 0 {
            return Ok(&[]);
        }
        let offset = self.offset;
        self.skip(num_bytes)?;
        match &self.data {
            Some(data) if self.offset <= data.len() => Ok(&data[offset..self.offset]),
            _ => Err(InternalError::Truncated),
        }
    }

    fn read_u8(&mut self) -> InternalResult<u8> {
        Ok(self.read(1)?[0])
    }

    fn read_u16(&mut self) -> InternalResult<u16> {
        Ok(u16::from_be_bytes(self.read(2)?.try_into().unwrap()))
    }

    fn read_u24(&mut self) -> InternalResult<u32> {
        Ok((self.read_uint(1)? << 16) | self.read_uint(2)?)
    }

    fn read_u32(&mut self) -> InternalResult<u32> {
        Ok(u32::from_be_bytes(self.read(4)?.try_into().unwrap()))
    }

    fn read_u64(&mut self) -> InternalResult<u64> {
        Ok(u64::from_be_bytes(self.read(8)?.try_into().unwrap()))
    }

    fn read_uint(&mut self, num_bytes: u8) -> InternalResult<u32> {
        match num_bytes {
            1 => Ok(self.read_u8()? as u32),
            2 => Ok(self.read_u16()? as u32),
            4 => Ok(self.read_u32()?),
            _ => Err(InternalError::Aborted),
        }
    }

    fn read_4cc(&mut self) -> InternalResult<&[u8; 4]> {
        Ok(self.read(4)?.try_into().unwrap())
    }

    fn skip(&mut self, num_bytes: usize) -> InternalResult<()> {
        self.offset = self.offset.checked_add(num_bytes).ok_or(InternalError::Aborted)?;
        if let Some(size) = self.size {
            if self.offset > size {
                return Err(InternalError::Invalid);
            }
        }
        Ok(())
    }

    fn num_read_bytes(&self) -> usize {
        self.offset // Includes the unchecked skipped bytes.
    }

    fn has_more_bytes(&self) -> bool {
        match self.size {
            Some(size) => self.offset < size,
            None => true,
        }
    }

    // Returns a portion of the stream. The size of the portion is either given
    // or all remaining bytes are returned.
    fn substream(&mut self, num_bytes: Option<usize>) -> InternalResult<Stream> {
        let offset = self.offset;
        let num_transferred_bytes = num_bytes.unwrap_or(match &self.data {
            Some(data) => data.len().saturating_sub(offset),
            None => 0,
        });
        self.skip(num_transferred_bytes)?;
        self.offset = offset.checked_add(num_transferred_bytes).ok_or(InternalError::Aborted)?;
        Ok(Stream {
            data: if let Some(data) = &self.data {
                let available_size = data.len().saturating_sub(offset);
                let size = std::cmp::min(available_size, num_transferred_bytes);
                if size != 0 { Some(&data[offset..offset + size]) } else { None }
            } else {
                None
            },
            size: num_bytes,
            offset: 0,
        })
    }
}

//------------------------------------------------------------------------------
// Features are parsed into temporary property associations.

#[derive(Default)]
struct InternalTile {
    tile_item_id: u8,
    parent_item_id: u8,
    dimg_idx: u8, // Index of this association in the dimg box (0-based).
}

#[derive(Default)]
struct InternalProp {
    property_index: u8,
    item_id: u8,
}

#[derive(Default)]
struct InternalDimProp {
    property_index: u8,
    width: u32,
    height: u32,
}

#[derive(Default)]
struct InternalChanProp {
    property_index: u8,
    bit_depth: u8,
    num_channels: u8,
}

#[derive(Default)]
struct InternalFeatures {
    has_primary_item: bool,     // True if "pitm" was parsed.
    has_alpha: bool,            // True if an alpha "auxC" was parsed.
    gainmap_property_index: u8, // Index of the gain map auxC property.
    primary_item_id: u8,
    primary_item_features: Features, // Deduced from the data below.
    data_was_skipped: bool,          // True if some loops/indices were skipped.
    tone_mapped_item_id: u8,         // Id of the "tmap" box, > 0 if present.
    iinf_parsed: bool,               // True if the "iinf" (item info) box was parsed.
    iref_parsed: bool,               // True if the "iref" (item reference) box was parsed.

    num_tiles: usize,
    tiles: [InternalTile; AVIFINFO_MAX_TILES],
    num_props: usize,
    props: [InternalProp; AVIFINFO_MAX_PROPS],
    num_dim_props: usize,
    dim_props: [InternalDimProp; AVIFINFO_MAX_FEATURES],
    num_chan_props: usize,
    chan_props: [InternalChanProp; AVIFINFO_MAX_FEATURES],
}

impl InternalFeatures {
    fn get_item_features(&mut self, target_item_id: u8, tile_depth: u32) -> InternalResult<()> {
        for prop_item in 0..self.num_props {
            if self.props[prop_item].item_id != target_item_id {
                continue;
            }
            let property_index = self.props[prop_item].property_index;

            // Retrieve the width and height of the primary item if not already done.
            if target_item_id == self.primary_item_id
                && (self.primary_item_features.width == AVIFINFO_UNDEFINED as u32
                    || self.primary_item_features.height == AVIFINFO_UNDEFINED as u32)
            {
                for i in 0..self.num_dim_props {
                    if self.dim_props[i].property_index != property_index {
                        continue;
                    }
                    self.primary_item_features.width = self.dim_props[i].width;
                    self.primary_item_features.height = self.dim_props[i].height;
                    if self.primary_item_features.bit_depth != AVIFINFO_UNDEFINED
                        && self.primary_item_features.num_channels != AVIFINFO_UNDEFINED
                    {
                        return Ok(());
                    }
                    break;
                }
            }
            // Retrieve the bit depth and number of channels of the target item if not
            // already done.
            if self.primary_item_features.bit_depth == AVIFINFO_UNDEFINED
                || self.primary_item_features.num_channels == AVIFINFO_UNDEFINED
            {
                for i in 0..self.num_chan_props {
                    if self.chan_props[i].property_index != property_index {
                        continue;
                    }
                    self.primary_item_features.bit_depth = self.chan_props[i].bit_depth;
                    self.primary_item_features.num_channels = self.chan_props[i].num_channels;
                    if self.primary_item_features.width != AVIFINFO_UNDEFINED as u32
                        && self.primary_item_features.height != AVIFINFO_UNDEFINED as u32
                    {
                        return Ok(());
                    }
                    break;
                }
            }
        }

        // Check for the bit_depth and num_channels in a tile if not yet found.
        if tile_depth < 3 {
            for tile in 0..self.num_tiles {
                if self.tiles[tile].parent_item_id != target_item_id {
                    continue;
                }
                match self.get_item_features(self.tiles[tile].tile_item_id, tile_depth + 1) {
                    Ok(()) => return Ok(()),
                    Err(InternalError::NotFound) => {} // Keep searching.
                    Err(error) => return Err(error),
                }
            }
        }
        Err(InternalError::NotFound)
    }

    // Generates the primary_item_features.
    // Returns InternalError::NotFound if there is not enough information.
    fn get_primary_item_features(&mut self) -> InternalResult<()> {
        // Nothing to do without the primary item ID.
        if !self.has_primary_item {
            return Err(InternalError::NotFound);
        }
        // Early exit.
        if self.num_dim_props == 0 || self.num_chan_props == 0 {
            return Err(InternalError::NotFound);
        }

        // Look for a gain map.
        // HEIF scheme: gain map is a hidden input of a derived item.
        if self.tone_mapped_item_id != AVIFINFO_UNDEFINED {
            for tile in 0..self.num_tiles {
                if self.tiles[tile].parent_item_id == self.tone_mapped_item_id
                    && self.tiles[tile].dimg_idx == 1
                {
                    self.primary_item_features.has_gainmap = true;
                    self.primary_item_features.gainmap_item_id = self.tiles[tile].tile_item_id;
                    break;
                }
            }
        }
        // Adobe scheme: gain map is an auxiliary item.
        if !self.primary_item_features.has_gainmap && self.gainmap_property_index > 0 {
            for prop_item in 0..self.num_props {
                if self.props[prop_item].property_index == self.gainmap_property_index {
                    self.primary_item_features.has_gainmap = true;
                    self.primary_item_features.gainmap_item_id = self.props[prop_item].item_id;
                    break;
                }
            }
        }
        // If the gain map has not been found but we haven't read all the relevant
        // metadata, we might still find one later and cannot stop now.
        if !self.primary_item_features.has_gainmap
            && (!self.iinf_parsed
                || (self.tone_mapped_item_id != AVIFINFO_UNDEFINED && !self.iref_parsed))
        {
            return Err(InternalError::NotFound);
        }

        self.get_item_features(self.primary_item_id, /* tile_depth= */ 0)?;

        // "auxC" is parsed before the "ipma" properties so it is known now, if any.
        if self.has_alpha {
            self.primary_item_features.num_channels += 1;
        }
        Ok(())
    }
}

//------------------------------------------------------------------------------
// Box header parsing and various size checks.

struct InternalBox {
    box_type: [u8; 4],           // Four characters.
    version: u8,                 // 0 or actual version if this is a full box.
    flags: u32,                  // 0 or actual value if this is a full box.
    content_size: Option<usize>, // If known, size of the box in bytes, header exclusive.
}

// Reads the header of a box starting at the beginning of a stream.
fn parse_box(
    nesting_level: u32,
    stream: &mut Stream,
    num_parsed_boxes: &mut u32,
) -> InternalResult<InternalBox> {
    // See ISO/IEC 14496-12:2012(E) 4.2
    let mut box_header_size = 8usize; // box 32-bit size + 32-bit type (at least)
    let mut box_size: Option<usize> =
        Some(stream.read_u32()?.try_into().or(Err(InternalError::Aborted))?);
    let mut box_type = *stream.read_4cc()?;
    // box_size==1 means 64-bit size should be read after the box type.
    // box_size==0 means this box extends to all remaining bytes.
    if box_size == Some(1) {
        box_header_size += 8;
        box_size = Some(stream.read_u64()?.try_into().or(Err(InternalError::Aborted))?);
    } else if box_size == Some(0) {
        if nesting_level != 0 {
            // ISO/IEC 14496-12 4.2.2:
            //   if size is 0, then this box shall be in a top-level box
            //   (i.e. not contained in another box)
            return Err(InternalError::Invalid);
        }
        box_size = None;
    }
    // 16 bytes of usertype should be read here if the box type is 'uuid'.
    // 'uuid' boxes are skipped so usertype is part of the skipped body.

    let has_fullbox_header = matches!(
        &box_type,
        b"meta" | b"pitm" | b"ipma" | b"ispe" | b"pixi" | b"iref" | b"auxC" | b"iinf" | b"infe"
    );
    if has_fullbox_header {
        box_header_size += 4;
    }
    let content_size = match box_size {
        Some(box_size) => {
            Some(box_size.checked_sub(box_header_size).ok_or(InternalError::Invalid)?)
        }
        None => None,
    };
    if let Some(size) = stream.size {
        if content_size.unwrap_or(box_header_size) > size.saturating_sub(stream.offset) {
            return Err(InternalError::Invalid);
        }
    }
    // get_features() can be called on a full stream or on a stream
    // where the 'ftyp' box was already read. Do not count 'ftyp' boxes towards
    // AVIFINFO_MAX_NUM_BOXES, so that this function returns the same status in
    // both situations (because of the AVIFINFO_MAX_NUM_BOXES check that would
    // compare a different box count otherwise). This is fine because top-level
    // 'ftyp' boxes are just skipped anyway.
    if nesting_level != 0 || &box_type != b"ftyp" {
        // Avoid timeouts. The maximum number of parsed boxes is arbitrary.
        *num_parsed_boxes += 1;
        if *num_parsed_boxes >= AVIFINFO_MAX_NUM_BOXES {
            return Err(InternalError::Aborted);
        }
    }

    let mut version = 0;
    let mut flags = 0;
    if has_fullbox_header {
        version = stream.read_u8()?;
        flags = stream.read_u24()?;
        // See AV1 Image File Format (AVIF) 8.1
        // at https://aomediacodec.github.io/av1-avif/#avif-boxes (available when
        // https://github.com/AOMediaCodec/av1-avif/pull/170 is merged).
        let is_parsable = (&box_type == b"meta" && version == 0)
            || (&box_type == b"pitm" && version <= 1)
            || (&box_type == b"ipma" && version <= 1)
            || (&box_type == b"ispe" && version == 0)
            || (&box_type == b"pixi" && version == 0)
            || (&box_type == b"iref" && version <= 1)
            || (&box_type == b"auxC" && version == 0)
            || (&box_type == b"iinf" && version <= 1)
            || (&box_type == b"infe" && version <= 2);
        // Instead of considering this file as invalid, skip unparsable boxes.
        if !is_parsable {
            box_type = *b"skip"; // FreeSpaceBox. To be ignored by readers.
        }
    }

    Ok(InternalBox { box_type, version, flags, content_size })
}

//------------------------------------------------------------------------------

impl InternalFeatures {
    // Parses a stream of an 'ipco' box.
    // 'ispe' is used for width and height, 'pixi' and 'av1C' are used for bit depth
    // and number of channels, and 'auxC' is used for alpha.
    fn parse_ipco(
        &mut self,
        nesting_level: u32,
        stream: &mut Stream,
        num_parsed_boxes: &mut u32,
    ) -> InternalResult<()> {
        let mut box_index = 1u8; // 1-based index. Used for iterating over properties.
        while stream.has_more_bytes() {
            let box_features = parse_box(nesting_level, stream, num_parsed_boxes)?;
            let mut box_stream = stream.substream(box_features.content_size)?;

            match &box_features.box_type {
                b"ispe" => {
                    // See ISO/IEC 23008-12:2017(E) 6.5.3.2
                    let width = box_stream.read_u32()?;
                    let height = box_stream.read_u32()?;
                    if width == 0 || height == 0 {
                        return Err(InternalError::Invalid);
                    }
                    if self.num_dim_props < AVIFINFO_MAX_FEATURES {
                        self.dim_props[self.num_dim_props].property_index = box_index;
                        self.dim_props[self.num_dim_props].width = width;
                        self.dim_props[self.num_dim_props].height = height;
                        self.num_dim_props += 1;
                    } else {
                        self.data_was_skipped = true;
                    }
                }
                b"pixi" => {
                    // See ISO/IEC 23008-12:2017(E) 6.5.6.2
                    let num_channels = box_stream.read_u8()?;
                    if num_channels == 0 || num_channels > 3 {
                        return Err(InternalError::Invalid);
                    }
                    let bit_depth = box_stream.read_u8()?;
                    if bit_depth == 0 {
                        return Err(InternalError::Invalid);
                    }
                    for _ in 1..num_channels {
                        // Bit depth should be the same for all channels.
                        if box_stream.read_u8()? != bit_depth {
                            return Err(InternalError::Invalid);
                        }
                    }
                    if self.num_chan_props < AVIFINFO_MAX_FEATURES {
                        self.chan_props[self.num_chan_props].property_index = box_index;
                        self.chan_props[self.num_chan_props].bit_depth = bit_depth;
                        self.chan_props[self.num_chan_props].num_channels = num_channels;
                        self.num_chan_props += 1;
                    } else {
                        self.data_was_skipped = true;
                    }
                }
                b"av1C" => {
                    // See AV1 Codec ISO Media File Format Binding 2.3.1
                    // at https://aomediacodec.github.io/av1-isobmff/#av1c
                    // Only parse the necessary third byte. Assume that the others are valid.
                    box_stream.skip(2)?;
                    let data = box_stream.read_u8()?;
                    let high_bitdepth = (data & 0x40) != 0;
                    let twelve_bit = (data & 0x20) != 0;
                    let monochrome = (data & 0x10) != 0;
                    if twelve_bit && !high_bitdepth {
                        return Err(InternalError::Invalid);
                    }
                    if self.num_chan_props < AVIFINFO_MAX_FEATURES {
                        self.chan_props[self.num_chan_props].property_index = box_index;
                        self.chan_props[self.num_chan_props].bit_depth =
                            if high_bitdepth { if twelve_bit { 12 } else { 10 } } else { 8 };
                        self.chan_props[self.num_chan_props].num_channels =
                            if monochrome { 1 } else { 3 };
                        self.num_chan_props += 1;
                    } else {
                        self.data_was_skipped = true;
                    }
                }
                b"auxC" => {
                    // See AV1 Image File Format (AVIF) 4
                    // at https://aomediacodec.github.io/av1-avif/#auxiliary-images
                    const ALPHA_STR: &[u8] = b"urn:mpeg:mpegB:cicp:systems:auxiliary:alpha\0";
                    const GAINMAP_STR: &[u8] = b"urn:com:photo:aux:hdrgainmap\0";
                    // Check for a gain map or for an alpha plane.
                    let content_size = box_features.content_size.unwrap();
                    let data = box_stream.read(std::cmp::min(content_size, ALPHA_STR.len()))?;
                    if &data[..std::cmp::min(data.len(), ALPHA_STR.len())] == ALPHA_STR {
                        // Note: It is unlikely but possible that this alpha plane does not belong
                        // to the primary item or a tile. Ignore this issue.
                        self.has_alpha = true;
                    } else if &data[..std::cmp::min(data.len(), GAINMAP_STR.len())] == GAINMAP_STR {
                        // Note: It is unlikely but possible that this gain map does not belong to
                        // the primary item or a tile. Ignore this issue.
                        self.gainmap_property_index = box_index;
                    }
                }
                _ => {}
            }

            if box_index == AVIFINFO_MAX_VALUE {
                self.data_was_skipped = true;
                return Err(InternalError::NotFound);
            }
            box_index += 1;
        }
        Err(InternalError::NotFound)
    }

    // Parses a stream of an 'iprp' box.
    // The 'ipco' box contains the properties which are linked to items by the
    // 'ipma' box.
    fn parse_iprp(
        &mut self,
        nesting_level: u32,
        stream: &mut Stream,
        num_parsed_boxes: &mut u32,
    ) -> InternalResult<()> {
        while stream.has_more_bytes() {
            let box_features = parse_box(nesting_level, stream, num_parsed_boxes)?;
            let mut box_stream = stream.substream(box_features.content_size)?;

            match &box_features.box_type {
                b"ipco" => {
                    match self.parse_ipco(nesting_level + 1, &mut box_stream, num_parsed_boxes) {
                        Ok(()) => return Ok(()),
                        Err(InternalError::NotFound) => {} // Keep searching.
                        Err(error) => return Err(error),
                    }
                }
                b"ipma" => {
                    // See ISO/IEC 23008-12:2017(E) 9.3.2
                    let entry_count = box_stream.read_u32()?;
                    let id_num_bytes = if box_features.version < 1 { 2 } else { 4 };
                    let index_num_bytes = if box_features.flags & 1 != 0 { 2 } else { 1 };
                    let essential_bit_mask =
                        if box_features.flags & 1 != 0 { 0x8000 } else { 0x80 };

                    for entry in 0..entry_count as usize {
                        if entry >= AVIFINFO_MAX_PROPS || self.num_props >= AVIFINFO_MAX_PROPS {
                            self.data_was_skipped = true;
                            break;
                        }
                        let item_id = box_stream.read_uint(id_num_bytes)?;
                        let association_count = box_stream.read_u8()? as usize;
                        let mut property = 0usize;
                        while property < association_count {
                            if property >= AVIFINFO_MAX_PROPS
                                || self.num_props >= AVIFINFO_MAX_PROPS
                            {
                                self.data_was_skipped = true;
                                break;
                            }
                            let value = box_stream.read_uint(index_num_bytes)?;
                            // let essential = (value & essential_bit_mask);  // Unused.
                            let property_index = value & (!essential_bit_mask);
                            if property_index <= AVIFINFO_MAX_VALUE as u32
                                && item_id <= AVIFINFO_MAX_VALUE as u32
                            {
                                self.props[self.num_props].property_index = property_index as u8;
                                self.props[self.num_props].item_id = item_id as u8;
                                self.num_props += 1;
                            } else {
                                self.data_was_skipped = true;
                            }
                            property += 1;
                        }
                        if property < association_count {
                            break; // Do not read garbage.
                        }
                    }

                    // If all features are available now, do not look further.
                    match self.get_primary_item_features() {
                        Ok(()) => return Ok(()),
                        Err(InternalError::NotFound) => {}
                        Err(error) => return Err(error),
                    }
                }
                _ => {}
            }
        }
        Err(InternalError::NotFound)
    }

    // Parses a stream of an 'iref' box.
    // The 'dimg' boxes contain links between tiles and their parent items, which
    // can be used to infer bit depth and number of channels for the primary item
    // when the latter does not have these properties.
    fn parse_iref(
        &mut self,
        nesting_level: u32,
        stream: &mut Stream,
        num_parsed_boxes: &mut u32,
    ) -> InternalResult<()> {
        self.iref_parsed = true;

        while stream.has_more_bytes() {
            let box_features = parse_box(nesting_level, stream, num_parsed_boxes)?;
            let mut box_stream = stream.substream(box_features.content_size)?;

            if let b"dimg" = &box_features.box_type {
                // See ISO/IEC 14496-12:2015(E) 8.11.12.2
                let num_bytes_per_id = if box_features.version == 0 { 2 } else { 4 };
                let from_item_id = box_stream.read_uint(num_bytes_per_id)?;
                let reference_count = box_stream.read_u16()?;
                for i in 0..reference_count {
                    if i as usize >= AVIFINFO_MAX_TILES {
                        self.data_was_skipped = true;
                        break;
                    }
                    let to_item_id = box_stream.read_uint(num_bytes_per_id)?;
                    if from_item_id <= AVIFINFO_MAX_VALUE as u32
                        && to_item_id <= AVIFINFO_MAX_VALUE as u32
                        && self.num_tiles < AVIFINFO_MAX_TILES
                    {
                        self.tiles[self.num_tiles].tile_item_id = to_item_id as u8;
                        self.tiles[self.num_tiles].parent_item_id = from_item_id as u8;
                        self.tiles[self.num_tiles].dimg_idx = i as u8;
                        self.num_tiles += 1;
                    } else {
                        self.data_was_skipped = true;
                    }
                }

                // If all features are available now, do not look further.
                match self.get_primary_item_features() {
                    Ok(()) => return Ok(()),
                    Err(InternalError::NotFound) => {}
                    Err(error) => return Err(error),
                }
            }
        }
        Err(InternalError::NotFound)
    }

    // Parses a stream of an 'iinf' box.
    fn parse_iinf(
        &mut self,
        nesting_level: u32,
        stream: &mut Stream,
        box_version: u8,
        num_parsed_boxes: &mut u32,
    ) -> InternalResult<()> {
        self.iinf_parsed = true;

        let num_bytes_per_entry_count = if box_version == 0 { 2 } else { 4 };
        let entry_count = stream.read_uint(num_bytes_per_entry_count)?;
        for _ in 0..entry_count {
            let box_features = parse_box(nesting_level, stream, num_parsed_boxes)?;
            let mut box_stream = stream.substream(box_features.content_size)?;

            if let b"infe" = &box_features.box_type {
                // See ISO/IEC 14496-12:2015(E) 8.11.6.2
                let num_bytes_per_id = if box_features.version == 2 { 2 } else { 4 };
                let item_id = box_stream.read_uint(num_bytes_per_id)?;

                // Skip item_protection_index.
                box_stream.skip(2)?;

                if box_stream.read_4cc()? == b"tmap" {
                    // Tone Mapped Image: indicates the presence of a gain map.
                    if item_id <= AVIFINFO_MAX_VALUE as u32 {
                        self.tone_mapped_item_id = item_id as u8;
                    } else {
                        self.data_was_skipped = true;
                    }
                }
            }

            if !stream.has_more_bytes() {
                break; // Ignore entry_count bigger than box.
            }
        }
        Err(InternalError::NotFound)
    }

    // Parses a stream of a 'meta' box. It looks for the primary item ID in the
    // 'pitm' box and recurses into other boxes to find the features.
    fn parse_meta(
        &mut self,
        nesting_level: u32,
        stream_offset: usize,
        stream: &mut Stream,
        num_parsed_boxes: &mut u32,
    ) -> InternalResult<()> {
        while stream.has_more_bytes() {
            let box_features = parse_box(nesting_level, stream, num_parsed_boxes)?;
            let box_header_size = stream.num_read_bytes();
            let mut box_stream = stream.substream(box_features.content_size)?;

            match &box_features.box_type {
                b"pitm" => {
                    // See ISO/IEC 14496-12:2015(E) 8.11.4.2
                    let num_bytes_per_id = if box_features.version == 0 { 2 } else { 4 };
                    let primary_item_id_location =
                        stream_offset.checked_add(box_header_size).ok_or(InternalError::Aborted)?;
                    let primary_item_id = box_stream.read_uint(num_bytes_per_id)?;
                    if primary_item_id > AVIFINFO_MAX_VALUE as u32 {
                        return Err(InternalError::Aborted);
                    }
                    self.has_primary_item = true;
                    self.primary_item_id = primary_item_id as u8;
                    self.primary_item_features.primary_item_id_location = primary_item_id_location;
                    self.primary_item_features.primary_item_id_bytes = num_bytes_per_id;
                }
                b"iprp" => {
                    match self.parse_iprp(nesting_level + 1, &mut box_stream, num_parsed_boxes) {
                        Ok(()) => return Ok(()),
                        Err(InternalError::NotFound) => {} // Keep searching.
                        Err(error) => return Err(error),
                    }
                }
                b"iref" => {
                    match self.parse_iref(nesting_level + 1, &mut box_stream, num_parsed_boxes) {
                        Ok(()) => return Ok(()),
                        Err(InternalError::NotFound) => {} // Keep searching.
                        Err(error) => return Err(error),
                    }
                }
                b"iinf" => {
                    match self.parse_iinf(
                        nesting_level + 1,
                        &mut box_stream,
                        box_features.version,
                        num_parsed_boxes,
                    ) {
                        Ok(()) => return Ok(()),
                        Err(InternalError::NotFound) => {} // Keep searching.
                        Err(error) => return Err(error),
                    }
                }
                _ => {}
            }
        }
        // According to ISO/IEC 14496-12:2012(E) 8.11.1.1 there is at most one 'meta'.
        Err(if self.data_was_skipped { InternalError::Aborted } else { InternalError::Invalid })
    }
}

//------------------------------------------------------------------------------

// Parses a file stream. The file type is checked through the 'ftyp' box.
fn parse_ftyp(stream: &mut Stream) -> InternalResult<()> {
    let mut num_parsed_boxes = 0u32;
    let nesting_level = 0;
    let box_features = parse_box(nesting_level, stream, &mut num_parsed_boxes)?;
    if &box_features.box_type != b"ftyp" {
        return Err(InternalError::Invalid);
    }
    // Consider a FileTypeBox running till the end of the file as invalid,
    // because it should be first and a MetaBox should follow.
    let content_size = box_features.content_size.ok_or(InternalError::Invalid)?;
    // Iterate over brands. See ISO/IEC 14496-12:2012(E) 4.3.1
    if content_size < 8 {
        // major_brand,minor_version
        return Err(InternalError::Invalid);
    }
    for i in 0..content_size / 4 {
        let brand = stream.read_4cc()?;
        if i == 1 {
            continue; // Skip minor_version.
        }
        if brand == b"avif" || brand == b"avis" {
            stream.skip(content_size - (i * 4 + 4))?;
            return Ok(());
        }
        if i > 32 {
            return Err(InternalError::Aborted); // Be reasonable.
        }
    }
    Err(InternalError::Invalid)
}

// Parses a file stream. Features are extracted from the 'meta' box.
impl InternalFeatures {
    fn parse_file(&mut self, stream: &mut Stream) -> InternalResult<()> {
        let mut num_parsed_boxes = 0u32;
        loop {
            let box_features =
                parse_box(/* nesting_level= */ 0, stream, &mut num_parsed_boxes)?;
            let offset = stream.num_read_bytes();
            let mut box_stream = stream.substream(box_features.content_size)?;

            if &box_features.box_type == b"meta" {
                return self.parse_meta(
                    /* nesting_level= */ 1,
                    offset,
                    &mut box_stream,
                    &mut num_parsed_boxes,
                );
            } else if box_features.content_size.is_none() {
                // This non-MetaBox runs till the end of the file. 'meta' is missing.
                return Err(InternalError::Invalid);
            }
        }
    }
}

//------------------------------------------------------------------------------
// Fixed-size input public API

pub fn identify(data: &[u8]) -> AvifInfoResult<()> {
    match parse_ftyp(&mut Stream { data: Some(data), size: None, offset: 0 }) {
        Ok(()) => Ok(()),
        Err(error) => Err(error.into()),
    }
}

pub fn get_features(data: &[u8]) -> AvifInfoResult<Features> {
    let mut features = InternalFeatures { ..Default::default() };
    match features.parse_file(&mut Stream { data: Some(data), size: None, offset: 0 }) {
        Ok(()) => Ok(features.primary_item_features),
        Err(error) => Err(error.into()),
    }
}

//------------------------------------------------------------------------------
// Streamed input API

// There is no streamed input API yet.
