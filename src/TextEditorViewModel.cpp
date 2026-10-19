/*
 * Copyright 2026 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tools/text_editor/include/TextEditorViewModel.h"
#include <algorithm>
#include <cmath>
#include "include/core/SkFontMetrics.h"
#include "src/core/SkUTF.h"

namespace skia::text_editor {

TextEditorViewModel::TextEditorViewModel(std::unique_ptr<TextDocument> document,
                                         NavigationMode default_nav_mode)
        : fDocument(std::move(document)), fNavMode(default_nav_mode) {
    updateCursorPosition(0);
}

TextEditorViewModel::TextEditorViewModel(std::string initial_text,
                                         SkFont default_font,
                                         SkColor4f text_color,
                                         LayoutConstraints constraints,
                                         NavigationMode default_nav_mode)
        : fDocument(std::make_unique<TextDocument>(std::move(initial_text),
                                                   std::move(default_font),
                                                   text_color,
                                                   std::move(constraints)))
        , fNavMode(default_nav_mode) {
    updateCursorPosition(0);
}

namespace {

// Sanitizes input text per Domain Invariant 14:
// - Normalizes \r\n and single \r to \n
// - Expands \t to 4 soft spaces
// - Drops ASCII C0 (< 32, except \n), DEL (127), and binary nulls
// - Preserves all valid UTF-8 sequences including Unicode Format Controls (Cf, like
// ZWJ/ZWNJ/LRM/RLM)
std::string SanitizeInputText(std::string_view raw) {
    std::string sanitized;
    sanitized.reserve(raw.size());

    const char* ptr = raw.data();
    const char* end = ptr + raw.size();

    while (ptr < end) {
        // Check for CRLF or CR
        if (*ptr == '\r') {
            ptr++;
            if (ptr < end && *ptr == '\n') {
                ptr++;
            }
            sanitized.push_back('\n');
            continue;
        }

        // Check for Tab
        if (*ptr == '\t') {
            sanitized.append("    ");
            ptr++;
            continue;
        }

        // Check single-byte ASCII
        unsigned char byte = static_cast<unsigned char>(*ptr);
        if (byte < 0x80) {
            if (byte == '\n' || byte >= 32) {
                if (byte != 0x7F) {  // Exclude DEL
                    sanitized.push_back(static_cast<char>(byte));
                }
            }
            ptr++;
            continue;
        }

        // Multi-byte UTF-8 sequence
        const char* prev = ptr;
        SkUnichar u = SkUTF::NextUTF8(&ptr, end);
        if (u < 0) {
            // Invalid UTF-8 sequence, skip bad byte
            ptr = prev + 1;
            continue;
        }

        // Exclude Unicode C1 controls (0x80 - 0x9F) and macOS function key PUA (0xF700 - 0xF8FF)
        if ((u >= 0x80 && u <= 0x9F) || (u >= 0xF700 && u <= 0xF8FF)) {
            continue;
        }

        // Append valid multi-byte sequence (includes Cf category like ZWJ/ZWNJ/LRM/RLM)
        sanitized.append(prev, ptr - prev);
    }

    return sanitized;
}

// Zero-allocation case-sensitive and case-insensitive byte matching across std::string_view
bool MatchesAt(std::string_view text, size_t pos, std::string_view query, bool matchCase) {
    if (pos + query.size() > text.size()) {
        return false;
    }
    if (matchCase) {
        return text.substr(pos, query.size()) == query;
    }
    for (size_t k = 0; k < query.size(); ++k) {
        if (std::tolower(static_cast<unsigned char>(text[pos + k])) !=
            std::tolower(static_cast<unsigned char>(query[k]))) {
            return false;
        }
    }
    return true;
}

// Forward search with wrap-around without intermediate heap allocations
size_t FindForward(std::string_view text, std::string_view query, size_t startPos, bool matchCase) {
    if (query.empty() || text.size() < query.size()) {
        return std::string_view::npos;
    }
    size_t limit = text.size() - query.size();
    if (startPos <= limit) {
        for (size_t i = startPos; i <= limit; ++i) {
            if (MatchesAt(text, i, query, matchCase)) {
                return i;
            }
        }
    }
    size_t wrapLimit = std::min(limit, startPos);
    for (size_t i = 0; i < wrapLimit; ++i) {
        if (MatchesAt(text, i, query, matchCase)) {
            return i;
        }
    }
    return std::string_view::npos;
}

// Backward search with wrap-around without intermediate heap allocations
size_t FindBackward(std::string_view text,
                    std::string_view query,
                    size_t startPos,
                    bool matchCase) {
    if (query.empty() || text.size() < query.size()) {
        return std::string_view::npos;
    }
    size_t limit = text.size() - query.size();
    size_t cur = std::min(startPos, limit);
    while (true) {
        if (MatchesAt(text, cur, query, matchCase)) {
            return cur;
        }
        if (cur == 0) {
            break;
        }
        --cur;
    }
    if (limit > startPos) {
        cur = limit;
        while (cur > startPos) {
            if (MatchesAt(text, cur, query, matchCase)) {
                return cur;
            }
            --cur;
        }
    }
    return std::string_view::npos;
}

struct LineClusterInfo {
    TextRange text_range;
    SkRect bounds;
    bool is_rtl{false};
};

std::vector<LineClusterInfo> getLineClusters(const LineBox& line,
                                             std::string_view fullText,
                                             SkSpan<const TextIndex> graphemeBreaks) {
    std::vector<LineClusterInfo> clusters;
    for (const auto& vr : line.visual_runs) {
        SkScalar curX = vr.x_offset;
        for (const auto& g : vr.glyphs) {
            TextIndex endIdx = g.cluster_text_index + 1;
            if (g.cluster_text_index.value < fullText.size()) {
                const char* ptr = fullText.data() + g.cluster_text_index.value;
                const char* end = fullText.data() + fullText.size();
                SkUTF::NextUTF8(&ptr, end);
                endIdx = TextIndex(ptr - fullText.data());
            }

            if (!graphemeBreaks.empty()) {
                auto it = std::upper_bound(
                        graphemeBreaks.begin(), graphemeBreaks.end(), g.cluster_text_index);
                if (it != graphemeBreaks.end() && *it > endIdx) {
                    endIdx = *it;
                }
            }

            SkScalar cbWidth = g.advance.fX;
            SkScalar cbHeight = std::abs(vr.ascent) + std::abs(vr.descent);
            if (cbHeight <= 0) {
                cbHeight = 16.0f;
            }

            if (!clusters.empty()) {
                LineClusterInfo& lastCb = clusters.back();
                if (g.is_mark || lastCb.text_range.contains(g.cluster_text_index) ||
                    g.cluster_text_index == lastCb.text_range.start) {
                    lastCb.text_range.end = std::max(lastCb.text_range.end, endIdx);
                    lastCb.bounds.fRight += g.advance.fX;
                    curX += g.advance.fX;
                    continue;
                }
            }

            LineClusterInfo cb;
            cb.text_range = TextRange(g.cluster_text_index, endIdx);
            cb.is_rtl = vr.isRTL();
            cb.bounds = SkRect::MakeXYWH(
                    curX + g.offset.fX, line.baseline + vr.ascent, cbWidth, cbHeight);
            curX += g.advance.fX;
            clusters.push_back(cb);
        }
    }
    return clusters;
}

CaretPosition resolveCaretPosition(const TextDocument& doc,
                                   size_t index,
                                   Affinity affinity = Affinity::kDownstream) {
    std::string_view text = doc.text();
    index = std::min(index, text.size());
    TextIndex textIndex = TextIndex(index);
    Affinity resolvedAffinity = (index == text.size()) ? Affinity::kUpstream : affinity;
    SkRect caretRect = SkRect::MakeEmpty();

    const auto& lines = doc.formatted().lines();
    if (lines.empty()) {
        return CaretPosition(
                textIndex, resolvedAffinity, SkRect::MakeXYWH(0.0f, 0.0f, 1.0f, 16.0f));
    }

    const auto* targetLine = &lines[0];
    for (const auto& line : lines) {
        if (index >= line.text_range.start.value && index < line.text_range.end.value) {
            targetLine = &line;
            break;
        }
    }
    if (index >= lines.back().text_range.end.value) {
        targetLine = &lines.back();
    }

    SkScalar caretTop = targetLine->baseline + targetLine->typographic_ascent;
    SkScalar caretHeight =
            std::abs(targetLine->typographic_ascent) + std::abs(targetLine->typographic_descent);
    if (caretHeight <= 0.0f) {
        caretHeight = 16.0f;
    }

    auto clusters = getLineClusters(*targetLine, doc.text(), doc.unicode().grapheme_breaks());
    if (clusters.empty()) {
        return CaretPosition(
                textIndex,
                resolvedAffinity,
                SkRect::MakeXYWH(targetLine->bounds.fLeft, caretTop, 1.0f, caretHeight));
    }

    if (affinity == Affinity::kUpstream && index > 0) {
        const LineClusterInfo* prevCluster = nullptr;
        for (const auto& cb : clusters) {
            if (cb.text_range.contains(TextIndex(index - 1))) {
                prevCluster = &cb;
                break;
            }
        }
        if (prevCluster) {
            SkScalar x =
                    prevCluster->is_rtl ? prevCluster->bounds.fLeft : prevCluster->bounds.fRight;
            caretRect = SkRect::MakeXYWH(x, caretTop, 1.0f, caretHeight);
            if (caretRect.isEmpty()) {
                caretRect = SkRect::MakeXYWH(0.0f, 0.0f, 1.0f, 16.0f);
            }
            return CaretPosition(textIndex, Affinity::kUpstream, caretRect);
        }
    }

    const LineClusterInfo* lastCluster = nullptr;
    const LineClusterInfo* lastNonWsCluster = nullptr;
    for (const auto& cb : clusters) {
        if (!lastCluster || cb.text_range.end > lastCluster->text_range.end) {
            lastCluster = &cb;
        }
        bool isWs = true;
        for (size_t i = cb.text_range.start.value; i < cb.text_range.end.value && i < text.size();
             ++i) {
            if (!doc.unicode().isWhitespace(TextIndex(i))) {
                isWs = false;
                break;
            }
        }
        if (!isWs) {
            if (!lastNonWsCluster || cb.text_range.end > lastNonWsCluster->text_range.end) {
                lastNonWsCluster = &cb;
            }
        }
    }

    if (index >= targetLine->text_range.end.value || index == text.size()) {
        if (lastCluster && lastCluster->is_rtl) {
            caretRect = SkRect::MakeXYWH(lastCluster->bounds.fLeft, caretTop, 1.0f, caretHeight);
        } else if (lastNonWsCluster && lastNonWsCluster->is_rtl) {
            caretRect =
                    SkRect::MakeXYWH(lastNonWsCluster->bounds.fLeft, caretTop, 1.0f, caretHeight);
        } else if (lastCluster) {
            caretRect = SkRect::MakeXYWH(lastCluster->bounds.fRight, caretTop, 1.0f, caretHeight);
        } else {
            caretRect = SkRect::MakeXYWH(targetLine->bounds.fRight, caretTop, 1.0f, caretHeight);
        }
    } else {
        const LineClusterInfo* match = nullptr;
        for (const auto& cb : clusters) {
            if (cb.text_range.contains(TextIndex(index))) {
                match = &cb;
                break;
            }
        }
        if (match) {
            bool isTrailingWs = (lastNonWsCluster &&
                                 match->text_range.start >= lastNonWsCluster->text_range.end);
            if (isTrailingWs && lastNonWsCluster->is_rtl) {
                caretRect = SkRect::MakeXYWH(
                        lastNonWsCluster->bounds.fLeft, caretTop, 1.0f, caretHeight);
            } else {
                SkScalar x = match->is_rtl ? match->bounds.fRight : match->bounds.fLeft;
                caretRect = SkRect::MakeXYWH(x, caretTop, 1.0f, caretHeight);
            }
        } else {
            const LineClusterInfo* firstCluster = nullptr;
            for (const auto& cb : clusters) {
                if (!firstCluster || cb.text_range.start < firstCluster->text_range.start) {
                    firstCluster = &cb;
                }
            }
            if (firstCluster && index <= firstCluster->text_range.start.value) {
                SkScalar x = firstCluster->is_rtl ? firstCluster->bounds.fRight
                                                  : firstCluster->bounds.fLeft;
                caretRect = SkRect::MakeXYWH(x, caretTop, 1.0f, caretHeight);
            } else {
                SkScalar x = (index > targetLine->text_range.start.value)
                                     ? targetLine->bounds.fRight
                                     : targetLine->bounds.fLeft;
                caretRect = SkRect::MakeXYWH(x, caretTop, 1.0f, caretHeight);
            }
        }
    }

    if (caretRect.isEmpty()) {
        caretRect = SkRect::MakeXYWH(0.0f, 0.0f, 1.0f, 16.0f);
    }
    return CaretPosition(textIndex, resolvedAffinity, caretRect);
}

}  // namespace

void TextEditorViewModel::insertText(std::string_view utf8_text) {
    std::string sanitized = SanitizeInputText(utf8_text);
    if (sanitized.empty()) {
        return;
    }

    EditorSelection selBefore = fSelection;
    std::string textBefore;
    size_t insertPos = 0;
    EditCommand::Kind kind = EditCommand::Kind::kTyping;

    if (!fSelection.is_collapsed()) {
        kind = EditCommand::Kind::kCutOrBlock;
        textBefore = copySelection();
        if (!fSelection.ranges().empty()) {
            insertPos = fSelection.ranges().front().start.value;
            for (auto it = fSelection.ranges().rbegin(); it != fSelection.ranges().rend(); ++it) {
                fDocument->erase(*it);
            }
            fDocument->insert(TextIndex(insertPos), sanitized);
            size_t finalCaret = insertPos + sanitized.size();
            updateCursorPosition(finalCaret, Affinity::kUpstream);
        } else {
            TextRange range = fSelection.text_range();
            insertPos = range.start.value;
            fDocument->replace(range, sanitized);
            size_t finalCaret = insertPos + sanitized.size();
            updateCursorPosition(finalCaret, Affinity::kUpstream);
        }
    } else {
        insertPos = std::min(fSelection.focus().text_index().value, fDocument->text().size());
        if (sanitized == "\n") {
            kind = EditCommand::Kind::kNewline;
        } else if (sanitized.size() > 4) {
            kind = EditCommand::Kind::kPaste;
        } else {
            kind = EditCommand::Kind::kTyping;
        }
        fDocument->insert(TextIndex(insertPos), sanitized);
        size_t finalCaret = insertPos + sanitized.size();
        updateCursorPosition(finalCaret, Affinity::kUpstream);
    }

    if (!fIsPerformingUndoRedo) {
        EditCommand cmd;
        cmd.kind = kind;
        cmd.position = TextIndex(insertPos);
        cmd.textBefore = std::move(textBefore);
        cmd.textAfter = sanitized;
        cmd.selectionBefore = selBefore;
        cmd.selectionAfter = fSelection;
        cmd.timestamp = std::chrono::steady_clock::now();
        pushEditCommand(std::move(cmd));
    }

    notifyRedraw();
}

void TextEditorViewModel::deleteBackward(MovementGranularity gran) {
    EditorSelection selBefore = fSelection;
    std::string textBefore;
    size_t deletePos = 0;

    if (!fSelection.is_collapsed()) {
        textBefore = copySelection();
        SkScalar cutX = 0.0f;
        SkScalar cutY = 0.0f;
        bool hasCutCoord = false;

        TextRange range = fSelection.text_range();
        std::vector<SkRect> selRects;
        if (!fSelection.ranges().empty()) {
            for (const auto& r : fSelection.ranges()) {
                std::vector<SkRect> rRects;
                fDocument->spatial_index().getSelectionRects(r, rRects);
                selRects.insert(selRects.end(), rRects.begin(), rRects.end());
            }
        } else {
            fDocument->spatial_index().getSelectionRects(range, selRects);
        }

        if (!selRects.empty()) {
            SkScalar minX = SK_ScalarMax;
            for (const auto& r : selRects) {
                minX = std::min(minX, r.fLeft);
            }
            cutX = minX;
            cutY = selRects[0].centerY();
            hasCutCoord = true;
        }

        if (!fSelection.ranges().empty()) {
            deletePos = fSelection.ranges().front().start.value;
            for (auto it = fSelection.ranges().rbegin(); it != fSelection.ranges().rend(); ++it) {
                fDocument->erase(*it);
            }
        } else {
            deletePos = range.start.value;
            fDocument->erase(range);
        }

        if (hasCutCoord && !fDocument->text().empty()) {
            CaretPosition newPos = fDocument->spatial_index().hitTest(cutX, cutY);
            fSelection.collapse_to(newPos);
        } else {
            updateCursorPosition(deletePos);
        }

        if (!fIsPerformingUndoRedo) {
            EditCommand cmd;
            cmd.kind = EditCommand::Kind::kCutOrBlock;
            cmd.position = TextIndex(deletePos);
            cmd.textBefore = std::move(textBefore);
            cmd.textAfter = "";
            cmd.selectionBefore = selBefore;
            cmd.selectionAfter = fSelection;
            cmd.timestamp = std::chrono::steady_clock::now();
            pushEditCommand(std::move(cmd));
        }
    } else {
        size_t cursor = fSelection.focus().text_index().value;
        std::string_view text = fDocument->text();
        if (cursor == 0 || text.empty()) {
            return;
        }
        size_t prevPos = cursor;
        if (gran == MovementGranularity::kWord) {
            CaretPosition target = fDocument->spatial_index().moveCaret(fSelection.focus(),
                                                                        CursorDirection::kLeft,
                                                                        MovementGranularity::kWord,
                                                                        fNavMode);
            prevPos = target.text_index().value;
        } else if (gran == MovementGranularity::kLine) {
            CaretPosition target =
                    fDocument->spatial_index().moveCaret(fSelection.focus(),
                                                         CursorDirection::kLeft,
                                                         MovementGranularity::kLine,
                                                         NavigationMode::kTextLogical);
            prevPos = target.text_index().value;
        } else {
            while (prevPos > 0) {
                --prevPos;
                if ((static_cast<uint8_t>(text[prevPos]) & 0xC0) != 0x80) {
                    break;
                }
            }
        }
        if (prevPos >= cursor) {
            return;
        }
        deletePos = prevPos;
        textBefore = std::string(text.substr(prevPos, cursor - prevPos));
        fDocument->erase(TextRange(TextIndex(prevPos), TextIndex(cursor)));
        updateCursorPosition(prevPos);

        if (!fIsPerformingUndoRedo) {
            EditCommand cmd;
            cmd.kind = EditCommand::Kind::kDelete;
            cmd.position = TextIndex(deletePos);
            cmd.textBefore = std::move(textBefore);
            cmd.textAfter = "";
            cmd.selectionBefore = selBefore;
            cmd.selectionAfter = fSelection;
            cmd.timestamp = std::chrono::steady_clock::now();
            pushEditCommand(std::move(cmd));
        }
    }
    notifyRedraw();
}

void TextEditorViewModel::deleteForward(MovementGranularity gran) {
    if (!fSelection.is_collapsed()) {
        deleteBackward(gran);
        return;
    }
    size_t cursor = fSelection.focus().text_index().value;
    std::string_view text = fDocument->text();
    if (cursor >= text.size()) {
        return;
    }
    EditorSelection selBefore = fSelection;
    size_t nextPos = cursor;
    if (gran == MovementGranularity::kWord) {
        CaretPosition target = fDocument->spatial_index().moveCaret(
                fSelection.focus(), CursorDirection::kRight, MovementGranularity::kWord, fNavMode);
        nextPos = target.text_index().value;
    } else if (gran == MovementGranularity::kLine) {
        CaretPosition target = fDocument->spatial_index().moveCaret(fSelection.focus(),
                                                                    CursorDirection::kRight,
                                                                    MovementGranularity::kLine,
                                                                    NavigationMode::kTextLogical);
        nextPos = target.text_index().value;
    } else {
        const char* begin = text.data();
        const char* ptr = begin + cursor;
        const char* end = begin + text.size();
        SkUTF::NextUTF8(&ptr, end);
        nextPos = ptr - begin;
    }
    if (nextPos <= cursor) {
        return;
    }
    std::string textBefore = std::string(text.substr(cursor, nextPos - cursor));
    fDocument->erase(TextRange(TextIndex(cursor), TextIndex(nextPos)));
    updateCursorPosition(cursor);

    if (!fIsPerformingUndoRedo) {
        EditCommand cmd;
        cmd.kind = EditCommand::Kind::kDelete;
        cmd.position = TextIndex(cursor);
        cmd.textBefore = std::move(textBefore);
        cmd.textAfter = "";
        cmd.selectionBefore = selBefore;
        cmd.selectionAfter = fSelection;
        cmd.timestamp = std::chrono::steady_clock::now();
        pushEditCommand(std::move(cmd));
    }
    notifyRedraw();
}

void TextEditorViewModel::moveCaret(CursorDirection dir,
                                    MovementGranularity gran,
                                    NavigationMode mode,
                                    bool select) {
    CaretPosition current = fSelection.focus();
    if ((dir == CursorDirection::kUp || dir == CursorDirection::kDown) &&
        gran != MovementGranularity::kParagraph) {
        if (!fHasPreferredX) {
            fPreferredX = current.caret_rect().fLeft;
            fHasPreferredX = true;
        }
        SkRect rect = current.caret_rect();
        rect.fLeft = fPreferredX;
        rect.fRight = fPreferredX + 1.0f;
        current = CaretPosition(current.text_index(), current.affinity(), rect);
    } else {
        fHasPreferredX = false;
    }

    CaretPosition next = fDocument->spatial_index().moveCaret(current, dir, gran, mode);
    if (!select) {
        fSelection.collapse_to(next);
    } else {
        fSelection.set_span(fSelection.anchor(), next);
    }
    notifyRedraw();
}

void TextEditorViewModel::moveCaretToPoint(SkScalar screenX, SkScalar screenY, bool select) {
    fHasPreferredX = false;
    SkScalar docX = screenX + fScrollOffset.fX;
    SkScalar docY = screenY + fScrollOffset.fY;
    CaretPosition hit = fDocument->spatial_index().hitTest(docX, docY);
    if (!select) {
        fSelection.collapse_to(hit);
        fDragAnchorDocPoint = SkPoint::Make(docX, docY);
        fHasDragPoint = true;
    } else {
        if (!fHasDragPoint) {
            fDragAnchorDocPoint = SkPoint::Make(docX, docY);
            fHasDragPoint = true;
        }
        std::vector<SkRect> visualRects;
        std::vector<TextRange> visualRanges;
        fDocument->spatial_index().getSelectionForVisualDrag(fDragAnchorDocPoint.fX,
                                                             fDragAnchorDocPoint.fY,
                                                             docX,
                                                             docY,
                                                             visualRects,
                                                             visualRanges);
        fSelection.set_ranges(fSelection.anchor(), hit, std::move(visualRanges));
    }
    notifyRedraw();
}

void TextEditorViewModel::selectAll() {
    fHasPreferredX = false;
    std::string_view text = fDocument->text();
    if (text.empty()) {
        return;
    }
    CaretPosition hitStart = fDocument->spatial_index().hitTest(0.0f, 0.0f);
    CaretPosition start(TextIndex(0), Affinity::kDownstream, hitStart.caret_rect());

    CaretPosition end = resolveCaretPosition(*fDocument, text.size());
    fSelection.set_span(start, end);
    notifyRedraw();
}

void TextEditorViewModel::selectWordAtPoint(SkScalar screenX, SkScalar screenY) {
    fHasPreferredX = false;
    SkScalar docX = screenX + fScrollOffset.fX;
    SkScalar docY = screenY + fScrollOffset.fY;
    CaretPosition hit = fDocument->spatial_index().hitTest(docX, docY);
    TextRange wr = fDocument->spatial_index().getWordBoundary(hit.text_index());

    CaretPosition anchor = resolveCaretPosition(*fDocument, wr.start.value, Affinity::kDownstream);
    CaretPosition focus = resolveCaretPosition(*fDocument, wr.end.value, Affinity::kUpstream);

    fSelection.set_span(anchor, focus);
    notifyRedraw();
}

void TextEditorViewModel::selectLineAtPoint(SkScalar screenX, SkScalar screenY) {
    fHasPreferredX = false;
    SkScalar docX = screenX + fScrollOffset.fX;
    SkScalar docY = screenY + fScrollOffset.fY;
    CaretPosition hit = fDocument->spatial_index().hitTest(docX, docY);

    std::string_view docText = fDocument->text();
    if (docText.empty()) {
        return;
    }

    size_t idx = std::min(hit.text_index().value, docText.size());
    if (hit.affinity() == Affinity::kUpstream && idx > 0) {
        --idx;
    }

    size_t start = 0;
    if (idx > 0) {
        size_t prevNl = docText.rfind('\n', idx - 1);
        if (prevNl != std::string_view::npos) {
            start = prevNl + 1;
        }
    }
    size_t end = docText.size();
    size_t nextNl = docText.find('\n', idx);
    if (nextNl != std::string_view::npos) {
        end = nextNl + 1;
    }

    CaretPosition anchor = resolveCaretPosition(*fDocument, start, Affinity::kDownstream);
    CaretPosition focus = resolveCaretPosition(*fDocument, end, Affinity::kUpstream);
    fSelection.set_span(anchor, focus);
    notifyRedraw();
}

void TextEditorViewModel::handleMouseDown(SkScalar screenX,
                                          SkScalar screenY,
                                          skui::ModifierKey modifiers) {
    bool shift = (modifiers & skui::ModifierKey::kShift) != skui::ModifierKey::kNone;
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - fLastClickTime).count();
    SkScalar dx = screenX - fLastClickScreenPos.fX;
    SkScalar dy = screenY - fLastClickScreenPos.fY;
    SkScalar distSq = dx * dx + dy * dy;

    if (!shift && elapsedMs < 500 && distSq < 100.0f) {
        fClickCount = (fClickCount % 3) + 1;
    } else {
        fClickCount = 1;
    }
    fLastClickTime = now;
    fLastClickScreenPos = SkPoint::Make(screenX, screenY);
    fIsMouseDown = true;

    if (shift) {
        fDragMode = DragMode::kGrapheme;
        moveCaretToPoint(screenX, screenY, true);
        return;
    }

    if (fClickCount == 1) {
        fDragMode = DragMode::kGrapheme;
        moveCaretToPoint(screenX, screenY, false);
    } else if (fClickCount == 2) {
        fDragMode = DragMode::kWord;
        selectWordAtPoint(screenX, screenY);
        fInitialWordRange = fSelection.text_range();
    } else if (fClickCount == 3) {
        fDragMode = DragMode::kLine;
        selectLineAtPoint(screenX, screenY);
        fInitialLineRange = fSelection.text_range();
    }
}

void TextEditorViewModel::handleMouseMove(SkScalar screenX, SkScalar screenY) {
    if (!fIsMouseDown) {
        return;
    }

    if (fDragMode == DragMode::kGrapheme) {
        moveCaretToPoint(screenX, screenY, true);
        return;
    }

    SkScalar docX = screenX + fScrollOffset.fX;
    SkScalar docY = screenY + fScrollOffset.fY;
    CaretPosition hit = fDocument->spatial_index().hitTest(docX, docY);

    if (fDragMode == DragMode::kWord) {
        TextIndex lookupIdx = hit.text_index();
        if (hit.affinity() == Affinity::kUpstream && lookupIdx.value > 0) {
            lookupIdx = lookupIdx - 1;
        }
        TextRange targetWord = fDocument->spatial_index().getWordBoundary(lookupIdx);

        if (targetWord.end > fInitialWordRange.end) {
            CaretPosition anchor = resolveCaretPosition(
                    *fDocument, fInitialWordRange.start.value, Affinity::kDownstream);
            CaretPosition focus =
                    resolveCaretPosition(*fDocument, targetWord.end.value, Affinity::kUpstream);
            fSelection.set_span(anchor, focus);
        } else if (targetWord.start < fInitialWordRange.start) {
            CaretPosition anchor = resolveCaretPosition(
                    *fDocument, fInitialWordRange.end.value, Affinity::kUpstream);
            CaretPosition focus =
                    resolveCaretPosition(*fDocument, targetWord.start.value, Affinity::kDownstream);
            fSelection.set_span(anchor, focus);
        } else {
            CaretPosition anchor = resolveCaretPosition(
                    *fDocument, fInitialWordRange.start.value, Affinity::kDownstream);
            CaretPosition focus = resolveCaretPosition(
                    *fDocument, fInitialWordRange.end.value, Affinity::kUpstream);
            fSelection.set_span(anchor, focus);
        }
        notifyRedraw();
    } else if (fDragMode == DragMode::kLine) {
        std::string_view docText = fDocument->text();
        size_t idx = std::min(hit.text_index().value, docText.size());
        if (hit.affinity() == Affinity::kUpstream && idx > 0) {
            --idx;
        }
        size_t start = 0;
        if (idx > 0) {
            size_t prevNl = docText.rfind('\n', idx - 1);
            if (prevNl != std::string_view::npos) {
                start = prevNl + 1;
            }
        }
        size_t end = docText.size();
        size_t nextNl = docText.find('\n', idx);
        if (nextNl != std::string_view::npos) {
            end = nextNl + 1;
        }

        if (end > fInitialLineRange.end.value) {
            CaretPosition anchor = resolveCaretPosition(
                    *fDocument, fInitialLineRange.start.value, Affinity::kDownstream);
            CaretPosition focus = resolveCaretPosition(*fDocument, end, Affinity::kUpstream);
            fSelection.set_span(anchor, focus);
        } else if (start < fInitialLineRange.start.value) {
            CaretPosition anchor = resolveCaretPosition(
                    *fDocument, fInitialLineRange.end.value, Affinity::kUpstream);
            CaretPosition focus = resolveCaretPosition(*fDocument, start, Affinity::kDownstream);
            fSelection.set_span(anchor, focus);
        } else {
            CaretPosition anchor = resolveCaretPosition(
                    *fDocument, fInitialLineRange.start.value, Affinity::kDownstream);
            CaretPosition focus = resolveCaretPosition(
                    *fDocument, fInitialLineRange.end.value, Affinity::kUpstream);
            fSelection.set_span(anchor, focus);
        }
        notifyRedraw();
    }
}

void TextEditorViewModel::handleMouseUp(SkScalar screenX, SkScalar screenY) {
    fIsMouseDown = false;
}

bool TextEditorViewModel::findNext(std::string_view query, bool matchCase) {
    if (query.empty() || fDocument->text().empty()) {
        return false;
    }
    std::string_view docText = fDocument->text();
    size_t startPos = fSelection.is_collapsed() ? fSelection.focus().text_index().value
                                                : fSelection.text_range().end.value;
    size_t matchPos = FindForward(docText, query, startPos, matchCase);
    if (matchPos == std::string_view::npos) {
        return false;
    }

    CaretPosition anchor = resolveCaretPosition(*fDocument, matchPos, Affinity::kDownstream);
    CaretPosition focus =
            resolveCaretPosition(*fDocument, matchPos + query.size(), Affinity::kUpstream);
    fSelection.set_span(anchor, focus);
    notifyRedraw();
    return true;
}

bool TextEditorViewModel::findPrevious(std::string_view query, bool matchCase) {
    if (query.empty() || fDocument->text().empty()) {
        return false;
    }
    std::string_view docText = fDocument->text();
    size_t startPos = fSelection.is_collapsed() ? fSelection.focus().text_index().value
                                                : fSelection.text_range().start.value;
    if (startPos > 0) {
        --startPos;
    } else {
        startPos = docText.size();
    }

    size_t matchPos = FindBackward(docText, query, startPos, matchCase);
    if (matchPos == std::string_view::npos) {
        return false;
    }

    CaretPosition anchor = resolveCaretPosition(*fDocument, matchPos, Affinity::kDownstream);
    CaretPosition focus =
            resolveCaretPosition(*fDocument, matchPos + query.size(), Affinity::kUpstream);
    fSelection.set_span(anchor, focus);
    notifyRedraw();
    return true;
}

bool TextEditorViewModel::replaceCurrent(std::string_view replacement) {
    if (fSelection.is_collapsed()) {
        return false;
    }

    TextRange range = fSelection.text_range();
    size_t start = range.start.value;
    std::string textBefore(fDocument->text().substr(start, range.length()));
    EditorSelection selBefore = fSelection;

    fDocument->replace(range, replacement);
    updateCursorPosition(start + replacement.size());

    if (!fIsPerformingUndoRedo) {
        EditCommand cmd;
        cmd.kind = EditCommand::Kind::kCutOrBlock;
        cmd.position = TextIndex(start);
        cmd.textBefore = std::move(textBefore);
        cmd.textAfter = std::string(replacement);
        cmd.selectionBefore = selBefore;
        cmd.selectionAfter = fSelection;
        cmd.timestamp = std::chrono::steady_clock::now();
        pushEditCommand(std::move(cmd));
    }
    notifyRedraw();
    return true;
}

size_t TextEditorViewModel::replaceAll(std::string_view query,
                                       std::string_view replacement,
                                       bool matchCase) {
    if (query.empty() || fDocument->text().empty()) {
        return 0;
    }

    std::string_view docText = fDocument->text();
    std::vector<size_t> matches;
    if (docText.size() >= query.size()) {
        size_t limit = docText.size() - query.size();
        for (size_t i = 0; i <= limit;) {
            if (MatchesAt(docText, i, query, matchCase)) {
                matches.push_back(i);
                i += query.size();
            } else {
                ++i;
            }
        }
    }

    if (matches.empty()) {
        return 0;
    }

    EditorSelection selBefore = fSelection;
    std::string textBefore(docText);

    for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
        fDocument->replace(TextRange(TextIndex(*it), TextIndex(*it + query.size())), replacement);
    }

    updateCursorPosition(std::min(selBefore.focus().text_index().value, fDocument->text().size()));

    if (!fIsPerformingUndoRedo) {
        EditCommand cmd;
        cmd.kind = EditCommand::Kind::kCutOrBlock;
        cmd.position = TextIndex(0);
        cmd.textBefore = std::move(textBefore);
        cmd.textAfter = std::string(fDocument->text());
        cmd.selectionBefore = selBefore;
        cmd.selectionAfter = fSelection;
        cmd.timestamp = std::chrono::steady_clock::now();
        pushEditCommand(std::move(cmd));
    }

    notifyRedraw();
    return matches.size();
}

void TextEditorViewModel::setSelection(CaretPosition anchor, CaretPosition focus) {
    fHasPreferredX = false;
    fSelection.set_span(anchor, focus);
    notifyRedraw();
}

void TextEditorViewModel::collapseTo(CaretPosition pos) {
    fHasPreferredX = false;
    fSelection.collapse_to(pos);
    notifyRedraw();
}

void TextEditorViewModel::ensureCaretVisible(const SkRect& viewportBounds) {
    const SkRect docCaret = fSelection.focus().caret_rect();
    if (docCaret.isEmpty() || viewportBounds.isEmpty()) {
        return;
    }

    SkRect screenCaret = docCaret;
    screenCaret.offset(-fScrollOffset.fX, -fScrollOffset.fY);

    if (screenCaret.fLeft < viewportBounds.fLeft) {
        fScrollOffset.fX = docCaret.fLeft - viewportBounds.fLeft;
    } else if (screenCaret.fRight > viewportBounds.fRight) {
        fScrollOffset.fX = docCaret.fRight - viewportBounds.fRight;
    }

    if (screenCaret.fTop < viewportBounds.fTop) {
        fScrollOffset.fY = docCaret.fTop - viewportBounds.fTop;
    } else if (screenCaret.fBottom > viewportBounds.fBottom) {
        fScrollOffset.fY = docCaret.fBottom - viewportBounds.fBottom;
    }
    notifyRedraw();
}

bool TextEditorViewModel::handleKey(skui::Key key,
                                    skui::InputState state,
                                    skui::ModifierKey modifiers) {
    if (key == skui::Key::kCtrl) {
        fCtrlKeyHeld = (state == skui::InputState::kDown);
        return true;
    }

    if (state != skui::InputState::kDown) {
        return false;
    }

    bool shift = (modifiers & skui::ModifierKey::kShift) != skui::ModifierKey::kNone;
    bool ctrl =
            ((modifiers & skui::ModifierKey::kControl) != skui::ModifierKey::kNone) || fCtrlKeyHeld;
    bool cmd = (modifiers & skui::ModifierKey::kCommand) != skui::ModifierKey::kNone;
    bool alt = (modifiers & skui::ModifierKey::kOption) != skui::ModifierKey::kNone;
    bool ctrlOrCmd = ctrl || cmd;
    bool wordMod = alt || ctrl;

    switch (key) {
        case skui::Key::kLeft: {
            MovementGranularity gran = MovementGranularity::kGrapheme;
            if (cmd) {
                gran = MovementGranularity::kLine;
            } else if (wordMod) {
                gran = MovementGranularity::kWord;
            }
            moveCaret(CursorDirection::kLeft, gran, fNavMode, shift);
            return true;
        }
        case skui::Key::kRight: {
            MovementGranularity gran = MovementGranularity::kGrapheme;
            if (cmd) {
                gran = MovementGranularity::kLine;
            } else if (wordMod) {
                gran = MovementGranularity::kWord;
            }
            moveCaret(CursorDirection::kRight, gran, fNavMode, shift);
            return true;
        }
        case skui::Key::kHome: {
            MovementGranularity gran =
                    ctrlOrCmd ? MovementGranularity::kParagraph : MovementGranularity::kLine;
            moveCaret(CursorDirection::kLeft, gran, fNavMode, shift);
            return true;
        }
        case skui::Key::kEnd: {
            MovementGranularity gran =
                    ctrlOrCmd ? MovementGranularity::kParagraph : MovementGranularity::kLine;
            moveCaret(CursorDirection::kRight, gran, fNavMode, shift);
            return true;
        }
        case skui::Key::kUp: {
            MovementGranularity gran =
                    cmd ? MovementGranularity::kParagraph : MovementGranularity::kLine;
            NavigationMode mode = cmd ? fNavMode : NavigationMode::kScreenPhysical;
            moveCaret(CursorDirection::kUp, gran, mode, shift);
            return true;
        }
        case skui::Key::kDown: {
            MovementGranularity gran =
                    cmd ? MovementGranularity::kParagraph : MovementGranularity::kLine;
            NavigationMode mode = cmd ? fNavMode : NavigationMode::kScreenPhysical;
            moveCaret(CursorDirection::kDown, gran, mode, shift);
            return true;
        }
        case skui::Key::kBack: {
            MovementGranularity gran = MovementGranularity::kGrapheme;
            if (cmd) {
                gran = MovementGranularity::kLine;
            } else if (wordMod) {
                gran = MovementGranularity::kWord;
            }
            deleteBackward(gran);
            return true;
        }
        case skui::Key::kDelete: {
            MovementGranularity gran = MovementGranularity::kGrapheme;
            if (cmd) {
                gran = MovementGranularity::kLine;
            } else if (wordMod) {
                gran = MovementGranularity::kWord;
            }
            deleteForward(gran);
            return true;
        }
        case skui::Key::kA:
            if (ctrlOrCmd) {
                selectAll();
                return true;
            }
            break;
        case skui::Key::kC:
            if (ctrlOrCmd) {
                if (fClipboardSetter) {
                    fClipboardSetter(copySelection());
                }
                return true;
            }
            break;
        case skui::Key::kX:
            if (ctrlOrCmd) {
                cutSelection();
                return true;
            }
            break;
        case skui::Key::kV:
            if (ctrlOrCmd) {
                if (fClipboardGetter) {
                    pasteText(fClipboardGetter());
                }
                return true;
            }
            break;
        case skui::Key::kZ:
            if (ctrlOrCmd) {
                if (shift) {
                    redo();
                } else {
                    undo();
                }
                return true;
            }
            break;
        case skui::Key::kY:
            if (ctrlOrCmd) {
                redo();
                return true;
            }
            break;
        case skui::Key::kOK:
            insertText("\n");
            return true;
        case skui::Key::kTab: {
            // Immediate Soft-Tab Normalization:
            // Calculate column offset from start of line
            std::string_view fullText = fDocument->text();
            size_t cursor = std::min(fSelection.focus().text_index().value, fullText.size());
            size_t lineStart = 0;
            if (cursor > 0) {
                size_t lastNewline = fullText.rfind('\n', cursor - 1);
                if (lastNewline != std::string_view::npos) {
                    lineStart = lastNewline + 1;
                }
            }
            // Count codepoints from lineStart to cursor
            size_t col = 0;
            const char* p = fullText.data() + lineStart;
            const char* end = fullText.data() + cursor;
            while (p < end) {
                SkUTF::NextUTF8(&p, end);
                col++;
            }
            constexpr size_t kTabSize = 4;
            size_t spacesNeeded = kTabSize - (col % kTabSize);
            std::string spaces(spacesNeeded, ' ');
            insertText(spaces);
            return true;
        }
        default:
            break;
    }
    return false;
}

bool TextEditorViewModel::handleChar(SkUnichar c, skui::ModifierKey modifiers) {
    bool ctrlOrCmd = ((modifiers & (skui::ModifierKey::kControl | skui::ModifierKey::kCommand)) !=
                      skui::ModifierKey::kNone) ||
                     fCtrlKeyHeld;

    // Invariant: Never insert text or control characters when Control/Command is held,
    // when raw ASCII control characters (c < 32 except \n, \t) or DEL (127) arrive,
    // or when macOS function key PUA codepoints (0xF700 - 0xF8FF) arrive
    if (ctrlOrCmd || (c < 32 && c != '\n' && c != '\t') || c == 0x7F ||
        (c >= 0xF700 && c <= 0xF8FF)) {
        return false;
    }

    char utf8Buffer[4];
    size_t len = SkUTF::ToUTF8(c, utf8Buffer);
    if (len > 0) {
        insertText(std::string_view(utf8Buffer, len));
        return true;
    }
    return false;
}

SkRect TextEditorViewModel::screenCaretRect() const {
    SkRect r = fSelection.focus().caret_rect();
    if (r.isEmpty() || r.height() <= 0) {
        const auto& lines = fDocument->formatted().lines();
        if (!lines.empty()) {
            const auto& firstLine = lines[0];
            r = SkRect::MakeXYWH(firstLine.bounds.fLeft,
                                 firstLine.baseline + firstLine.ascent,
                                 1.0f,
                                 std::abs(firstLine.ascent) + std::abs(firstLine.descent));
        } else {
            SkFont font = !fDocument->styles().empty() ? fDocument->styles()[0].font : SkFont();
            SkFontMetrics metrics;
            font.getMetrics(&metrics);
            SkScalar h = std::abs(metrics.fAscent) + std::abs(metrics.fDescent);
            if (h <= 0) {
                h = 16.0f;
            }
            r = SkRect::MakeXYWH(0, metrics.fAscent < 0 ? metrics.fAscent : 0, 1.0f, h);
        }
    }
    r.offset(-fScrollOffset.fX, -fScrollOffset.fY);
    return r;
}

std::vector<SkRect> TextEditorViewModel::screenSelectionRects() const {
    std::vector<SkRect> rects;
    if (!fSelection.is_collapsed()) {
        if (!fSelection.ranges().empty()) {
            for (const auto& r : fSelection.ranges()) {
                std::vector<SkRect> subRects;
                fDocument->spatial_index().getSelectionRects(r, subRects);
                rects.insert(rects.end(), subRects.begin(), subRects.end());
            }
        } else {
            fDocument->spatial_index().getSelectionRects(fSelection.text_range(), rects);
        }
        for (auto& r : rects) {
            r.offset(-fScrollOffset.fX, -fScrollOffset.fY);
        }
    }
    return rects;
}

void TextEditorViewModel::visitScreenRuns(const SkRect& screenClip,
                                          RenderRunVisitor visitor) const {
    SkRect docClip = screenClip;
    docClip.offset(fScrollOffset.fX, fScrollOffset.fY);
    fDocument->visitDocumentRuns(docClip, std::move(visitor));
}

void TextEditorViewModel::updateCursorPosition(size_t index, Affinity affinity) {
    fHasPreferredX = false;
    CaretPosition pos = resolveCaretPosition(*fDocument, index, affinity);
    fSelection.collapse_to(pos);
}

void TextEditorViewModel::setClipboardHandlers(ClipboardSetter setter, ClipboardGetter getter) {
    fClipboardSetter = std::move(setter);
    fClipboardGetter = std::move(getter);
}

std::string TextEditorViewModel::copySelection() const {
    if (fSelection.is_collapsed()) {
        return "";
    }
    std::string_view docText = fDocument->text();
    if (!fSelection.ranges().empty()) {
        std::string result;
        for (const auto& r : fSelection.ranges()) {
            size_t start = std::min(r.start.value, docText.size());
            size_t end = std::min(r.end.value, docText.size());
            if (start > end) {
                std::swap(start, end);
            }
            result.append(docText.substr(start, end - start));
        }
        return result;
    }
    TextRange range = fSelection.text_range();
    size_t start = std::min(range.start.value, docText.size());
    size_t end = std::min(range.end.value, docText.size());
    if (start > end) {
        std::swap(start, end);
    }
    return std::string(docText.substr(start, end - start));
}

void TextEditorViewModel::cutSelection() {
    if (fSelection.is_collapsed()) {
        return;
    }
    std::string copied = copySelection();
    if (fClipboardSetter) {
        fClipboardSetter(copied);
    }
    deleteBackward();
}

void TextEditorViewModel::pasteText(std::string_view raw) {
    if (raw.empty()) {
        return;
    }
    // Smart Duplicate Invariant:
    // If the active selection is non-collapsed and its content is identical to the pasted payload,
    // collapse the selection to its end (right edge) and insert the text adjacent to it.
    // This allows immediate duplication via Ctrl+C -> Ctrl+V without requiring manual
    // cursor movement
    if (!fSelection.is_collapsed() && raw == copySelection()) {
        size_t rightEdge = 0;
        if (!fSelection.ranges().empty()) {
            for (const auto& r : fSelection.ranges()) {
                rightEdge = std::max({rightEdge, r.start.value, r.end.value});
            }
        } else {
            TextRange r = fSelection.text_range();
            rightEdge = std::max(r.start.value, r.end.value);
        }
        updateCursorPosition(rightEdge);
        collapseTo(fSelection.focus());
    }
    insertText(raw);
}

bool TextEditorViewModel::canUndo() const { return !fUndoStack.empty(); }

bool TextEditorViewModel::canRedo() const { return !fRedoStack.empty(); }

bool TextEditorViewModel::undo() {
    if (fUndoStack.empty()) {
        return false;
    }

    EditCommand cmd = std::move(fUndoStack.back());
    fUndoStack.pop_back();

    fIsPerformingUndoRedo = true;

    // Apply inverse mutation:
    // 1. If text was inserted, erase it
    if (!cmd.textAfter.empty()) {
        fDocument->erase(TextRange(cmd.position, cmd.position + cmd.textAfter.size()));
    }
    // 2. If text was deleted, re-insert it
    if (!cmd.textBefore.empty()) {
        fDocument->insert(cmd.position, cmd.textBefore);
    }

    // 3. Restore selection state
    if (cmd.selectionBefore.is_collapsed()) {
        updateCursorPosition(cmd.selectionBefore.focus().text_index().value);
    } else {
        fSelection = cmd.selectionBefore;
    }

    fIsPerformingUndoRedo = false;
    fRedoStack.push_back(std::move(cmd));

    notifyRedraw();
    return true;
}

bool TextEditorViewModel::redo() {
    if (fRedoStack.empty()) {
        return false;
    }

    EditCommand cmd = std::move(fRedoStack.back());
    fRedoStack.pop_back();

    fIsPerformingUndoRedo = true;

    // Apply forward mutation:
    if (!cmd.textBefore.empty()) {
        fDocument->erase(TextRange(cmd.position, cmd.position + cmd.textBefore.size()));
    }
    if (!cmd.textAfter.empty()) {
        fDocument->insert(cmd.position, cmd.textAfter);
    }

    if (cmd.selectionAfter.is_collapsed()) {
        updateCursorPosition(cmd.selectionAfter.focus().text_index().value);
    } else {
        fSelection = cmd.selectionAfter;
    }

    fIsPerformingUndoRedo = false;
    fUndoStack.push_back(std::move(cmd));

    notifyRedraw();
    return true;
}

void TextEditorViewModel::clearHistory() {
    fUndoStack.clear();
    fRedoStack.clear();
}

void TextEditorViewModel::pushEditCommand(EditCommand cmd) {
    if (fIsPerformingUndoRedo) {
        return;
    }

    // Any new mutation invalidates the entire Redo stack
    fRedoStack.clear();

    auto now = cmd.timestamp;
    bool coalesced = false;

    if (!fUndoStack.empty()) {
        auto& last = fUndoStack.back();
        auto dt =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last.timestamp).count();

        // 1. Typing coalescing
        if (last.kind == EditCommand::Kind::kTyping && cmd.kind == EditCommand::Kind::kTyping) {
            bool isBoundary = (cmd.textAfter == " " || cmd.textAfter == "\t" ||
                               std::ispunct(static_cast<unsigned char>(cmd.textAfter[0])));
            bool lastWasBoundary = (last.textAfter == " " || last.textAfter == "\t" ||
                                    (last.textAfter.size() == 1 &&
                                     std::ispunct(static_cast<unsigned char>(last.textAfter[0]))));

            if (dt <= 750 && cmd.position.value == (last.position.value + last.textAfter.size()) &&
                !isBoundary && !lastWasBoundary) {
                last.textAfter.append(cmd.textAfter);
                last.selectionAfter = cmd.selectionAfter;
                last.timestamp = now;
                coalesced = true;
            }
        }
        // 2. Backward deletion coalescing
        else if (last.kind == EditCommand::Kind::kDelete &&
                 cmd.kind == EditCommand::Kind::kDelete) {
            if (dt <= 750 && (cmd.position.value + cmd.textBefore.size() == last.position.value)) {
                last.textBefore = cmd.textBefore + last.textBefore;
                last.position = cmd.position;
                last.selectionAfter = cmd.selectionAfter;
                last.timestamp = now;
                coalesced = true;
            }
        }
    }

    if (!coalesced) {
        fUndoStack.push_back(std::move(cmd));
        if (fUndoStack.size() > kMaxUndoDepth) {
            fUndoStack.pop_front();
        }
    }
}

}  // namespace skia::text_editor
