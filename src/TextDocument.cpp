/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tools/text_editor/include/TextDocument.h"
#include <algorithm>

namespace skia::text_editor {

TextDocument::TextDocument(std::string initial_text,
                           SkFont default_font,
                           SkColor4f text_color,
                           LayoutConstraints constraints)
        : fText(std::move(initial_text)), fConstraints(std::move(constraints)), fRevision(0) {
    fStyles = {{TextRange(TextIndex(0), TextIndex(fText.size())),
                std::move(default_font),
                text_color}};
    rebuildPipeline();
}

TextDocument::TextDocument(std::string initial_text,
                           std::vector<StyleSpan> styles,
                           LayoutConstraints constraints)
        : fText(std::move(initial_text))
        , fStyles(std::move(styles))
        , fConstraints(std::move(constraints))
        , fRevision(0) {
    if (fStyles.empty()) {
        fStyles.push_back({TextRange(TextIndex(0), TextIndex(fText.size())),
                           SkFont(),
                           SkColor4f{0, 0, 0, 1}});
    }
    rebuildPipeline();
}

void TextDocument::visitDocumentRuns(const SkRect& docClip, RenderRunVisitor visitor) const {
    if (fSpatialIndex) {
        fSpatialIndex->formatted().visitParagraphRuns(docClip, std::move(visitor));
    }
}

void TextDocument::insert(TextIndex pos, std::string_view utf8_text) {
    if (utf8_text.empty()) {
        return;
    }
    size_t insertPos = std::min(pos.value, fText.size());
    fText.insert(insertPos, utf8_text);
    shiftStylesOnInsert(insertPos, utf8_text.size());
    ++fRevision;
    rebuildPipeline();
}

void TextDocument::erase(TextRange range) {
    if (range.empty() || fText.empty()) {
        return;
    }
    size_t start = std::min(range.start.value, fText.size());
    size_t len = std::min(range.length(), fText.size() - start);
    if (len == 0) {
        return;
    }
    fText.erase(start, len);
    shiftStylesOnErase(start, len);
    ++fRevision;
    rebuildPipeline();
}

void TextDocument::replace(TextRange range, std::string_view utf8_text) {
    size_t start = std::min(range.start.value, fText.size());
    size_t len = std::min(range.length(), fText.size() - start);
    fText.replace(start, len, utf8_text);
    shiftStylesOnErase(start, len);
    shiftStylesOnInsert(start, utf8_text.size());
    ++fRevision;
    rebuildPipeline();
}

void TextDocument::setStyles(std::vector<StyleSpan> styles) {
    fStyles = std::move(styles);
    if (fStyles.empty()) {
        fStyles.push_back({TextRange(TextIndex(0), TextIndex(fText.size())),
                           SkFont(),
                           SkColor4f{0, 0, 0, 1}});
    }
    ++fRevision;
    rebuildPipeline();
}

void TextDocument::setConstraints(LayoutConstraints constraints) {
    fConstraints = std::move(constraints);
    ++fRevision;
    rebuildPipeline();
}

void TextDocument::rebuildPipeline() {
    auto unicodePara = UnicodeParagraph::Make(fText, fStyles);
    auto shapedPara = ShapedParagraph::Make(std::move(unicodePara));
    auto formattedPara = FormattedParagraph::Make(std::move(shapedPara), fConstraints);
    fSpatialIndex = ParagraphSpatialIndex::Make(std::move(formattedPara));
}

void TextDocument::shiftStylesOnInsert(size_t pos, size_t len) {
    if (fStyles.empty() || len == 0) {
        return;
    }

    if (pos == 0 && fStyles[0].range.start.value == 0) {
        fStyles[0].range.end = fStyles[0].range.end + len;
        for (size_t i = 1; i < fStyles.size(); ++i) {
            fStyles[i].range.start = fStyles[i].range.start + len;
            fStyles[i].range.end = fStyles[i].range.end + len;
        }
        return;
    }

    bool expanded = false;
    for (auto& span : fStyles) {
        if (!expanded && span.range.start.value < pos && span.range.end.value >= pos) {
            span.range.end = span.range.end + len;
            expanded = true;
        } else if (span.range.start.value >= pos) {
            span.range.start = span.range.start + len;
            span.range.end = span.range.end + len;
        }
    }

    if (!expanded && !fStyles.empty()) {
        fStyles.back().range.end = fStyles.back().range.end + len;
    }
}

void TextDocument::shiftStylesOnErase(size_t start, size_t len) {
    size_t eraseEnd = start + len;
    for (auto& span : fStyles) {
        size_t sStart = span.range.start.value;
        size_t sEnd = span.range.end.value;

        if (sStart >= eraseEnd) {
            span.range.start = TextIndex(sStart - len);
        } else if (sStart > start) {
            span.range.start = TextIndex(start);
        }

        if (sEnd >= eraseEnd) {
            span.range.end = TextIndex(sEnd - len);
        } else if (sEnd > start) {
            span.range.end = TextIndex(start);
        }
    }
}

}  // namespace skia::text_editor
