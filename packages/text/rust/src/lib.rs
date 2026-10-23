// The font island: Skrifa parses and instances the font, zeno rasterizes the
// outline. Every entry point catches panics so a malformed font cannot unwind
// into the C++ callers.

use std::panic::{catch_unwind, AssertUnwindSafe};

use skrifa::instance::LocationRef;
use skrifa::outline::{DrawSettings, OutlinePen};
use skrifa::prelude::*;
use zeno::{Command, Fill, Mask, Origin, PathBuilder, Transform};

pub struct ResonateTextFont
{
    bytes: Vec<u8>,
}

#[repr(C)]
pub struct ResonateTextBitmap
{
    pub width: u32,
    pub height: u32,
    pub bearing_x: i32,
    pub bearing_y: i32,
    pub advance: f32,
    pub pixels: *mut u8,
}

impl Default for ResonateTextBitmap
{
    fn default() -> Self
    {
        Self { width: 0, height: 0, bearing_x: 0, bearing_y: 0, advance: 0.0, pixels: std::ptr::null_mut() }
    }
}

struct PathPen
{
    commands: Vec<Command>,
}

impl OutlinePen for PathPen
{
    fn move_to(&mut self, x: f32, y: f32)
    {
        self.commands.move_to([x, y]);
    }

    fn line_to(&mut self, x: f32, y: f32)
    {
        self.commands.line_to([x, y]);
    }

    fn quad_to(&mut self, cx0: f32, cy0: f32, x: f32, y: f32)
    {
        self.commands.quad_to([cx0, cy0], [x, y]);
    }

    fn curve_to(&mut self, cx0: f32, cy0: f32, cx1: f32, cy1: f32, x: f32, y: f32)
    {
        self.commands
            .curve_to([cx0, cy0], [cx1, cy1], [x, y]);
    }

    fn close(&mut self)
    {
        self.commands.close();
    }
}

unsafe fn font_ref<'a>(font: *const ResonateTextFont) -> Option<FontRef<'a>>
{
    let font = font.as_ref()?;
    FontRef::new(&font.bytes).ok()
}

#[no_mangle]
pub extern "C" fn resonate_text_font_create(data: *const u8, len: usize) -> *mut ResonateTextFont
{
    if data.is_null() || len == 0
    {
        return std::ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let bytes = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
        if FontRef::new(&bytes).is_err()
        {
            return std::ptr::null_mut();
        }
        Box::into_raw(Box::new(ResonateTextFont { bytes }))
    }));
    result.unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn resonate_text_font_destroy(font: *mut ResonateTextFont)
{
    if font.is_null()
    {
        return;
    }
    unsafe
    {
        drop(Box::from_raw(font));
    }
}

#[no_mangle]
pub extern "C" fn resonate_text_font_metrics(font: *const ResonateTextFont, size: f32, ascent: *mut f32,
                                             descent: *mut f32, leading: *mut f32) -> i32
{
    if ascent.is_null() || descent.is_null() || leading.is_null() || size <= 0.0
    {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let font = unsafe { font_ref(font) }?;
        let location = font.axes().location([("wght", 400.0)]);
        let metrics = font.metrics(Size::new(size), LocationRef::from(&location));
        unsafe
        {
            *ascent = metrics.ascent;
            *descent = metrics.descent;
            *leading = metrics.leading;
        }
        Some(())
    }));
    match result
    {
        Ok(Some(())) => 1,
        _ => 0,
    }
}

#[no_mangle]
pub extern "C" fn resonate_text_font_glyph_index(font: *const ResonateTextFont, codepoint: u32) -> u32
{
    let result = catch_unwind(AssertUnwindSafe(|| {
        let font = unsafe { font_ref(font) }?;
        font.charmap().map(codepoint).map(|glyph| glyph.to_u32())
    }));
    result.ok().flatten().unwrap_or(0)
}

// Squared Euclidean distance transform of a binary mask (Felzenszwalb).
// The seed value is 1; every output pixel gets the squared distance to the
// nearest seed.
fn edt_1d(f: &[f32], d: &mut [f32])
{
    let n = f.len();
    if n == 0
    {
        return;
    }
    let mut v = vec![0usize; n];
    let mut z = vec![0.0f32; n + 1];
    let mut k = 0usize;
    v[0] = 0;
    z[0] = f32::NEG_INFINITY;
    z[1] = f32::INFINITY;
    for q in 1..n
    {
        let mut s = ((f[q] + (q * q) as f32) - (f[v[k]] + (v[k] * v[k]) as f32))
            / (2.0 * q as f32 - 2.0 * v[k] as f32);
        while s <= z[k]
        {
            k -= 1;
            s = ((f[q] + (q * q) as f32) - (f[v[k]] + (v[k] * v[k]) as f32))
                / (2.0 * q as f32 - 2.0 * v[k] as f32);
        }
        k += 1;
        v[k] = q;
        z[k] = s;
        z[k + 1] = f32::INFINITY;
    }
    k = 0;
    for q in 0..n
    {
        while z[k + 1] < q as f32
        {
            k += 1;
        }
        let delta = q as f32 - v[k] as f32;
        d[q] = delta * delta + f[v[k]];
    }
}

fn distance_to_seed(mask: &[u8], width: usize, height: usize) -> Vec<f32>
{
    let mut f = vec![0.0f32; width * height];
    for (index, value) in mask.iter().enumerate()
    {
        f[index] = if *value != 0 { 0.0 } else { 1.0e20 };
    }
    let mut scratch = vec![0.0f32; width.max(height)];
    let mut column = vec![0.0f32; height];
    for x in 0..width
    {
        for y in 0..height
        {
            column[y] = f[y * width + x];
        }
        edt_1d(&column, &mut scratch[..height]);
        for y in 0..height
        {
            f[y * width + x] = scratch[y];
        }
    }
    let mut result = vec![0.0f32; width * height];
    for y in 0..height
    {
        let row = &f[y * width..(y + 1) * width];
        edt_1d(row, &mut result[y * width..(y + 1) * width]);
    }
    for value in &mut result
    {
        *value = value.sqrt();
    }
    result
}

fn coverage_to_sdf(coverage: &[u8], width: usize, height: usize) -> Vec<u8>
{
    const SPREAD: f32 = 4.0;
    let inside: Vec<u8> = coverage.iter().map(|value| u8::from(*value >= 128)).collect();
    let outside: Vec<u8> = inside.iter().map(|value| 1 - *value).collect();
    let to_inside = distance_to_seed(&inside, width, height);
    let to_outside = distance_to_seed(&outside, width, height);
    let mut sdf = vec![0u8; coverage.len()];
    for index in 0..coverage.len()
    {
        let signed = if inside[index] != 0
        {
            to_outside[index]
        }
        else
        {
            -to_inside[index]
        };
        let encoded = (0.5 + signed / (2.0 * SPREAD)).clamp(0.0, 1.0);
        sdf[index] = (encoded * 255.0 + 0.5) as u8;
    }
    sdf
}

#[no_mangle]
pub extern "C" fn resonate_text_glyph_raster(font: *const ResonateTextFont, glyph_id: u32, size: f32,
                                             out: *mut ResonateTextBitmap) -> i32
{
    if out.is_null() || size <= 0.0
    {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let font = unsafe { font_ref(font) }?;
        let glyphs = font.outline_glyphs();
        let glyph = glyphs.get(GlyphId::new(glyph_id))?;
        let location = font.axes().location([("wght", 400.0)]);
        let mut pen = PathPen { commands: Vec::new() };
        let metrics = glyph
            .draw(
                DrawSettings::unhinted(Size::new(size), LocationRef::from(&location)),
                &mut pen,
            )
            .ok()?;

        let advance = metrics.advance_width.unwrap_or(0.0);
        if pen.commands.is_empty()
        {
            unsafe
            {
                *out = ResonateTextBitmap { advance, ..ResonateTextBitmap::default() };
            }
            return Some(());
        }

        // Skrifa outlines are y-up; the atlas is y-down, so flip before
        // measuring and rasterizing.
        let flip = Transform::new(1.0, 0.0, 0.0, -1.0, 0.0, 0.0);
        let placed: Vec<Command> = pen.commands.iter().map(|c| c.transform(&flip)).collect();
        let bounds = zeno::bounds(&placed, Fill::NonZero, None);
        // Room for the signed-distance spread on every side.
        let pad = 5.0f32;
        let left = bounds.min.x.floor();
        let top = bounds.min.y.floor();
        let width = (bounds.max.x.ceil() - left + 2.0 * pad).max(0.0) as u32;
        let height = (bounds.max.y.ceil() - top + 2.0 * pad).max(0.0) as u32;
        if width == 0 || height == 0
        {
            unsafe
            {
                *out = ResonateTextBitmap { advance, ..ResonateTextBitmap::default() };
            }
            return Some(());
        }

        let translation = Transform::translation(pad - left, pad - top);
        let translated: Vec<Command> = placed.iter().map(|c| c.transform(&translation)).collect();
        let mut mask = Mask::new(&translated);
        mask.size(width, height).origin(Origin::TopLeft);
        let (coverage, _) = mask.render();
        let pixels = coverage_to_sdf(&coverage, width as usize, height as usize);

        let bitmap = pixels.into_boxed_slice();
        let pointer = Box::into_raw(bitmap) as *mut u8;
        unsafe
        {
            *out = ResonateTextBitmap {
                width,
                height,
                bearing_x: (left - pad) as i32,
                bearing_y: (pad - bounds.min.y) as i32,
                advance,
                pixels: pointer,
            };
        }
        Some(())
    }));
    match result
    {
        Ok(Some(())) => 1,
        _ => 0,
    }
}

#[no_mangle]
pub extern "C" fn resonate_text_bitmap_free(bitmap: *mut ResonateTextBitmap)
{
    if bitmap.is_null()
    {
        return;
    }
    unsafe
    {
        let out = &mut *bitmap;
        if out.pixels.is_null() || out.width == 0 || out.height == 0
        {
            return;
        }
        let length = out.width as usize * out.height as usize;
        let slice = std::ptr::slice_from_raw_parts_mut(out.pixels, length);
        drop(Box::from_raw(slice));
        *out = ResonateTextBitmap::default();
    }
}

