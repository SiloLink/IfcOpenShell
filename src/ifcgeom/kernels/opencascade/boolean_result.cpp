#include "opencascade_kernel.h"

#include "boolean_utils.h"
#include "base_utils.h"

using namespace ifcopenshell::geom;
using namespace ifcopenshell::geom::kernels;

namespace {
	BOPAlgo_Operation op_to_occt(taxonomy::boolean_result::operation_type t) {
		switch (t) {
		case taxonomy::boolean_result::UNION: return BOPAlgo_FUSE;
		case taxonomy::boolean_result::INTERSECTION: return BOPAlgo_COMMON;
		case taxonomy::boolean_result::SUBTRACTION: return BOPAlgo_CUT;
		}
		throw std::invalid_argument("Unsupported boolean operation");
	}

	bool get_single_child(const TopoDS_Shape& s, TopoDS_Shape& child) {
		TopoDS_Iterator it(s);
		if (!it.More()) {
			return false;
		}
		child = it.Value();
		it.Next();
		return !it.More();
	}

	bool is_unbounded_halfspace(const TopoDS_Shape& solid) {
		if (solid.ShapeType() != TopAbs_SOLID) {
			return false;
		}
		TopoDS_Shape shell;
		if (!get_single_child(solid, shell)) {
			return false;
		}
		TopoDS_Shape face;
		if (!get_single_child(shell, face)) {
			return false;
		}
		TopoDS_Iterator it(face);
		return !it.More();
	}
}

bool open_cascade_kernel::convert_impl(const taxonomy::boolean_result::ptr br, std::vector<conversion_result>& results) {
    return handle_occt_exception([&]() -> bool {
        bool valid_result = false;
        bool first = true;
        const double tol = settings_.get<settings::Precision>().get();

        TopoDS_Shape a;
        NCollection_List<TopoDS_Shape> b;

        taxonomy::style::ptr first_item_style;
        std::vector<diagnostic_fields> diagnostics;
        size_t operand_index = 0;

        for (auto& c : br->children) {
            ++operand_index;
            std::vector<ifcopenshell::geom::conversion_result> cr;
            bool converted = false;
            std::string failure_reason = "operand_conversion_unavailable";
            try {
                converted = abstract_kernel::convert(c, cr) && !cr.empty();
                if (converted) {
                    converted = std::all_of(cr.begin(), cr.end(), [](const auto& r) {
                        const auto shape = std::dynamic_pointer_cast<open_cascade_shape>(r.shape());
                        return shape && !shape->shape().IsNull();
                    });
                }
            } catch (const std::exception& e) {
                failure_reason = std::string("std_exception: ") + e.what();
                logger_.message(ifcopenshell::logger::LOG_NOTICE, std::string("Boolean operand conversion exception: ") + e.what(), c->instance);
            } catch (...) {
                failure_reason = "unknown_exception";
                logger_.message(ifcopenshell::logger::LOG_NOTICE, "Boolean operand conversion exception", c->instance);
            }
            if (!converted) {
                // A subtraction's base cannot be replaced by its first surviving cutter.
                if (!partial_success_is_success || (first && br->operation == taxonomy::boolean_result::SUBTRACTION)) {
                    return false;
                }
                logger_.notice("Boolean operand unavailable; action=skip_operand candidate=partial operation=" + std::to_string(br->operation) + " expression=#" + std::to_string(br->instance.id()), c->instance);
                diagnostics.push_back({{"category", "boolean_partial"}, {"stage", "boolean"}, {"source_id", std::to_string(br->instance.id())}, {"detail_id", std::to_string(operand_index)}, {"operand_id", std::to_string(c->instance.id())}, {"operation", std::to_string(br->operation)}, {"reason", failure_reason}, {"action", "skip_operand"}, {"outcome", "partial"}});
                first = false;
                continue; // Discard cr, including output left by a failed conversion.
            }
            for (const auto& result : cr) {
                diagnostics.insert(diagnostics.end(), result.diagnostics().begin(), result.diagnostics().end());
            }
            if (first && br->operation == taxonomy::boolean_result::SUBTRACTION) {
                // @todo A will be null on union/intersection, intended?
                ifcopenshell::geom::util::flatten_shape_list(cr, a, false, true, settings_.get<settings::Precision>().get());
                if (a.IsNull()) {
                    return false;
                }
                first_item_style = c->surface_style;
                if (!first_item_style && c->kind() == taxonomy::COLLECTION && !taxonomy::cast<taxonomy::collection>(c)->children.empty()) {
                    // @todo recursively right?
                    first_item_style = taxonomy::cast<taxonomy::geom_item>(taxonomy::cast<taxonomy::collection>(c)->children[0])->surface_style;
                }

                if (settings_.get<settings::DisableBooleanResult>().get()) {
                    results.emplace_back(ifcopenshell::geom::conversion_result(
                        br->instance.id(),
                        br->matrix,
                        new open_cascade_shape(a),
                        br->surface_style ? br->surface_style : first_item_style));
                    return true;
                }

                const double first_operand_volume = util::shape_volume(a);
                if (first_operand_volume <= ALMOST_ZERO) {
                    logger_.message(ifcopenshell::logger::LOG_WARNING, "GEO", 119, "Empty solid for:", c->instance);
                }
            } else {

                for (auto& r : cr) {
                    auto S = std::static_pointer_cast<open_cascade_shape>(r.shape())->shape();
                    if (S.IsNull()) {
                        logger_.error("GEO", 120, "Null operand");
                        continue;
                    }
                    gp_GTrsf trsf;
                    convert(r.placement(), trsf);
                    // @todo it really confuses me why I cannot use Moved() here instead
                    S.Location(S.Location() * trsf.Trsf());

                    if (is_unbounded_halfspace(S)) {
                        double d;
                        TopoDS_Shape result;
                        util::fit_halfspace(a, S, result, d, tol * 1e3);
                        // #2665 we also set a precision-independent threshold, because in the boolean op routine
                        // the working fuzziness might still be increased.
                        if (d < tol * 20. || d < 0.00002) {
                            logger_.message(ifcopenshell::logger::LOG_WARNING, "GEO", 121, "Halfspace subtraction yields unchanged volume:", c->instance);
                            continue;
                        } else {
                            S = result;
                        }
                    } else {
                        S = util::ensure_fit_for_subtraction(S, tol);
                    }

                    b.Append(S);
                }
            }
            first = false;
        }

        util::boolean_settings bst;
        bst.attempt_2d = settings_.get<settings::BooleanAttempt2d>().get();
        bst.debug = settings_.get<settings::DebugBooleanOperations>().get();
        bst.precision = settings_.get<settings::Precision>().get();
        bst.logger = &logger_;

        TopoDS_Shape r;

        if (br->operation == taxonomy::boolean_result::SUBTRACTION && !a.IsNull() && a.ShapeType() == TopAbs_COMPOUND && TopoDS_Iterator(a).More() && util::is_nested_compound_of_solid(a)) {
            TopoDS_Compound C;
            BRep_Builder B;
            B.MakeCompound(C);
            TopoDS_Iterator it(a);
            valid_result = true;
            for (; it.More(); it.Next()) {
                TopoDS_Shape part;
                if (util::boolean_operation(bst, it.Value(), b, op_to_occt(br->operation), part)) {
                    B.Add(C, part);
                } else {
                    valid_result = false;
                }
            }
            r = C;
        } else {
            if (br->operation != taxonomy::boolean_result::SUBTRACTION && a.IsNull()) {
                if (b.IsEmpty()) {
                    return false;
                }
                a = b.First();
                b.RemoveFirst();
            }
            if (a.IsNull()) {
                return false;
            }
            if (b.IsEmpty()) {
                r = a;
                valid_result = true;
            } else {
                valid_result = util::boolean_operation(bst, a, b, op_to_occt(br->operation), r);
            }
        }

        if (valid_result) {
            std::swap(r, a);
        } else {
            if (!partial_success_is_success || a.IsNull()) {
                return false;
            }
            logger_.notice("Boolean operation failed; action=retain_valid_base candidate=partial", br->instance);
            diagnostics.push_back({{"category", "boolean_partial"}, {"stage", "boolean"}, {"source_id", std::to_string(br->instance.id())}, {"detail_id", "operation"}, {"operation", std::to_string(br->operation)}, {"reason", "boolean_operation_failed"}, {"action", "retain_valid_base"}, {"outcome", "partial"}});
        }

        results.emplace_back(ifcopenshell::geom::conversion_result(
            br->instance.id(),
            br->matrix,
            new open_cascade_shape(a),
            br->surface_style ? br->surface_style : first_item_style));
        for (auto& diagnostic : diagnostics) {
            results.back().add_diagnostic(std::move(diagnostic));
        }

        return true;
    });
}
