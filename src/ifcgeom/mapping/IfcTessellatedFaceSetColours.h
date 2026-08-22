/********************************************************************************
 *                                                                              *
 * This file is part of IfcOpenShell.                                           *
 *                                                                              *
 * IfcOpenShell is free software: you can redistribute it and/or modify         *
 * it under the terms of the Lesser GNU General Public License as published by  *
 * the Free Software Foundation, either version 3.0 of the License, or          *
 * (at your option) any later version.                                          *
 *                                                                              *
 * IfcOpenShell is distributed in the hope that it will be useful,              *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of               *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                 *
 * Lesser GNU General Public License for more details.                          *
 *                                                                              *
 * You should have received a copy of the Lesser GNU General Public License     *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.         *
 *                                                                              *
 ********************************************************************************/

#pragma once

#include <cmath>
#include <vector>

namespace {

struct indexed_colour_mapping {
    std::vector<ifcopenshell::geom::taxonomy::style::ptr> styles;
    std::vector<int> face_style_indices;
};

template <typename FaceSet>
indexed_colour_mapping map_indexed_colours(
    const FaceSet& inst,
    size_t face_count,
    ifcopenshell::logger& logger
) {
    using namespace ifcopenshell::geom;

    indexed_colour_mapping result;
    result.face_style_indices.resize(face_count, -1);

    auto colour_maps = inst.HasColours();
    if (colour_maps.empty()) {
        return result;
    }
    if (colour_maps.size() > 1) {
        logger.warning("Multiple IfcIndexedColourMaps ignored", inst);
        return result;
    }

    auto colour_map = colour_maps.front();
    auto palette = colour_map.Colours().ColourList();
    auto colour_indices = colour_map.ColourIndex();
    auto opacity_value = colour_map.Opacity();
    const double opacity = opacity_value ? *opacity_value : 1.;

    bool valid = !palette.empty()
        && std::isfinite(opacity)
        && opacity >= 0.
        && opacity <= 1.
        && colour_indices.size() == face_count;

    for (const auto& colour : palette) {
        if (colour.size() < 3) {
            valid = false;
            break;
        }
        for (size_t channel = 0; channel < 3; ++channel) {
            if (!std::isfinite(colour[channel]) || colour[channel] < 0. || colour[channel] > 1.) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            break;
        }
    }

    for (int64_t index : colour_indices) {
        if (index < 1 || static_cast<size_t>(index) > palette.size()) {
            valid = false;
            break;
        }
    }

    if (!valid) {
        logger.warning("Invalid IfcIndexedColourMap ignored", colour_map);
        return result;
    }

    result.styles.reserve(palette.size());
    for (size_t i = 0; i < palette.size(); ++i) {
        auto style = taxonomy::make<taxonomy::style>();
        style->instance = colour_map;
        style->name = "IfcIndexedColourMap-" + std::to_string(colour_map.id()) + "-" + std::to_string(i + 1);
        style->surface = taxonomy::colour(palette[i][0], palette[i][1], palette[i][2]);
        style->diffuse = style->surface;
        style->transparency = 1. - opacity;
        result.styles.push_back(style);
    }

    for (size_t i = 0; i < colour_indices.size(); ++i) {
        result.face_style_indices[i] = static_cast<int>(colour_indices[i] - 1);
    }

    return result;
}

} // namespace
