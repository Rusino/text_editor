/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tools/text_editor/include/TextEditorPainter.h"
#include <vector>
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"

namespace skia::text_editor {

void TextEditorPainter::Paint(SkCanvas* canvas,
                              const TextEditorViewModel& viewModel,
                              const PaintOptions& options) {
    if (!canvas) {
        return;
    }

    canvas->save();
    canvas->translate(options.origin.fX, options.origin.fY);

    // 1. Draw Selection Rectangles (Pure Consumer of ViewModel Projection - Invariant 13)
    if (!viewModel.selection().is_collapsed()) {
        SkPaint selPaint;
        selPaint.setColor4f(options.selection_color);
        selPaint.setStyle(SkPaint::kFill_Style);

        for (const SkRect& r : viewModel.screenSelectionRects()) {
            canvas->drawRect(r, selPaint);
        }
    }

    // 2. Draw Text Glyphs via Zero-Allocation Streaming Visitor
    canvas->save();
    canvas->translate(-viewModel.scrollOffset().fX, -viewModel.scrollOffset().fY);
    SkRect localClip = canvas->getLocalClipBounds();
    viewModel.document().visitDocumentRuns(localClip, [&](const RenderRun& run) {
        if (run.glyphs.empty()) {
            return;
        }
        SkPaint textPaint;
        textPaint.setColor4f(run.color);
        textPaint.setAntiAlias(true);

        canvas->drawGlyphs(run.glyphs, run.positions, SkPoint::Make(0, 0), run.font, textPaint);
    });
    canvas->restore();

    // 3. Draw Caret (Pure Consumer of ViewModel Projection - Invariant 13)
    if (options.show_caret && viewModel.isCaretVisible() && viewModel.selection().is_collapsed()) {
        SkRect caretRect = viewModel.screenCaretRect();
        caretRect.fRight = caretRect.fLeft + options.caret_width;

        SkPaint caretPaint;
        caretPaint.setColor4f(options.caret_color);
        caretPaint.setStyle(SkPaint::kFill_Style);
        canvas->drawRect(caretRect, caretPaint);
    }

    canvas->restore();
}

}  // namespace skia::text_editor
