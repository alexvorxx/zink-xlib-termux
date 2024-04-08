// Copyright © 2024 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::format::Format;
use crate::image::SampleLayout;
use crate::tiling::{gob_height, Tiling, GOB_DEPTH, GOB_WIDTH_B};
use crate::Minify;

#[derive(Clone, Debug, Copy, PartialEq, Default)]
#[repr(C)]
pub struct Extent4D {
    pub width: u32,
    pub height: u32,
    pub depth: u32,
    pub array_len: u32,
}

#[no_mangle]
pub extern "C" fn nil_extent4d(
    width: u32,
    height: u32,
    depth: u32,
    array_len: u32,
) -> Extent4D {
    Extent4D {
        width,
        height,
        depth,
        array_len,
    }
}

impl Extent4D {
    pub fn new(
        width: u32,
        height: u32,
        depth: u32,
        array_len: u32,
    ) -> Extent4D {
        Extent4D {
            width,
            height,
            depth,
            array_len,
        }
    }

    pub fn size(&self) -> u32 {
        self.width * self.height * self.depth
    }

    pub fn align(self, alignment: &Self) -> Self {
        Self {
            width: self.width.next_multiple_of(alignment.width),
            height: self.height.next_multiple_of(alignment.height),
            depth: self.depth.next_multiple_of(alignment.depth),
            array_len: self.array_len.next_multiple_of(alignment.array_len),
        }
    }

    pub fn mul(self, other: Self) -> Self {
        Self {
            width: self.width * other.width,
            height: self.height * other.height,
            depth: self.depth * other.depth,
            array_len: self.array_len * other.array_len,
        }
    }

    pub fn div_ceil(self, other: Self) -> Self {
        Self {
            width: self.width.div_ceil(other.width),
            height: self.height.div_ceil(other.height),
            depth: self.depth.div_ceil(other.depth),
            array_len: self.array_len.div_ceil(other.array_len),
        }
    }

    pub fn px_to_sa(self, sample_layout: SampleLayout) -> Self {
        self.mul(sample_layout.px_extent_sa())
    }

    #[no_mangle]
    pub extern "C" fn nil_extent4d_px_to_el(
        self,
        format: Format,
        sample_layout: SampleLayout,
    ) -> Extent4D {
        self.px_to_el(format, sample_layout)
    }

    pub fn px_to_el(self, format: Format, sample_layout: SampleLayout) -> Self {
        self.px_to_sa(sample_layout).div_ceil(format.el_extent_sa())
    }

    pub fn el_to_B(self, bytes_per_element: u32) -> Self {
        Self {
            width: self.width * bytes_per_element,
            ..self
        }
    }
    pub fn px_to_B(self, format: Format, sample_layout: SampleLayout) -> Self {
        self.px_to_el(format, sample_layout)
            .el_to_B(format.el_size_B())
    }

    pub fn B_to_GOB(self, gob_height_is_8: bool) -> Self {
        let gob_extent_B = Self {
            width: GOB_WIDTH_B,
            height: gob_height(gob_height_is_8),
            depth: GOB_DEPTH,
            array_len: 1,
        };

        self.div_ceil(gob_extent_B)
    }

    #[no_mangle]
    pub extern "C" fn nil_extent4d_px_to_tl(
        self,
        tiling: &Tiling,
        format: Format,
        sample_layout: SampleLayout,
    ) -> Self {
        self.px_to_tl(tiling, format, sample_layout)
    }

    pub fn px_to_tl(
        self,
        tiling: &Tiling,
        format: Format,
        sample_layout: SampleLayout,
    ) -> Self {
        let tl_extent_B = tiling.extent_B();
        self.px_to_B(format, sample_layout).div_ceil(tl_extent_B)
    }
}

#[derive(Clone, Debug, Copy, PartialEq)]
#[repr(C)]
pub struct Offset4D {
    pub x: u32,
    pub y: u32,
    pub z: u32,
    pub a: u32,
}

#[no_mangle]
pub extern "C" fn nil_offset4d(x: u32, y: u32, z: u32, a: u32) -> Offset4D {
    Offset4D { x, y, z, a }
}

impl Offset4D {
    fn div_floor(self, other: Extent4D) -> Self {
        Self {
            x: self.x / other.width,
            y: self.y / other.height,
            z: self.z / other.depth,
            a: self.a / other.array_len,
        }
    }

    fn mul(self, other: Extent4D) -> Self {
        Self {
            x: self.x * other.width,
            y: self.y * other.height,
            z: self.z * other.depth,
            a: self.a * other.array_len,
        }
    }

    #[no_mangle]
    pub extern "C" fn nil_offset4d_px_to_el(
        self,
        format: Format,
        sample_layout: SampleLayout,
    ) -> Self {
        self.px_to_el(format, sample_layout)
    }

    pub fn px_to_el(self, format: Format, sample_layout: SampleLayout) -> Self {
        self.mul(sample_layout.px_extent_sa())
            .div_floor(format.el_extent_sa())
    }

    #[no_mangle]
    pub extern "C" fn nil_offset4d_px_to_tl(
        self,
        tiling: &Tiling,
        format: Format,
        sample_layout: SampleLayout,
    ) -> Self {
        self.px_to_tl(tiling, format, sample_layout)
    }

    pub fn px_to_tl(
        self,
        tiling: &Tiling,
        format: Format,
        sample_layout: SampleLayout,
    ) -> Self {
        self.px_to_B(format, sample_layout)
            .div_floor(tiling.extent_B())
    }

    #[no_mangle]
    pub extern "C" fn nil_offset4d_px_to_B(
        self,
        format: Format,
        sample_layout: SampleLayout,
    ) -> Self {
        self.px_to_B(format, sample_layout)
    }

    pub fn px_to_B(self, format: Format, sample_layout: SampleLayout) -> Self {
        self.px_to_el(format, sample_layout)
            .el_to_B(format.el_size_B())
    }

    #[no_mangle]
    pub extern "C" fn nil_offset4d_el_to_B(
        self,
        bytes_per_element: u32,
    ) -> Self {
        self.el_to_B(bytes_per_element)
    }

    pub fn el_to_B(self, bytes_per_element: u32) -> Self {
        let mut offset_B = self;
        offset_B.x *= bytes_per_element;
        offset_B
    }
}

impl Minify<u32> for Extent4D {
    fn minify(self, level: u32) -> Self {
        Self {
            width: self.width.minify(level),
            height: self.height.minify(level),
            depth: self.depth.minify(level),
            ..self
        }
    }
}
