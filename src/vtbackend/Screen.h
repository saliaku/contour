/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtbackend/Capabilities.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/Charset.h>
#include <vtbackend/Color.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/ScreenEvents.h>
#include <vtbackend/TerminalState.h>
#include <vtbackend/VTType.h>
#include <vtbackend/cell/CellConcept.h>

#include <vtparser/ParserExtension.h>

#include <crispy/StrongLRUCache.h>
#include <crispy/algorithm.h>
#include <crispy/logstore.h>
#include <crispy/size.h>
#include <crispy/utils.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/width.h>

#include <fmt/format.h>

#include <algorithm>
#include <bitset>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

namespace terminal
{

class ScreenBase: public SequenceHandler
{
  public:
    virtual void verifyState() const = 0;
    virtual void fail(std::string const& _message) const = 0;
    [[nodiscard]] virtual Margin margin() const noexcept = 0;
    [[nodiscard]] virtual Margin& margin() noexcept = 0;
    [[nodiscard]] virtual bool contains(CellLocation _coord) const noexcept = 0;
    [[nodiscard]] virtual bool isCellEmpty(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual bool compareCellTextAt(CellLocation position, char codepoint) const noexcept = 0;
    [[nodiscard]] virtual std::string cellTextAt(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual std::string lineTextAt(LineOffset line) const noexcept = 0;
    [[nodiscard]] virtual bool isLineEmpty(LineOffset line) const noexcept = 0;
    [[nodiscard]] virtual uint8_t cellWidthAt(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual LineCount historyLineCount() const noexcept = 0;
    [[nodiscard]] virtual HyperlinkId hyperlinkIdAt(CellLocation position) const noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<HyperlinkInfo const> hyperlinkAt(
        CellLocation pos) const noexcept = 0;
    virtual void inspect(std::string const& _message, std::ostream& _os) const = 0;
    virtual void moveCursorTo(LineOffset line, ColumnOffset column) = 0; // CUP
    virtual void updateCursorIterator() noexcept = 0;

    [[nodiscard]] virtual std::optional<CellLocation> search(std::u32string_view searchText,
                                                             CellLocation startPosition) = 0;
    [[nodiscard]] virtual std::optional<CellLocation> searchReverse(std::u32string_view searchText,
                                                                    CellLocation startPosition) = 0;
};

/**
 * Terminal Screen.
 *
 * Implements the all VT command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
class Screen final: public ScreenBase, public capabilities::StaticDatabase
{
  public:
    /// @param terminal            reference to the terminal this display belongs to.
    /// @param pageSize            page size of this display. This is passed because it does not necessarily
    ///                            need to match the terminal's main display page size.
    /// @param reflowOnResize      whether or not to perform virtual line text reflow on resuze.
    /// @param maxHistoryLineCount maximum number of lines that are can be scrolled back to via Viewport.
    Screen(Terminal& terminal,
           PageSize pageSize,
           bool reflowOnResize,
           MaxHistoryLineCount maxHistoryLineCount);

    Screen(Screen const&) = delete;
    Screen& operator=(Screen const&) = delete;
    Screen(Screen&&) noexcept = default;
    Screen& operator=(Screen&&) noexcept = default;
    ~Screen() override = default;

    using StaticDatabase::numericCapability;
    [[nodiscard]] unsigned numericCapability(capabilities::Code _cap) const override;

    // {{{ SequenceHandler overrides
    void writeText(char32_t _char) override;
    void writeText(std::string_view _chars, size_t cellCount) override;
    void executeControlCode(char controlCode) override;
    void processSequence(Sequence const& seq) override;
    // }}}

    void writeTextFromExternal(std::string_view _chars);

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename Renderer>
    RenderPassHints render(Renderer&& _render,
                           ScrollOffset _scrollOffset = {},
                           HighlightSearchMatches highlightSearchMatches = HighlightSearchMatches::Yes) const
    {
        return _grid.render(std::forward<Renderer>(_render), _scrollOffset, highlightSearchMatches);
    }

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    [[nodiscard]] std::string renderMainPageText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    std::string screenshot(std::function<std::string(LineOffset)> const& _postLine = {}) const;

    void crlf() { linefeed(margin().horizontal.from); }
    void crlfIfWrapPending();

    // {{{ VT API
    void linefeed(); // LF

    void clearToBeginOfLine();
    void clearToEndOfLine();
    void clearLine();

    void clearToBeginOfScreen();
    void clearToEndOfScreen();
    void clearScreen();

    // DECSEL
    void selectiveEraseToBeginOfLine();
    void selectiveEraseToEndOfLine();
    void selectiveEraseLine(LineOffset line);

    // DECSED
    void selectiveEraseToBeginOfScreen();
    void selectiveEraseToEndOfScreen();
    void selectiveEraseScreen();

    void selectiveEraseArea(Rect area);

    void selectiveErase(LineOffset line, ColumnOffset begin, ColumnOffset end);
    [[nodiscard]] bool containsProtectedCharacters(LineOffset line,
                                                   ColumnOffset begin,
                                                   ColumnOffset end) const;

    void eraseCharacters(ColumnCount _n);  // ECH
    void insertCharacters(ColumnCount _n); // ICH
    void deleteCharacters(ColumnCount _n); // DCH
    void deleteColumns(ColumnCount _n);    // DECDC
    void insertLines(LineCount _n);        // IL
    void insertColumns(ColumnCount _n);    // DECIC

    void copyArea(Rect sourceArea, int page, CellLocation targetTopLeft, int targetPage);

    void eraseArea(int _top, int _left, int _bottom, int _right);

    void fillArea(char32_t _ch, int _top, int _left, int _bottom, int _right);

    void deleteLines(LineCount _n); // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorTo(LineOffset line, ColumnOffset column) override; // CUP
    void moveCursorBackward(ColumnCount _n);                          // CUB
    void moveCursorDown(LineCount _n);                                // CUD
    void moveCursorForward(ColumnCount _n);                           // CUF
    void moveCursorToBeginOfLine();                                   // CR
    void moveCursorToColumn(ColumnOffset _n);                         // CHA
    void moveCursorToLine(LineOffset _n);                             // VPA
    void moveCursorToNextLine(LineCount _n);                          // CNL
    void moveCursorToNextTab();                                       // HT
    void moveCursorToPrevLine(LineCount _n);                          // CPL
    void moveCursorUp(LineCount _n);                                  // CUU

    void cursorBackwardTab(TabStopCount _n);            // CBT
    void cursorForwardTab(TabStopCount _n);             // CHT
    void backspace();                                   // BS
    void horizontalTabClear(HorizontalTabClear _which); // TBC
    void horizontalTabSet();                            // HTS

    void index();        // IND
    void reverseIndex(); // RI

    void setMark();
    void deviceStatusReport();           // DSR
    void reportCursorPosition();         // CPR
    void reportExtendedCursorPosition(); // DECXCPR
    void selectConformanceLevel(VTType _level);
    void requestDynamicColor(DynamicColorName _name);
    void requestCapability(capabilities::Code _code);
    void requestCapability(std::string_view _name);
    void sendDeviceAttributes();
    void sendTerminalId();

    /// Sets the current working directory as file:// URL.
    void setCurrentWorkingDirectory(std::string const& _url); // OSC 7

    void hyperlink(std::string _id, std::string _uri);                   // OSC 8
    void notify(std::string const& _title, std::string const& _content); // OSC 777

    void captureBuffer(LineCount _lineCount, bool _logicalLines);

    void setForegroundColor(Color _color);
    void setBackgroundColor(Color _color);
    void setUnderlineColor(Color _color);
    void setCursorStyle(CursorDisplay _display, CursorShape _shape);
    void setGraphicsRendition(GraphicsRendition _rendition);
    void screenAlignmentPattern();
    void applicationKeypadMode(bool _enable);
    void designateCharset(CharsetTable _table, CharsetId _charset);
    void singleShiftSelect(CharsetTable _table);
    void requestPixelSize(RequestPixelSize _area);
    void requestCharacterSize(RequestPixelSize _area);
    void sixelImage(ImageSize _pixelSize, Image::Data&& _rgba);
    void requestStatusString(RequestStatusString _value);
    void requestTabStops();
    void resetDynamicColor(DynamicColorName _name);
    void setDynamicColor(DynamicColorName _name, RGBColor _color);
    void inspect();
    void smGraphics(XtSmGraphics::Item _item, XtSmGraphics::Action _action, XtSmGraphics::Value _value);
    // }}}

    std::shared_ptr<Image const> uploadImage(ImageFormat _format,
                                             ImageSize _imageSize,
                                             Image::Data&& _pixmap);

    /**
     * Renders an image onto the screen.
     *
     * @p _imageId ID to the image to be rendered.
     * @p _topLeft Screen coordinate to start rendering the top/left corner of the image.
     * @p _gridSize Screen grid size to span the image into.
     * @p _imageOffset Offset into the image in screen grid coordinate to start rendering from.
     * @p _imageSize Size of the full image in Screen grid coordinates.
     * @p _alignmentPolicy render the image using the given image alignment policy.
     * @p _resizePolicy render the image using the given image resize policy.
     * @p _autoScroll Boolean indicating whether or not the screen should scroll if the image cannot be fully
     * displayed otherwise.
     */
    void renderImage(std::shared_ptr<Image const> _image,
                     CellLocation _topLeft,
                     GridSize _gridSize,
                     PixelCoordinate _imageOffset,
                     ImageSize _imageSize,
                     ImageAlignment _alignmentPolicy,
                     ImageResize _resizePolicy,
                     bool _autoScroll);

    void inspect(std::string const& _message, std::ostream& _os) const override;

    // for DECSC and DECRC
    void saveModes(std::vector<DECMode> const& _modes);
    void restoreModes(std::vector<DECMode> const& _modes);
    void requestAnsiMode(unsigned int _mode);
    void requestDECMode(unsigned int _mode);

    [[nodiscard]] PageSize pageSize() const noexcept { return _grid.pageSize(); }
    [[nodiscard]] ImageSize pixelSize() const noexcept { return _state.cellPixelSize * _settings.pageSize; }

    [[nodiscard]] Margin margin() const noexcept override { return _grid.margin(); }
    [[nodiscard]] Margin& margin() noexcept override { return _grid.margin(); }

    [[nodiscard]] bool isFullHorizontalMargins() const noexcept
    {
        return margin().horizontal.to.value + 1 == pageSize().columns.value;
    }

    [[nodiscard]] bool isCursorInsideMargins() const noexcept;

    constexpr CellLocation realCursorPosition() const noexcept { return _state.cursor.position; }

    constexpr CellLocation logicalCursorPosition() const noexcept
    {
        if (!_state.cursor.originMode)
            return realCursorPosition();
        else
            return CellLocation { _state.cursor.position.line - margin().vertical.from,
                                  _state.cursor.position.column - margin().horizontal.from };
    }

    constexpr CellLocation origin() const noexcept
    {
        if (!_state.cursor.originMode)
            return {};

        return { margin().vertical.from, margin().horizontal.from };
    }

    [[nodiscard]] Cursor const& cursor() const noexcept { return _state.cursor; }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is
    /// enabled.
    CellLocation toRealCoordinate(CellLocation pos) const noexcept
    {
        if (!_state.cursor.originMode)
            return pos;
        else
            return { pos.line + margin().vertical.from, pos.column + margin().horizontal.from };
    }

    [[nodiscard]] LineOffset applyOriginMode(LineOffset line) const noexcept
    {
        if (!_state.cursor.originMode)
            return line;
        else
            return line + margin().vertical.from;
    }

    [[nodiscard]] ColumnOffset applyOriginMode(ColumnOffset column) const noexcept
    {
        if (!_state.cursor.originMode)
            return column;
        else
            return column + margin().horizontal.from;
    }

    [[nodiscard]] Rect applyOriginMode(Rect area) const noexcept
    {
        if (!_state.cursor.originMode)
            return area;

        auto const top = Top::cast_from(area.top.value + margin().vertical.from.value);
        auto const left = Left::cast_from(area.top.value + margin().horizontal.from.value);
        auto const bottom = Bottom::cast_from(area.bottom.value + margin().vertical.from.value);
        auto const right = Right::cast_from(area.right.value + margin().horizontal.from.value);
        // TODO: Should this automatically clamp to margin's botom/right values?

        return Rect { top, left, bottom, right };
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    CellLocation clampCoordinate(CellLocation coord) const noexcept
    {
        if (_state.cursor.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    CellLocation clampToOrigin(CellLocation coord) const noexcept
    {
        return { std::clamp(coord.line, LineOffset { 0 }, margin().vertical.to),
                 std::clamp(coord.column, ColumnOffset { 0 }, margin().horizontal.to) };
    }

    [[nodiscard]] LineOffset clampedLine(LineOffset _line) const noexcept
    {
        return std::clamp(_line, LineOffset(0), boxed_cast<LineOffset>(_grid.pageSize().lines) - 1);
    }

    [[nodiscard]] ColumnOffset clampedColumn(ColumnOffset _column) const noexcept
    {
        return std::clamp(_column, ColumnOffset(0), boxed_cast<ColumnOffset>(_grid.pageSize().columns) - 1);
    }

    CellLocation clampToScreen(CellLocation coord) const noexcept
    {
        return { clampedLine(coord.line), clampedColumn(coord.column) };
    }

    // Tests if given coordinate is within the visible screen area.
    [[nodiscard]] bool contains(CellLocation _coord) const noexcept override
    {
        return LineOffset(0) <= _coord.line && _coord.line < boxed_cast<LineOffset>(_settings.pageSize.lines)
               && ColumnOffset(0) <= _coord.column
               && _coord.column <= boxed_cast<ColumnOffset>(_settings.pageSize.columns);
    }

    [[nodiscard]] std::optional<CellLocation> search(std::u32string_view searchText,
                                                     CellLocation startPosition) override;
    [[nodiscard]] std::optional<CellLocation> searchReverse(std::u32string_view searchText,
                                                            CellLocation startPosition) override;

    Cell& usePreviousCell() noexcept
    {
        return useCellAt(_state.lastCursorPosition.line, _state.lastCursorPosition.column);
    }

    void updateCursorIterator() noexcept override
    {
#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
        _currentLine = &_grid.lineAt(_state.cursor.position.line);
#endif
    }

    Line<Cell>& currentLine() noexcept
    {
#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
        return *_currentLine;
#else
        return _grid.lineAt(_state.cursor.position.line);
#endif
    }

    Line<Cell> const& currentLine() const noexcept
    {
#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
        return *_currentLine;
#else
        return _grid.lineAt(_state.cursor.position.line);
#endif
    }

    Cell& useCurrentCell() noexcept { return currentLine().useCellAt(_state.cursor.position.column); }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell& at(LineOffset _line, ColumnOffset _column) noexcept { return _grid.useCellAt(_line, _column); }
    Cell& useCellAt(LineOffset _line, ColumnOffset _column) noexcept
    {
        return _grid.lineAt(_line).useCellAt(_column);
    }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell const& at(LineOffset _line, ColumnOffset _column) const noexcept { return _grid.at(_line, _column); }

    Cell& at(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    Cell& useCellAt(CellLocation p) noexcept { return useCellAt(p.line, p.column); }
    Cell const& at(CellLocation p) const noexcept { return _grid.at(p.line, p.column); }

    [[nodiscard]] std::string const& windowTitle() const noexcept { return _state.windowTitle; }

    /// Finds the next marker right after the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<LineOffset> findMarkerDownwards(LineOffset _currentCursorLine) const;

    /// Finds the previous marker right next to the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    [[nodiscard]] std::optional<LineOffset> findMarkerUpwards(LineOffset _currentCursorLine) const;

    /// ScreenBuffer's type, such as main screen or alternate screen.
    [[nodiscard]] ScreenType bufferType() const noexcept { return _state.screenType; }

    void scrollUp(LineCount n) { scrollUp(n, margin()); }
    void scrollDown(LineCount n) { scrollDown(n, margin()); }

    void verifyState() const override;

    [[nodiscard]] Grid<Cell> const& grid() const noexcept { return _grid; }
    [[nodiscard]] Grid<Cell>& grid() noexcept { return _grid; }

    /// @returns true iff given absolute line number is wrapped, false otherwise.
    [[nodiscard]] bool isLineWrapped(LineOffset _lineNumber) const noexcept
    {
        return _grid.isLineWrapped(_lineNumber);
    }

    [[nodiscard]] ColorPalette& colorPalette() noexcept { return _state.colorPalette; }
    [[nodiscard]] ColorPalette const& colorPalette() const noexcept { return _state.colorPalette; }

    [[nodiscard]] ColorPalette& defaultColorPalette() noexcept { return _state.defaultColorPalette; }
    [[nodiscard]] ColorPalette const& defaultColorPalette() const noexcept
    {
        return _state.defaultColorPalette;
    }

    [[nodiscard]] bool isCellEmpty(CellLocation position) const noexcept override
    {
        return _grid.lineAt(position.line).cellEmptyAt(position.column);
    }

    [[nodiscard]] bool compareCellTextAt(CellLocation position, char codepoint) const noexcept override
    {
        auto const& cell = _grid.lineAt(position.line).inflatedBuffer()[position.column.as<size_t>()];
        return CellUtil::compareText(cell, codepoint);
    }

    // IMPORTANT: Invokig inflatedBuffer() is expensive. This function should be invoked with caution.
    [[nodiscard]] std::string cellTextAt(CellLocation position) const noexcept override
    {
        return _grid.lineAt(position.line).inflatedBuffer()[position.column.as<size_t>()].toUtf8();
    }

    [[nodiscard]] std::string lineTextAt(LineOffset line) const noexcept override
    {
        return _grid.lineAt(line).toUtf8Trimmed();
    }

    [[nodiscard]] bool isLineEmpty(LineOffset line) const noexcept override
    {
        return _grid.lineAt(line).empty();
    }

    [[nodiscard]] uint8_t cellWidthAt(CellLocation position) const noexcept override
    {
        return _grid.lineAt(position.line).cellWidthAt(position.column);
    }

    [[nodiscard]] LineCount historyLineCount() const noexcept override { return _grid.historyLineCount(); }

    [[nodiscard]] HyperlinkId hyperlinkIdAt(CellLocation position) const noexcept override
    {
        auto const& line = _grid.lineAt(position.line);
        if (line.isTrivialBuffer())
        {
            TrivialLineBuffer const& lineBuffer = line.trivialBuffer();
            return lineBuffer.hyperlink;
        }
        return at(position).hyperlink();
    }

    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> hyperlinkAt(CellLocation pos) const noexcept override
    {
        return _state.hyperlinks.hyperlinkById(hyperlinkIdAt(pos));
    }

    [[nodiscard]] HyperlinkStorage const& hyperlinks() const noexcept { return _state.hyperlinks; }

    void resetInstructionCounter() noexcept { _state.instructionCounter = 0; }
    [[nodiscard]] uint64_t instructionCounter() const noexcept { return _state.instructionCounter; }
    [[nodiscard]] char32_t precedingGraphicCharacter() const noexcept
    {
        return _state.parser.precedingGraphicCharacter();
    }

    void applyAndLog(FunctionDefinition const& function, Sequence const& seq);
    [[nodiscard]] ApplyResult apply(FunctionDefinition const& function, Sequence const& seq);

    void fail(std::string const& _message) const override;

  private:
    void writeTextInternal(char32_t _char);

    /// Attempts to emplace the given character sequence into the current cursor position, assuming
    /// that the current line is either empty or trivial and the input character sequence is contiguous.
    ///
    /// @returns the string view of the UTF-8 text that could not be emplaced.
    std::string_view tryEmplaceChars(std::string_view chars, size_t cellCount) noexcept;
    size_t emplaceCharsIntoCurrentLine(std::string_view chars, size_t cellCount) noexcept;
    [[nodiscard]] bool isContiguousToCurrentLine(std::string_view continuationChars) const noexcept;
    void advanceCursorAfterWrite(ColumnCount n) noexcept;

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

    /// Applies LF but also moves cursor to given column @p _column.
    void linefeed(ColumnOffset _column);

    void writeCharToCurrentAndAdvance(char32_t _codepoint) noexcept;
    void clearAndAdvance(int _offset) noexcept;

    void scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin);
    void scrollUp(LineCount n, Margin margin);
    void scrollDown(LineCount n, Margin margin);
    void insertChars(LineOffset lineNo, ColumnCount _n);
    void deleteChars(LineOffset lineNo, ColumnOffset _column, ColumnCount _count);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(ColumnOffset _n);

    [[nodiscard]] std::unique_ptr<ParserExtension> hookSTP(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookSixel(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECRQSS(Sequence const& seq);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookXTGETTCAP(Sequence const& seq);

    Terminal& _terminal;
    Settings& _settings;
    TerminalState& _state;
    Grid<Cell> _grid;
#if defined(LIBTERMINAL_CACHE_CURRENT_LINE_POINTER)
    Line<Cell>* _currentLine = nullptr;
#endif
    std::unique_ptr<SixelImageBuilder> sixelImageBuilder_;
};

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline void Screen<Cell>::scrollUp(LineCount _n, Margin _margin)
{
    scrollUp(_n, cursor().graphicsRendition, _margin);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline bool Screen<Cell>::isContiguousToCurrentLine(std::string_view continuationChars) const noexcept
{
    auto const& line = currentLine();
    return line.isTrivialBuffer() && line.trivialBuffer().text.view().end() == continuationChars.begin();
}

} // namespace terminal