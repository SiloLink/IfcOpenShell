#include "opencascade_kernel.h"

#include "base_utils.h"

#include <algorithm>

using namespace ifcopenshell::geom;
using namespace ifcopenshell::geom::kernels;
using namespace ifcopenshell::geom::util;

namespace {
	// @todo move into taxonomy;
	bool shell_polyhedral(const taxonomy::shell::ptr& sh) {
		for (auto& f : sh->children) {
			for (auto& w : f->children) {
				if (!w->is_polyhedron()) {
					return false;
				}
			}
			if (f->basis && f->basis->kind() != taxonomy::PLANE) {
				return false;
			}
		}
		return true;
	}
}

bool open_cascade_kernel::convert(
	const taxonomy::shell::ptr l,
	TopoDS_Shape& shape,
	std::vector<taxonomy::style::ptr>* face_styles
) {
	std::unique_ptr<faceset_helper> helper_scope;
	if (shell_polyhedral(l)) {
		helper_scope.reset(new faceset_helper(this, l));
	}

	faceset_helper_ = helper_scope.get();

	double minimal_face_area = precision_ * precision_ * 0.5;

	double min_face_area = faceset_helper_
		? (faceset_helper_->epsilon() * faceset_helper_->epsilon() / 20.)
		: minimal_face_area;

	NCollection_List<TopoDS_Shape> face_list;
	std::vector<std::pair<TopoDS_Face, taxonomy::style::ptr>> source_faces;
	for (auto& face : l->children) {
		bool success = false;
		TopoDS_Face occ_face;

		try {
			success = convert(face, occ_face);
		} catch (const std::exception& e) {
			logger_.error("GEO", 194, e);
		} catch (const Standard_Failure& e) {
			if (e.GetMessageString() && strlen(e.GetMessageString())) {
				logger_.error("GEO", 195, e.GetMessageString());
			} else {
				logger_.error("GEO", 196, "Unknown error creating face");
			}
		} catch (...) {
			logger_.error("GEO", 197, "Unknown error creating face");
		}

		if (!success) {
			logger_.message(ifcopenshell::logger::LOG_WARNING, "GEO", 198, "Failed to convert face:", face->instance);
			continue;
		}

		if (occ_face.ShapeType() == TopAbs_COMPOUND) {
			TopoDS_Iterator face_it(occ_face, false);
			for (; face_it.More(); face_it.Next()) {
				if (face_it.Value().ShapeType() == TopAbs_FACE) {
					// This should really be the case. This is not asserted.
					const TopoDS_Face& triangle = TopoDS::Face(face_it.Value());
					if (face_area(triangle) > min_face_area) {
						face_list.Append(triangle);
						if (face_styles) {
							source_faces.emplace_back(triangle, face->surface_style);
						}
					} else {
						logger_.message(ifcopenshell::logger::LOG_WARNING, "GEO", 199, "Degenerate face:", face->instance);
					}
				}
			}
		} else {
			if (face_area(occ_face) > min_face_area) {
				face_list.Append(occ_face);
				if (face_styles) {
					source_faces.emplace_back(occ_face, face->surface_style);
				}
			} else {
				logger_.message(ifcopenshell::logger::LOG_WARNING, "GEO", 200, "Degenerate face:", face->instance);
			}
		}
	}

	if (face_list.Extent() == 0) {
		return false;
	}

	// @todo
	// face_list.Extent() <= settings_.get<settings::MaxFacesToReorient>().get() &&

	if (!settings_.get<settings::ReorientShells>().get() || !create_solid_from_faces(face_list, shape, settings_.get<settings::Precision>().get())) {
		TopoDS_Compound compound;
		BRep_Builder builder;
		builder.MakeCompound(compound);

		NCollection_List<TopoDS_Shape>::Iterator face_iterator;
		for (face_iterator.Initialize(face_list); face_iterator.More(); face_iterator.Next()) {
			builder.Add(compound, face_iterator.Value());
		}
		shape = compound;
	}

	if (face_styles) {
		face_styles->clear();
		const bool has_face_styles = std::any_of(
			source_faces.begin(),
			source_faces.end(),
			[](const auto& source) { return !!source.second; });
		if (has_face_styles) {
			std::vector<bool> matched_sources(source_faces.size(), false);
			TopExp_Explorer output_faces(shape, TopAbs_FACE);
			for (; output_faces.More(); output_faces.Next()) {
				const auto& output_face = TopoDS::Face(output_faces.Current());
				int match = -1;
				for (size_t i = 0; i < source_faces.size(); ++i) {
					if (output_face.IsSame(source_faces[i].first)) {
						if (match != -1) {
							match = -2;
							break;
						}
						match = static_cast<int>(i);
					}
				}

				if (match >= 0) {
					face_styles->push_back(source_faces[match].second);
					matched_sources[match] = true;
				} else {
					face_styles->push_back(nullptr);
				}
			}

			bool lost_style = false;
			for (size_t i = 0; i < source_faces.size(); ++i) {
				if (source_faces[i].second && !matched_sources[i]) {
					lost_style = true;
					break;
				}
			}
			if (lost_style) {
				logger_.warning("Unable to preserve all face styles after shell conversion", l->instance);
				face_styles->clear();
			}
		}
	}

	return true;
}

bool open_cascade_kernel::convert_impl(const taxonomy::shell::ptr shell, std::vector<ifcopenshell::geom::conversion_result>& results) {
    return handle_occt_exception([&]() -> bool {

	TopoDS_Shape shape;
	std::vector<taxonomy::style::ptr> face_styles;
	if (!convert(shell, shape, &face_styles)) {
		return false;
	}
	results.emplace_back(conversion_result(
		shell->instance.id(),
		shell->matrix,
		new open_cascade_shape(shape, std::move(face_styles)),
		shell->surface_style
	));
	return true;

	});
}
