// Copyright (c) 2024, Alliance for Open Media. All rights reserved
//
// This source code is subject to the terms of the BSD 2 Clause License and
// the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
// was not distributed with this source code in the LICENSE file, you can
// obtain it at www.aomedia.org/license/software. If the Alliance for Open
// Media Patent License 1.0 was not distributed with this source code in the
// PATENTS file, you can obtain it at www.aomedia.org/license/patent.

use avifinfo::{get_features, identify, AvifInfoError, Features};
use std::{fs::File, io::Read};

#[cfg(test)]
fn load_file(path: &str) -> Vec<u8> {
    let mut bytes = Vec::new();
    File::open(path).unwrap().read_to_end(&mut bytes).unwrap();
    bytes
}

//------------------------------------------------------------------------------
// Positive tests

#[test]
fn single_pixel() {
    let file = load_file("tests/avifinfo_test_1x1.avif");
    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 1,
            height: 1,
            bit_depth: 8,
            num_channels: 3,
            has_gainmap: false,
            gainmap_item_id: 0,
            primary_item_id_location: 96,
            primary_item_id_bytes: 2,
        })
    );
}

#[test]
fn with_alpha() {
    let file = load_file("tests/avifinfo_test_2x2_alpha.avif");
    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 2,
            height: 2,
            bit_depth: 8,
            num_channels: 4,
            has_gainmap: false,
            gainmap_item_id: 0,
            primary_item_id_location: 96,
            primary_item_id_bytes: 2,
        })
    );
}

#[test]
fn with_gainmap() {
    let file = load_file("tests/avifinfo_test_20x20_gainmap.avif");
    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 20,
            height: 20,
            bit_depth: 8,
            num_channels: 3,
            has_gainmap: true,
            gainmap_item_id: 2,
            primary_item_id_location: 96,
            primary_item_id_bytes: 2,
        })
    );
}

#[test]
fn set_primary_item_id_to_be_gainmap_item_id() {
    let mut file = load_file("tests/avifinfo_test_20x20_gainmap.avif");
    file[97] = 2;
    // TODO(maryla-uc): find a small test file with a gainmap that is smaller
    //                  than the main image.
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 20,
            height: 20,
            bit_depth: 8,
            num_channels: 1, // the gainmap is monochrome
            has_gainmap: true,
            gainmap_item_id: 2,
            primary_item_id_location: 96,
            primary_item_id_bytes: 2,
        })
    );
}

#[test]
fn with_gainmap_tmap() {
    for file_path in [
        "tests/avifinfo_test_12x34_gainmap_tmap.avif",
        "tests/avifinfo_test_12x34_gainmap_tmap_iref_after_iprp.avif",
    ] {
        let file = load_file(file_path);
        assert_eq!(identify(file.as_slice()), Ok(()));
        assert_eq!(
            get_features(file.as_slice()),
            Ok(Features {
                width: 12,
                height: 34,
                bit_depth: 10,
                num_channels: 4,
                has_gainmap: true,
                gainmap_item_id: 4,
                primary_item_id_location: 96,
                primary_item_id_bytes: 2,
            })
        );
    }
}

#[test]
fn no_pixi_10b() {
    // Same as above but "meta" box size is stored as 64 bits, "av1C" has
    // 'high_bitdepth' set to true, "pixi" was renamed to "pixy" and "mdat" size
    // is 0 (extends to the end of the file).
    let file = load_file("tests/avifinfo_test_1x1_10b_nopixi_metasize64b_mdatsize0.avif");
    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 1,
            height: 1,
            bit_depth: 10,
            num_channels: 3,
            has_gainmap: false,
            gainmap_item_id: 0,
            primary_item_id_location: 104,
            primary_item_id_bytes: 2,
        })
    );
}

#[test]
fn enough_bytes() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    // Truncate 'input' just after the required information (discard AV1 box).
    let mdat_position = file.windows(4).position(|window| window == b"mdat");
    file.resize(mdat_position.unwrap(), 0);

    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 1,
            height: 1,
            bit_depth: 8,
            num_channels: 3,
            has_gainmap: false,
            gainmap_item_id: 0,
            primary_item_id_location: 96,
            primary_item_id_bytes: 2,
        })
    );
}

#[test]
fn metabox_is_big() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    let meta_position = file.windows(4).position(|window| window == b"meta");
    // 32-bit "1" then 4-char "meta" then 64-bit size.
    file[meta_position.unwrap() - 4] = 0;
    file[meta_position.unwrap() - 3] = 0;
    file[meta_position.unwrap() - 2] = 0;
    file[meta_position.unwrap() - 1] = 1;
    for _ in 0..8 {
        file.insert(meta_position.unwrap() + 4, 1);
    }

    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 1,
            height: 1,
            bit_depth: 8,
            num_channels: 3,
            has_gainmap: false,
            gainmap_item_id: 0,
            primary_item_id_location: 104,
            primary_item_id_bytes: 2,
        })
    );
}

#[test]
fn metabox_runs_till_end_of_file() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    let meta_position = file.windows(4).position(|window| window == b"meta");
    // 32-bit "0" then 4-char "meta".
    file[meta_position.unwrap() - 4] = 0;
    file[meta_position.unwrap() - 3] = 0;
    file[meta_position.unwrap() - 2] = 0;
    file[meta_position.unwrap() - 1] = 0;

    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(
        get_features(file.as_slice()),
        Ok(Features {
            width: 1,
            height: 1,
            bit_depth: 8,
            num_channels: 3,
            has_gainmap: false,
            gainmap_item_id: 0,
            primary_item_id_location: 96,
            primary_item_id_bytes: 2,
        })
    );
}

//------------------------------------------------------------------------------
// Negative tests

#[test]
fn empty() {
    assert_eq!(identify(&[]), Err(AvifInfoError::NotEnoughData));
    assert_eq!(get_features(&[]), Err(AvifInfoError::NotEnoughData));
}

#[test]
fn not_enough_bytes() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    // Truncate 'input' before having all the required information.
    let ipma_position = file.windows(4).position(|window| window == b"ipma");
    file.resize(ipma_position.unwrap(), 0);

    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(get_features(file.as_slice()), Err(AvifInfoError::NotEnoughData));
}

#[test]
fn broken() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    // Change "ispe" to "aspe".
    let ispe_position = file.windows(4).position(|window| window == b"ispe");
    file[ispe_position.unwrap()] = b'a';

    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(get_features(file.as_slice()), Err(AvifInfoError::InvalidFile));
}

#[test]
fn metabox_is_too_big() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    let meta_position = file.windows(4).position(|window| window == b"meta");
    // 32-bit "1" then 4-char "meta" then 64-bit size.
    file[meta_position.unwrap() - 4] = 0;
    file[meta_position.unwrap() - 3] = 0;
    file[meta_position.unwrap() - 2] = 0;
    file[meta_position.unwrap() - 1] = 1;
    for _ in 0..8 {
        file.insert(meta_position.unwrap() + 4, 255);
    }

    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(get_features(file.as_slice()), Err(AvifInfoError::TooComplex));
}

#[test]
fn filetypebox_runs_till_end_of_file() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    let ftyp_position = file.windows(4).position(|window| window == b"ftyp");
    file[ftyp_position.unwrap() - 1] = 0;

    assert_eq!(identify(file.as_slice()), Err(AvifInfoError::InvalidFile));
    assert_eq!(get_features(file.as_slice()), Err(AvifInfoError::InvalidFile));
}

#[test]
fn imagespatialextentsproperty_runs_till_end_of_file() {
    let mut file = load_file("tests/avifinfo_test_1x1.avif");
    let ispe_position = file.windows(4).position(|window| window == b"ispe");
    file[ispe_position.unwrap() - 1] = 0;

    assert_eq!(identify(file.as_slice()), Ok(()));
    assert_eq!(get_features(file.as_slice()), Err(AvifInfoError::InvalidFile));
}

#[test]
fn too_many_boxes() {
    // Create a valid-ish input with too many boxes to parse.
    let mut input: Vec<u8> =
        vec![0, 0, 0, 16, b'f', b't', b'y', b'p', b'a', b'v', b'i', b'f', 0, 0, 0, 0];
    let num_boxes = 12345;
    input.reserve(input.len() + num_boxes * 8);
    for _ in 0..num_boxes {
        input.extend_from_slice(&[0, 0, 0, 8, b'a', b'b', b'c', b'd']);
    }

    assert_eq!(identify(input.as_slice()), Ok(()));
    assert_eq!(get_features(input.as_slice()), Err(AvifInfoError::TooComplex));
}
