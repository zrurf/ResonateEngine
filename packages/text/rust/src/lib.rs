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
        let pad = 1.0f32;
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
        let (pixels, _) = mask.render();

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

