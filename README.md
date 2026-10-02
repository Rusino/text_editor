# Skia Text Editor

A 4-layer immutable text layout and editing engine built on Skia, HarfBuzz, and SkUnicode (ICU), using a clean Model-View-ViewModel (MVVM) architecture.

## Architecture

1. **Layer 1 (`UnicodeParagraph`)**: UTF-8 itemization, UAX #9 BiDi level resolution, UAX #14 line break opportunities, UAX #29 grapheme & word segmentation, and character-level font fallback.
2. **Layer 2 (`ShapedParagraph`)**: Single-pass HarfBuzz shaping with strict 1-to-1 cluster preservation (`liga=0`, `ccmp=0`) and OpenType `GPOS` mark positioning.
3. **Layer 3 (`FormattedParagraph`)**: UAX #14 word-boundary line wrapping, UAX #9 Rule L2 visual run reordering, dynamic vertical bounds for stacked combining marks, and terminal ellipsis truncation.
4. **Layer 4 (`ParagraphSpatialIndex`)**: Precomputed spatial cluster and glyph geometry providing zero-allocation hot-path hit-testing, visual/logical caret navigation, and multi-line/BiDi selection geometry.
5. **Model (`TextDocument`)**: Encapsulates document text, style spans, layout constraints, and the 4 immutable layout layers.
6. **ViewModel (`TextEditorViewModel`)**: Headless session state, selection management, undo/redo command history with typing coalescing, clipboard interop, and input sanitization.
7. **Painter (`TextEditorPainter`)**: Stateless canvas renderer consuming streaming glyph runs and ViewModel geometry projections.

## Directory Structure

- `include/` — Public C++20 headers for all layout layers, document model, view model, and painter.
- `src/` — Implementation of the layout pipeline, spatial index, document model, view model, and painter.
- `app/` — Interactive desktop application (`TextEditorApp`) built on `sk_app`.
- `tests/` — Unit and regression test suite (`dm --match TextEditor`).
- `fuzz/corpus/` — Seed corpus for continuous fuzzing and differential buffer testing.
- `docs/` — Architectural design documentation.
- `INVARIANTS.md` — Domain and architectural invariants.
