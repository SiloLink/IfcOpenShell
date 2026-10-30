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

#ifndef HYBRID_KERNEL_H
#define HYBRID_KERNEL_H

#include "abstract_kernel.h"
#include "kernel_registry.h"

#include <algorithm>
#include <map>
#include <set>

namespace {
using face_style_map = std::map<int, std::set<ifcopenshell::geom::taxonomy::style::ptr>>;

face_style_map source_face_styles(
    const ifcopenshell::geom::taxonomy::ptr& item) {
    face_style_map result;
    if (auto collection = std::dynamic_pointer_cast<ifcopenshell::geom::taxonomy::collection>(item)) {
        for (const auto& child : collection->children) {
            for (const auto& child_styles : source_face_styles(child)) {
                result[child_styles.first].insert(child_styles.second.begin(), child_styles.second.end());
            }
        }
    } else {
        auto& styles = result[item->instance.id()];
        if (auto shell = std::dynamic_pointer_cast<ifcopenshell::geom::taxonomy::shell>(item)) {
            for (const auto& face : shell->children) {
                styles.insert(face->surface_style);
            }
        }
    }
    return result;
}

bool preserves_face_styles(
    const std::vector<ifcopenshell::geom::conversion_result>& results,
    size_t begin,
    const face_style_map& source) {
    for (size_t i = begin; i < results.size(); ++i) {
        auto source_styles = source.find(results[i].ItemId());
        if (source_styles == source.end()) {
            return false;
        }
        const auto& shape = results[i].shape();
        if (!shape || !shape->face_styles_complete()) {
            return false;
        }
        const auto& styles = shape->face_styles();
        if (styles.empty() && std::none_of(
                                  source_styles->second.begin(), source_styles->second.end(), [](const auto& style) { return !!style; })) {
            continue;
        }
        if (styles.empty() && source_styles->second.size() == 1 &&
            results[i].style_ptr() == *source_styles->second.begin()) {
            // A uniform source style applies to every surviving face without
            // requiring a backend-specific per-face association.
            continue;
        }
        // Degenerate source faces may be discarded. Every surviving face must
        // still have style provenance from its own representation item.
        if (styles.size() != static_cast<size_t>(shape->num_faces())) {
            return false;
        }
        for (const auto& style : styles) {
            if (source_styles->second.find(style) == source_styles->second.end()) {
                return false;
            }
        }
    }
    return true;
}
} // namespace

namespace ifcopenshell {
	namespace geom {
		namespace kernels {

			class hybrid_kernel : public ifcopenshell::geom::kernels::abstract_kernel {
				std::vector<std::unique_ptr<abstract_kernel>> kernels_;
				ifcopenshell::geom::abstract_mapping* mapping_;
				ifcopenshell::file* file_;
			public:
				hybrid_kernel(const std::string& name, ifcopenshell::file* file, ifcopenshell::geom::settings& settings, std::vector<std::unique_ptr<abstract_kernel>>&& kernels, ifcopenshell::logger& logger = ifcopenshell::logger::root())
					: abstract_kernel(name, settings, logger)
					, kernels_(std::move(kernels))
					, mapping_(ifcopenshell::geom::impl::mapping_implementations().construct(file, settings, logger))
					, file_(file)
				{
				}
				virtual bool supports_boolean_operations() const
				{
					for (auto& k : kernels_) {
						if (k->supports_boolean_operations()) {
							return true;
						}
					}
					return false;
				}
				bool supports_face_styles() const override {
					return std::any_of(kernels_.begin(), kernels_.end(), [](const auto& k) { return k->supports_face_styles(); });
				}
				virtual bool convert(const taxonomy::ptr item, std::vector<ifcopenshell::geom::conversion_result>& rs)
				{
                    auto ops = mapping_->find_openings(item->instance);
                    auto face_styles = source_face_styles(item);
                    bool has_face_styles = std::any_of(face_styles.begin(), face_styles.end(), [](const auto& source) {
                        return std::any_of(source.second.begin(), source.second.end(), [](const auto& style) { return !!style; });
                    });
                    bool has_openings = ops.size();
                    auto eligible = [&](const auto& k) {
#ifdef IFOPSH_WITH_CGAL
                        if (has_openings && !k->supports_boolean_operations()) {
                            // @todo this would fail later on in the find_openings() call, because we have a
                            // SimpleCgalShape which cannot be used on a kernel that supports booleans.
                            // @todo 1 implement the translation between various conversion result shapes
                            // @todo 2 fold the boolean result openings into the taxonomy item. This should be possible
                            //         now that we have shared_ptr<item> and caching in place. So the inability
                            //         to instance wouldn't matter as much.
                            return false;
                        }
#endif
                        return !has_openings || k->geometry_library() != "passthrough";
                    };
                    std::vector<ifcopenshell::geom::conversion_result> material_fallback;
                    bool material_fallback_partial = false;
                    size_t failed_attempts = 0;
                    auto event = [&](const char* category, const char* action, const char* outcome) {
                        return diagnostic_fields{{"category", category}, {"stage", "geometry"}, {"source_id", std::to_string(item->instance.id())}, {"reason", "complete_backend_requirements_unavailable"}, {"action", action}, {"outcome", outcome}};
                    };
                    auto publish_material_fallback = [&]() {
                        if (material_fallback.empty()) {
                            return false;
                        }
                        material_fallback.front().add_diagnostic(event("material_degraded", "use_object_or_default_material", "degraded"));
                        if (material_fallback_partial) {
                            material_fallback.front().add_diagnostic(event("geometry_partial", "use_partial_conversion", "partial"));
                            logger_.warning("Complete geometry unavailable; action=use_partial_conversion outcome=partial material=degraded", item->instance);
                        }
                        rs.insert(rs.end(), material_fallback.begin(), material_fallback.end());
                        logger_.warning("Face material provenance unavailable; action=use_object_or_default_material outcome=degraded", item->instance);
                        return true;
                    };
                    auto attempt = [&](abstract_kernel& k, bool allow_partial) {
                        k.apply_settings(settings_);
                        conversion_mode_scope mode(k, allow_partial);
                        bool success = false;
                        std::vector<ifcopenshell::geom::conversion_result> results;
                        try {
                            success = k.convert(item, results);
                        } catch (const std::exception& e) {
                            logger_.message(ifcopenshell::logger::LOG_NOTICE, "Geometry attempt exception; backend=" + k.geometry_library() + " reason=" + e.what(), item->instance);
                        } catch (...) {
                            logger_.message(ifcopenshell::logger::LOG_NOTICE, "Geometry attempt exception; backend=" + k.geometry_library(), item->instance);
                        }

                        if (success && has_face_styles) {
                            for (auto& result : results) {
                                const auto source = face_styles.find(result.ItemId());
                                if (source != face_styles.end() && source->second.size() == 1 &&
                                    *source->second.begin() && result.shape() &&
                                    result.shape()->face_styles_complete() && result.shape()->face_styles().empty()) {
                                    // Preserve the exact authored face style, including opacity.
                                    // The representation's object style may be different.
                                    result.setStyle(*source->second.begin());
                                }
                            }
                        }
                        if (success && has_face_styles && !preserves_face_styles(results, 0, face_styles)) {
                            if (material_fallback.empty() && std::all_of(results.begin(), results.end(), [&](const auto& r) {
                                    return r.shape() && face_styles.count(r.ItemId());
                                })) {
                                std::vector<conversion_result> fallback;
                                for (const auto& result : results) {
                                    if (result.shape()->face_styles().empty() && result.shape()->face_styles_complete()) {
                                        fallback.push_back(result);
                                        continue;
                                    }
                                    auto* shape = result.shape()->clone_without_face_styles();
                                    if (!shape) {
                                        fallback.clear();
                                        break;
                                    }
                                    fallback.emplace_back(result.ItemId(), result.placement(), shape, result.style_ptr());
                                    fallback.back().inherit_diagnostics(result);
                                }
                                material_fallback = std::move(fallback);
                                material_fallback_partial = allow_partial;
                            }
                            success = false;
                        }
                        if (success) {
                            if (!results.empty() && allow_partial) {
                                results.front().add_diagnostic(event("geometry_partial", "use_partial_conversion", "partial"));
                            } else if (!results.empty() && failed_attempts) {
                                results.front().add_diagnostic(event("fallback_recovered", "use_complete_backend", "recovered"));
                            }
                            rs.insert(rs.end(), results.begin(), results.end());
                            if (failed_attempts && !allow_partial) {
                                logger_.message(ifcopenshell::logger::LOG_NOTICE, "Geometry fallback; outcome=recovered backend=" + k.geometry_library() + " failed_attempts=" + std::to_string(failed_attempts), item->instance);
                            }
                        } else {
                            ++failed_attempts;
                        }
                        return success;
                    };
                    for (auto& k : kernels_) {
                        if (eligible(k) && attempt(*k, false)) {
                            return true;
                        }
                    }
                    if (publish_material_fallback()) {
                        return true;
                    }
                    // Prefer complete geometry from any backend. Retry partial conversion
                    // with a fresh cache, after discarding all failed attempts' output.
                    for (auto& k : kernels_) {
                        if (!eligible(k)) {
                            continue;
                        }
                        try {
                            std::unique_ptr<abstract_kernel> partial(k->clone(logger_));
                            if (attempt(*partial, true)) {
                                logger_.warning("Complete geometry unavailable; action=use_partial_conversion outcome=partial", item->instance);
                                return true;
                            }
                        } catch (...) {
                        }
                    }
                    if (publish_material_fallback()) {
                        return true;
                    }
                    logger_.warning("Geometry conversion exhausted all backends; action=return_unavailable_for_recovery", item->instance);
                    return false;
                }
				virtual bool apply_layerset(std::vector<ifcopenshell::geom::conversion_result>& items, const ifcopenshell::geom::layerset_information& layers)
				{
					for (auto& k : kernels_) {
						bool success = false;
                        k->apply_settings(settings_);
                        try {
                            success = k->apply_layerset(items, layers);
                        } catch (...) {
                            success = false;
                        }
                        if (success) {
							return true;
						}
					}
					return false;
				}
				virtual bool apply_folded_layerset(std::vector<ifcopenshell::geom::conversion_result>& items, const ifcopenshell::geom::layerset_information& layers, const std::map<express::base, ifcopenshell::geom::layerset_information>& folds)
				{
					for (auto& k : kernels_) {
						bool success = false;
                        k->apply_settings(settings_);
                        try {
                            success = k->apply_folded_layerset(items, layers, folds);
                        } catch (...) {
                            success = false;
                        }
                        if (success) {
							return true;
						}
					}
					return false;
				}
                virtual bool convert_openings_impl(const express::base& entity, const std::vector<std::pair<taxonomy::ptr, ifcopenshell::geom::taxonomy::matrix4>>& openings, const std::vector<ifcopenshell::geom::conversion_result>& entity_shapes, const ifcopenshell::geom::taxonomy::matrix4& entity_trsf, std::vector<ifcopenshell::geom::conversion_result>& cut_shapes) {
                    std::vector<conversion_result> material_fallback;
                    size_t failed_attempts = 0;
                    std::string opening_ids;
                    for (const auto& opening : openings) {
                        opening_ids += std::to_string(opening.first->instance.id()) + ",";
                    }
                    auto event = [&](const char* category, const char* action, const char* outcome) {
                        return diagnostic_fields{{"category", category}, {"stage", "openings"}, {"source_id", std::to_string(entity.id())}, {"detail_id", opening_ids}, {"reason", "earlier_backend_requirements_unavailable"}, {"action", action}, {"outcome", outcome}};
                    };
                    for (auto& k : kernels_) {
                        bool is_valid = true;
                        for (auto& s : entity_shapes) {
                            if (!k->accepts(*s.shape())) {
                                is_valid = false;
                                break;
                            }
                        }
                        if (!is_valid) {
                            continue;
                        }
                        bool success = false;
                        k->apply_settings(settings_);
                        // Each backend applies the complete-cutter contract in
                        // convert_openings; the converter can skip a whole failed
                        // opening afterwards.
                        try {
                            std::vector<ifcopenshell::geom::conversion_result> candidate;
                            success = k->convert_openings(entity, openings, entity_shapes, entity_trsf, candidate);
                            if (success) {
                                const bool valid_shapes = std::all_of(candidate.begin(), candidate.end(), [](const auto& r) { return !!r.shape(); });
                                const bool complete = valid_shapes && std::all_of(candidate.begin(), candidate.end(), [](const auto& r) {
                                                          return r.shape()->face_styles_complete();
                                                      });
                                if (complete) {
                                    if (!candidate.empty() && failed_attempts) {
                                        candidate.front().add_diagnostic(event("fallback_recovered", "use_complete_backend", "recovered"));
                                    }
                                    cut_shapes.insert(cut_shapes.end(), candidate.begin(), candidate.end());
                                } else {
                                    if (valid_shapes && material_fallback.empty()) {
                                        material_fallback = std::move(candidate);
                                    }
                                    success = false;
                                }
                            }
                        } catch (const std::exception& e) {
                            logger_.notice("Opening attempt exception; backend=" + k->geometry_library() + " reason=" + e.what(), entity);
                            success = false;
                        } catch (...) {
                            logger_.notice("Opening attempt exception; backend=" + k->geometry_library(), entity);
                            success = false;
                        }
                        if (success) {
                            if (failed_attempts) {
                                logger_.notice("Opening fallback; outcome=recovered backend=" + k->geometry_library() + " failed_attempts=" + std::to_string(failed_attempts), entity);
                            }
                            return true;
                        }
                        ++failed_attempts;
                        logger_.notice("Opening attempt failed; action=try_next_backend backend=" + k->geometry_library(), entity);
                    }
                    if (!material_fallback.empty()) {
                        material_fallback.front().add_diagnostic(event("material_degraded", "use_object_or_default_material", "degraded"));
                        cut_shapes.insert(cut_shapes.end(), material_fallback.begin(), material_fallback.end());
                        logger_.warning("Opening face materials unavailable; action=use_object_or_default_material outcome=degraded", entity);
                        return true;
                    }
                    return false;
                }
                virtual abstract_kernel* clone(ifcopenshell::logger& logger) const {
                    std::vector<std::unique_ptr<abstract_kernel>> ks;
                    for (auto& k : kernels_) {
                        ks.emplace_back(k->clone(logger));
                    }
                    // @todo ugly
                    return new hybrid_kernel(geometry_library(), file_, const_cast<ifcopenshell::geom::settings&>(settings()), std::move(ks), logger);
                }
            };
            } // namespace kernels
            } // namespace geom
            } // namespace ifcopenshell

#endif
