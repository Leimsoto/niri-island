use crate::effects::Assets;
use crate::model::{Properties, Texture};
use crate::pkg::Package;
use serde_json::Value;

mod font;
pub mod script;
pub(crate) mod system_font;

pub const EM_PER_POINT: f32 = 300.0 / 72.0;
pub const FALLBACK_FONT: &str = "fonts/NotoSans-Regular.ttf";
const TEXT_INPUT_KIND: &str = "textinput";
const PROPERTIES_PATH: &str = "/general/properties";
const OBJECTS_FIELD: &str = "objects";
const TEXT_FIELD: &str = "text";
const USER_FIELD: &str = "user";
const TYPE_FIELD: &str = "type";
const VALUE_FIELD: &str = "value";
const LIMIT_WIDTH_FIELD: &str = "limitwidth";
const MAX_WIDTH_FIELD: &str = "maxwidth";
const LIMIT_ROWS_FIELD: &str = "limitrows";
const MAX_ROWS_FIELD: &str = "maxrows";
const LINE_BREAK: &str = "\n";
const MAX_TEXT_EDGE: u32 = 8192;
const MAX_TEXT_CHARS: usize = 4096;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HAlign {
    Left,
    Center,
    Right,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VAlign {
    Top,
    Center,
    Bottom,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Metrics {
    pub width: f32,
    pub ascent: f32,
    pub descent: f32,
    pub line_height: f32,
    pub lines: usize,
}

impl Metrics {
    #[must_use]
    pub fn height(&self) -> f32 {
        self.ascent + self.descent + self.line_height * (self.lines.max(1) - 1) as f32
    }
}

#[must_use]
pub fn box_offset(metrics: &Metrics, halign: HAlign, valign: VAlign) -> (f32, f32) {
    let x = match halign {
        HAlign::Left => 0.0,
        HAlign::Center => -metrics.width * 0.5,
        HAlign::Right => -metrics.width,
    };
    let y = match valign {
        VAlign::Top => 0.0,
        VAlign::Center => {
            -metrics.ascent * 0.5 - metrics.line_height * (metrics.lines.max(1) - 1) as f32 * 0.5
        }
        VAlign::Bottom => -metrics.height(),
    };
    (x, y)
}

pub struct Rendered {
    pub texture: Texture,
    pub metrics: Metrics,
    pub offset: (f32, f32),
    pub live: Option<Prepared>,
}

pub struct Prepared {
    font: Vec<u8>,
    em: f32,
    halign: HAlign,
    width: u32,
    height: u32,
    placeholder: String,
    weekday: Option<Vec<String>>,
    clock: Option<crate::dynamic_text::clock::Clock>,
    date: Option<crate::dynamic_text::date::Date>,
}

impl Prepared {
    #[must_use]
    pub fn value(&self, now: crate::dynamic_text::LocalTime) -> String {
        if let Some(clock) = &self.clock {
            return clock.value(now);
        }
        if let Some(date) = &self.date {
            return date.value(now);
        }
        crate::dynamic_text::substitute_with(&self.placeholder, now, self.weekday.as_deref())
            .unwrap_or_else(|| self.placeholder.clone())
    }

    #[must_use]
    pub fn cadence(&self) -> crate::dynamic_text::Cadence {
        self.clock.as_ref().map_or_else(
            || crate::dynamic_text::cadence(&self.placeholder),
            crate::dynamic_text::clock::Clock::cadence,
        )
    }

    #[must_use]
    pub fn rasterize(&self, text: &str) -> Option<Texture> {
        let face = Face::parse(&self.font, self.em)?;
        face.rasterize_in(text, self.halign, self.width, self.height)
    }
}

const LIVE_HEADROOM: f32 = 1.35;

fn dynamic_placeholder(object: &Value) -> Option<String> {
    let Value::Object(map) = object.get("text")? else {
        return None;
    };
    map.get("script")?;
    let placeholder = text_value(map.get("value"))?;
    let now = crate::dynamic_text::local_now()?;
    crate::dynamic_text::clock::Clock::from_text(object.get("text")?)
        .map(|clock| clock.value(now))
        .or_else(|| {
            crate::dynamic_text::date::Date::from_text(object.get("text")?)
                .map(|date| date.value(now))
        })
        .or_else(|| crate::dynamic_text::substitute(&placeholder, now))
        .map(|_| placeholder)
}

fn halign_of(value: Option<&Value>) -> HAlign {
    match value.and_then(Value::as_str).map(str::to_ascii_lowercase).as_deref() {
        Some("left") => HAlign::Left,
        Some("right") => HAlign::Right,
        _ => HAlign::Center,
    }
}

fn valign_of(value: Option<&Value>) -> VAlign {
    match value.and_then(Value::as_str).map(str::to_ascii_lowercase).as_deref() {
        Some("top") => VAlign::Top,
        Some("bottom") => VAlign::Bottom,
        _ => VAlign::Center,
    }
}

#[must_use]
pub fn text_value(value: Option<&Value>) -> Option<String> {
    match value? {
        Value::String(text) => Some(text.clone()),
        Value::Number(number) => Some(number.to_string()),
        Value::Object(map) => text_value(map.get("value")),
        _ => None,
    }
}

pub(crate) fn bind_user_values(scene: &mut Value, project: &Value) {
    let Some(declarations) = project.pointer(PROPERTIES_PATH).and_then(Value::as_object) else {
        return;
    };
    let Some(objects) = scene.get_mut(OBJECTS_FIELD).and_then(Value::as_array_mut) else {
        return;
    };
    for object in objects {
        let Some(text) = object.get_mut(TEXT_FIELD).and_then(Value::as_object_mut) else {
            continue;
        };
        let Some(name) = text.get(USER_FIELD).and_then(Value::as_str) else {
            continue;
        };
        let selected = declarations
            .iter()
            .find(|(key, _)| key.eq_ignore_ascii_case(name))
            .map(|(_, entry)| entry)
            .filter(|entry| entry.get(TYPE_FIELD).and_then(Value::as_str) == Some(TEXT_INPUT_KIND))
            .and_then(|entry| entry.get(VALUE_FIELD))
            .and_then(Value::as_str);
        if let Some(value) = selected {
            text.insert(
                VALUE_FIELD.into(),
                Value::String(value.chars().take(MAX_TEXT_CHARS).collect()),
            );
        }
    }
}

#[derive(Clone, Copy)]
struct TextLimits {
    width: Option<f32>,
    rows: Option<usize>,
}

impl TextLimits {
    fn of(object: &Value, props: &Properties) -> Self {
        let width = object
            .get(LIMIT_WIDTH_FIELD)
            .and_then(Value::as_bool)
            .filter(|enabled| *enabled)
            .map(|_| crate::model::number(object.get(MAX_WIDTH_FIELD), props, MAX_TEXT_EDGE as f32))
            .filter(|width| width.is_finite() && *width > 0.0)
            .map(|width| width.min(MAX_TEXT_EDGE as f32));
        let rows = object
            .get(LIMIT_ROWS_FIELD)
            .and_then(Value::as_bool)
            .filter(|enabled| *enabled)
            .map(|_| crate::model::number(object.get(MAX_ROWS_FIELD), props, MAX_TEXT_CHARS as f32))
            .filter(|rows| rows.is_finite() && *rows > 0.0)
            .map(|rows| rows.min(MAX_TEXT_CHARS as f32) as usize);
        Self { width, rows }
    }

    fn apply(self, face: &Face, text: &str) -> String {
        wrap_text(text, self.width, self.rows, |line| {
            let (left, right) = face.line_box(line);
            right - left
        })
    }
}

fn wrap_text(
    text: &str,
    width: Option<f32>,
    rows: Option<usize>,
    measure: impl Fn(&str) -> f32,
) -> String {
    if rows == Some(0) {
        return String::new();
    }
    let mut lines = Vec::new();
    for source in text.split('\n') {
        let Some(width) = width else {
            if push_line(&mut lines, source.to_owned(), rows) {
                return lines.join(LINE_BREAK);
            }
            continue;
        };
        let mut line = String::new();
        for word in source.split_inclusive(' ') {
            let before = line.len();
            line.push_str(word);
            if before > 0 && measure(line.trim_end()) > width {
                let remainder = line.split_off(before);
                if push_line(&mut lines, line.trim_end().to_owned(), rows) {
                    return lines.join(LINE_BREAK);
                }
                remainder.trim_start().clone_into(&mut line);
            }
            while measure(line.trim_end()) > width {
                let split = line
                    .char_indices()
                    .map(|(index, ch)| index + ch.len_utf8())
                    .take_while(|end| measure(line[..*end].trim_end()) <= width)
                    .last()
                    .unwrap_or_else(|| line.chars().next().map_or(0, char::len_utf8));
                if split == line.len() {
                    break;
                }
                if push_line(&mut lines, line[..split].trim_end().to_owned(), rows) {
                    return lines.join(LINE_BREAK);
                }
                line = line[split..].trim_start().to_owned();
            }
        }
        if push_line(&mut lines, line.trim_end().to_owned(), rows) {
            return lines.join(LINE_BREAK);
        }
    }
    lines.join(LINE_BREAK)
}

fn push_line(lines: &mut Vec<String>, line: String, rows: Option<usize>) -> bool {
    lines.push(line);
    rows.is_some_and(|rows| lines.len() >= rows)
}

fn font_bytes(pkg: &Package, assets: &Assets, name: &str) -> Option<Vec<u8>> {
    if let Some(family) = name.strip_prefix("systemfont_") {
        return assets
            .system_fonts
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .resolve(assets, family);
    }
    if !name.is_empty() {
        if let Some(bytes) = pkg.find(name) {
            return Some(bytes.to_vec());
        }
        if let Some(bytes) = assets.read_bytes(name) {
            return Some(bytes);
        }
    }
    assets.read_bytes(FALLBACK_FONT)
}

pub struct Face {
    font: font::Font,
}

impl Face {
    pub fn parse(bytes: &[u8], em: f32) -> Option<Self> {
        Some(Self { font: font::Font::parse(bytes, em)? })
    }

    fn line_box(&self, line: &str) -> (f32, f32) {
        let mut cursor = 0.0f32;
        let mut ink_min = 0.0f32;
        let mut ink_max = 0.0f32;
        let mut previous = None;
        for ch in line.chars() {
            if let Some(prev) = previous {
                cursor += self.font.kern(prev, ch);
            }
            let glyph = self.font.metrics(ch);
            if glyph.width > 0 {
                ink_min = ink_min.min(cursor + glyph.xmin as f32);
                ink_max = ink_max.max(cursor + glyph.xmin as f32 + glyph.width as f32);
            } else {
                ink_max = ink_max.max(cursor + glyph.advance_width);
            }
            cursor += glyph.advance_width;
            previous = Some(ch);
        }
        (ink_min, ink_max)
    }

    #[must_use]
    pub fn measure(&self, text: &str) -> Option<Metrics> {
        let line = self.font.line_metrics();
        let lines: Vec<&str> = text.split('\n').collect();
        let width = lines
            .iter()
            .map(|line| {
                let (left, right) = self.line_box(line);
                right - left
            })
            .fold(0.0, f32::max);
        Some(Metrics {
            width,
            ascent: line.ascent.max(0.0),
            descent: (-line.descent).max(0.0),
            line_height: line.new_line_size.max(1.0),
            lines: lines.len(),
        })
    }

    #[must_use]
    fn draw(
        &self,
        text: &str,
        halign: HAlign,
        metrics: &Metrics,
        width: u32,
        height: u32,
        box_width: f32,
        top: f32,
    ) -> Vec<u8> {
        let mut data = vec![0u8; width as usize * height as usize * 4];
        for (index, line) in text.split('\n').enumerate() {
            let (left, right) = self.line_box(line);
            let line_width = right - left;
            let mut cursor = 1.0 - left
                + match halign {
                    HAlign::Left => 0.0,
                    HAlign::Center => (box_width - line_width) * 0.5,
                    HAlign::Right => box_width - line_width,
                };
            let baseline = top + metrics.ascent + metrics.line_height * index as f32;
            let mut previous = None;
            for ch in line.chars() {
                if let Some(prev) = previous {
                    cursor += self.font.kern(prev, ch);
                }
                let (glyph, bitmap) = self.font.rasterize(ch);
                let x0 = (cursor + glyph.xmin as f32).round() as i64;
                let y0 = (baseline - (glyph.ymin + glyph.height as i32) as f32).round() as i64;
                for row in 0..glyph.height {
                    let y = y0 + row as i64;
                    if y < 0 || y >= i64::from(height) {
                        continue;
                    }
                    for col in 0..glyph.width {
                        let x = x0 + col as i64;
                        if x < 0 || x >= i64::from(width) {
                            continue;
                        }
                        let coverage = bitmap[row * glyph.width + col];
                        let at = ((y as usize) * width as usize + x as usize) * 4;
                        data[at] = 255;
                        data[at + 1] = 255;
                        data[at + 2] = 255;
                        data[at + 3] = data[at + 3].max(coverage);
                    }
                }
                cursor += glyph.advance_width;
                previous = Some(ch);
            }
        }
        data
    }

    fn texture_of(width: u32, height: u32, data: Vec<u8>) -> Texture {
        Texture {
            video: None,
            pages: Vec::new(),
            system_texture: None,
            image_count: 1,
            width,
            height,
            img_width: width,
            img_height: height,
            pixels: crate::tex::Pixels::rgba(width, height, data),
            frames: Vec::new(),
            clamp: true,
            nearest: false,
            format: crate::tex::TexFormat::Rgba8888,
        }
    }

    pub fn rasterize(&self, text: &str, halign: HAlign) -> Option<(Metrics, Texture)> {
        let metrics = self.measure(text)?;
        let width = (metrics.width.ceil() as u32 + 2).clamp(1, MAX_TEXT_EDGE);
        let height = (metrics.height().ceil() as u32 + 2).clamp(1, MAX_TEXT_EDGE);
        let data = self.draw(text, halign, &metrics, width, height, metrics.width, 1.0);
        Some((metrics, Self::texture_of(width, height, data)))
    }

    pub fn rasterize_in(
        &self,
        text: &str,
        halign: HAlign,
        width: u32,
        height: u32,
    ) -> Option<Texture> {
        let metrics = self.measure(text)?;
        let top = ((f32::from(u16::try_from(height).unwrap_or(u16::MAX)) - metrics.height()) * 0.5)
            .max(1.0);
        let data = self.draw(
            text,
            halign,
            &metrics,
            width,
            height,
            f32::from(u16::try_from(width).unwrap_or(u16::MAX)) - 2.0,
            top,
        );
        Some(Self::texture_of(width, height, data))
    }
}

pub fn render(
    pkg: &Package,
    assets: &Assets,
    object: &Value,
    props: &Properties,
) -> Option<Rendered> {
    let mut text = crate::dynamic_text::local_now()
        .and_then(|now| crate::dynamic_text::resolve(object.get("text"), now))
        .or_else(|| text_value(object.get("text")))?;
    if text.chars().count() > MAX_TEXT_CHARS {
        text = text.chars().take(MAX_TEXT_CHARS).collect();
    }
    if text.trim().is_empty() {
        return None;
    }
    let pointsize = crate::model::number(object.get("pointsize"), props, 30.0).clamp(1.0, 1024.0);
    let em = pointsize * EM_PER_POINT;
    let font_name = object.get("font").and_then(Value::as_str).unwrap_or("");
    let bytes = font_bytes(pkg, assets, font_name)?;
    let face = Face::parse(&bytes, em)?;
    let text = TextLimits::of(object, props).apply(&face, &text);
    let halign = halign_of(object.get("horizontalalign"));
    let valign = valign_of(object.get("verticalalign"));
    let (metrics, texture) = face.rasterize(&text, halign)?;
    let offset = box_offset(&metrics, halign, valign);
    let Some(placeholder) = dynamic_placeholder(object) else {
        return Some(Rendered { texture, metrics, offset, live: None });
    };
    let width = ((texture.width as f32 * LIVE_HEADROOM).ceil() as u32).clamp(1, MAX_TEXT_EDGE);
    let weekday = object
        .get("text")
        .and_then(|text| text.get("script"))
        .and_then(Value::as_str)
        .and_then(crate::dynamic_text::weekday_table);
    let clock = object.get("text").and_then(crate::dynamic_text::clock::Clock::from_text);
    let date = object.get("text").and_then(crate::dynamic_text::date::Date::from_text);
    let live = Prepared {
        font: bytes,
        em,
        halign,
        width,
        height: texture.height,
        placeholder,
        weekday,
        clock,
        date,
    };
    let Some(padded) = live.rasterize(&text) else {
        return Some(Rendered { texture, metrics, offset, live: None });
    };
    let slack = (padded.width as f32 - texture.width as f32) * 0.5;
    let offset = (offset.0 - slack, offset.1);
    Some(Rendered { texture: padded, metrics, offset, live: Some(live) })
}

#[cfg(test)]
mod tests;
