/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2022 Christian Parpart <christian@parpart.family>
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

#ifdef __has_include
    #if __has_include(<version>)
        #include <version>
    #endif
#endif

// clang-format off

#if defined(__cpp_concepts) && __cpp_concepts >= 201500L && \
    defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L

#include <terminal/CellFlags.h>
#include <terminal/Color.h>
#include <terminal/ColorPalette.h>
#include <terminal/GraphicsAttributes.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>
#include <terminal/primitives.h>

namespace terminal
{

#include <concepts>

/**
 * Terminal Cell Concept!
 *
 * We're doing this in order to to eventually support two different - yet most efficient -
 * Cell implementations. One for the primary screen and one for the alternate screen.
 *
 * The primary screen's grid cell will have different use patterns than the one for the alternate screen,
 * which makes it a perfect contender to optimize the Cell's implementation based on their use.
 *
 * The Cell for the primary screen must be lightweight and fast for the standard text-scrolling case,
 * whereas the alternate-screen's Cell will most likely use all the Cell's features a intensively
 * but won't be needed for scrollback.
 */
template <typename T>
concept CellConcept = requires(T t)
{
    t.reset();
    t.reset(GraphicsAttributes{});
    t.reset(GraphicsAttributes{}, HyperlinkId{});

    { t.empty() } noexcept -> std::same_as<bool>;

    t.write(GraphicsAttributes{}, char32_t{}, uint8_t{});
    t.write(GraphicsAttributes{}, char32_t{}, uint8_t{}, HyperlinkId{});
    t.writeTextOnly(char32_t{}, uint8_t{});

    { t.codepoints() } -> std::convertible_to<std::u32string>;
    { t.codepoint(size_t{}) } noexcept -> std::same_as<char32_t>;
    { t.codepointCount() } noexcept -> std::same_as<size_t>;

    t.setCharacter(char32_t{});
    { t.appendCharacter(char32_t{}) } -> std::same_as<int>;

    { t.toUtf8() } -> std::convertible_to<std::string>;

    { t.width() } noexcept -> std::convertible_to<uint8_t>;
    { t.setWidth(uint8_t{}) } noexcept;

    { t.compareText(char{}) } noexcept -> std::same_as<bool>;

    { t.flags() } noexcept -> std::same_as<CellFlags>;
    { t.isFlagEnabled(CellFlags{}) } noexcept -> std::same_as<bool>;
    t.resetFlags();
    t.setFlags(CellFlags{}, bool{});

    t.setGraphicsRendition(GraphicsRendition{});

    t.setForegroundColor(Color{});
    { t.foregroundColor() } noexcept -> std::same_as<Color>;

    t.setBackgroundColor(Color{});
    { t.backgroundColor() } noexcept -> std::same_as<Color>;

    t.setUnderlineColor(Color{});
    { t.underlineColor() } noexcept -> std::same_as<Color>;

    { t.getUnderlineColor(ColorPalette{}, RGBColor{} /*defaultColor*/) } -> std::same_as<RGBColor>;

    { t.makeColors(ColorPalette{}, bool{} /*reverseVideo*/, bool{} /*blink*/, bool{} /*rapidBlink*/) } -> std::same_as<RGBColorPair>;

    { t.imageFragment() } -> std::same_as<std::shared_ptr<ImageFragment>>;
    t.setImageFragment(std::shared_ptr<RasterizedImage>{}, CellLocation{} /*offset*/);

    { t.hyperlink() } -> std::same_as<HyperlinkId>;
    t.setHyperlink(HyperlinkId{});
};


} // namespace terminal

// clang-format on
#endif