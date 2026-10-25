// This file was generated with the assistance of an AI coding tool.
#ifndef IFCGEOM_SHAPE_HEALING_CONTEXT_H
#define IFCGEOM_SHAPE_HEALING_CONTEXT_H

#include <ShapeBuild_ReShape.hxx>
#include <stdexcept>
#include <TopTools_MapOfShape.hxx>

namespace ifcopenshell::geom::util {

class cyclic_shape_replacement : public std::runtime_error {
  public:
    cyclic_shape_replacement() : std::runtime_error("Cyclic shape replacement during OpenCASCADE healing") {}
};

// OCCT 7.8's ShapeBuild_ReShape recursively follows replacements without
// detecting cycles. Reject a cyclic healing operation before it exhausts the
// stack; never accept a partially healed Boolean result as its replacement.
class shape_healing_context final : public ShapeBuild_ReShape {
    TopTools_MapOfShape active_;

  public:
    using ShapeBuild_ReShape::Apply;

    TopoDS_Shape Apply(const TopoDS_Shape& shape, const TopAbs_ShapeEnum until = TopAbs_SHAPE) override {
        if (shape.IsNull()) {
            return shape;
        }
        TopoDS_Shape key = shape;
        if (ModeConsiderLocation()) {
            key.Location(TopLoc_Location());
        }
        if (!active_.Add(key)) {
            throw cyclic_shape_replacement();
        }
        struct scope {
            TopTools_MapOfShape& active;
            TopoDS_Shape key;
            ~scope() { active.Remove(key); }
        } cleanup{active_, key};
        return ShapeBuild_ReShape::Apply(shape, until);
    }
};
} // namespace ifcopenshell::geom::util

#endif
