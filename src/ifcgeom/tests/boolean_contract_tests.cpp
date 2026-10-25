// This file was generated with the assistance of an AI coding tool.
// Crash-safety contracts for the OpenCASCADE kernel: ShapeFix replacement
// cycles are rejected before they exhaust the stack, and a Boolean operand
// whose conversion fails is never consumed as partial geometry.
#include "ifcgeom/hybrid_kernel.h"
#include "ifcgeom/iterator.h"
#include "ifcgeom/kernels/opencascade/opencascade_kernel.h"
#include "ifcgeom/kernels/opencascade/shape_healing_context.h"

#include <BRep_Builder.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <GProp_GProps.hxx>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <vector>

namespace {
using namespace ifcopenshell::geom;
using Settings = ifcopenshell::geom::settings;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// One IFC4 product (#102) whose instance can own taxonomy items.
std::unique_ptr<ifcopenshell::file> fixture() {
    const std::string text =
        "ISO-10303-21;\nHEADER;\n"
        "FILE_DESCRIPTION(('ViewDefinition [CoordinationView]'),'2;1');\n"
        "FILE_NAME('boolean-contract.ifc','2026-10-23T00:00:00',(),(),'','','');\n"
        "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n"
        "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
        "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
        "#3=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,0.00001,#2,$);\n"
        "#5=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);\n"
        "#6=IFCUNITASSIGNMENT((#5));\n"
        "#7=IFCPROJECT('0000000000000000000001',$,$,$,$,$,$,(#3),#6);\n"
        "#8=IFCCARTESIANPOINT((1.,0.,0.));\n"
        "#9=IFCPOLYLINE((#1,#8));\n"
        "#13=IFCLOCALPLACEMENT($,#2);\n"
        "#100=IFCSHAPEREPRESENTATION(#3,'Body','Curve3D',(#9));\n"
        "#101=IFCPRODUCTDEFINITIONSHAPE($,$,(#100));\n"
        "#102=IFCBUILDINGELEMENTPROXY('0000000000000000000102',$,'102',$,$,#13,#101,$,.NOTDEFINED.);\n"
        "ENDSEC;\nEND-ISO-10303-21;\n";
    std::istringstream stream(text);
    return std::make_unique<ifcopenshell::file>(stream, static_cast<int>(text.size()));
}

void healing_rejects_cycles(bool containment, bool consider_location) {
    BRep_Builder builder;
    TopoDS_Vertex a, b;
    builder.MakeVertex(a, gp_Pnt(0, 0, 0), 1.e-7);
    builder.MakeVertex(b, gp_Pnt(1, 0, 0), 1.e-7);
    util::shape_healing_context context;
    context.ModeConsiderLocation() = consider_location;
    if (containment) {
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        builder.Add(compound, a);
        context.Replace(a, compound);
    } else {
        context.Replace(a, b);
        context.Replace(b, a);
    }
    bool rejected = false;
    try {
        context.Apply(a);
    } catch (const util::cyclic_shape_replacement&) {
        rejected = true;
    }
    require(rejected, "cyclic healing was accepted or did not unwind normally");
    context.Clear();
    context.Replace(a, b);
    require(context.Apply(a).IsEqual(b), "failed healing retained active traversal state");
}

void healing_preserves_shared_subshapes() {
    BRep_Builder builder;
    TopoDS_Vertex a, b;
    builder.MakeVertex(a, gp_Pnt(0, 0, 0), 1.e-7);
    builder.MakeVertex(b, gp_Pnt(1, 0, 0), 1.e-7);
    TopoDS_Compound outer;
    builder.MakeCompound(outer);
    for (int i = 0; i < 8; ++i) {
        TopoDS_Compound child;
        builder.MakeCompound(child);
        builder.Add(child, a);
        builder.Add(outer, child);
    }
    util::shape_healing_context context;
    context.Replace(a, b);
    auto result = context.Apply(outer);
    int count = 0;
    for (TopoDS_Iterator child(result); child.More(); child.Next()) {
        TopoDS_Iterator vertex(child.Value());
        require(vertex.More() && vertex.Value().IsEqual(b), "shared vertex replacement was lost");
        ++count;
    }
    require(count == 8, "shared DAG was mistaken for a cycle");
}

void healing_preserves_location_changes() {
    BRep_Builder builder;
    TopoDS_Vertex a;
    builder.MakeVertex(a, gp_Pnt(0, 0, 0), 1.e-7);
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(2, 3, 4));
    const auto moved = a.Moved(TopLoc_Location(translation));
    util::shape_healing_context context;
    context.Replace(a, moved);
    require(context.Apply(a).IsEqual(moved), "finite location replacement was mistaken for a cycle");
    require(context.Apply(TopoDS_Shape()).IsNull(), "removed shape was not preserved");
}

void healing_preserves_long_replacement_chains() {
    BRep_Builder builder;
    std::vector<TopoDS_Vertex> vertices(256);
    util::shape_healing_context context;
    for (size_t i = 0; i < vertices.size(); ++i) {
        builder.MakeVertex(vertices[i], gp_Pnt(i, 0, 0), 1.e-7);
        if (i) {
            context.Replace(vertices[i - 1], vertices[i]);
        }
    }
    require(context.Apply(vertices.front()).IsEqual(vertices.back()), "finite replacement chain was truncated");
}

void boolean_operand_contract(int failed_operand, bool partial, taxonomy::boolean_result::operation_type operation) {
    class OperandKernel : public open_cascade_kernel {
        int failed_operand_;
        int calls_ = 0;

      public:
        OperandKernel(const Settings& settings, int failed_operand)
            : open_cascade_kernel(settings), failed_operand_(failed_operand) {}
        bool convert_impl(const taxonomy::solid::ptr solid, std::vector<conversion_result>& results) override {
            ++calls_;
            // A failed nested conversion can already have appended partial geometry.
            results.emplace_back(solid->instance.id(), solid->matrix, new open_cascade_shape(BRepPrimAPI_MakeBox(calls_ == failed_operand_ ? 100. : (calls_ == 1 ? 4. : 1.), calls_ == 1 ? 4. : 1., calls_ == 1 ? 4. : 1.).Shape()), nullptr);
            return calls_ != failed_operand_;
        }
    };
    auto file = fixture();
    auto boolean = taxonomy::make<taxonomy::boolean_result>();
    boolean->operation = operation;
    boolean->instance = file->instance_by_id(102);
    for (int i = 0; i < 3; ++i) {
        auto solid = taxonomy::make<taxonomy::solid>();
        solid->instance = boolean->instance;
        boolean->children.push_back(solid);
    }
    Settings settings;
    OperandKernel kernel(settings, failed_operand);
    kernel.partial_success_is_success = partial;
    std::vector<conversion_result> results;
    const bool expected = partial && !(failed_operand == 1 && operation == taxonomy::boolean_result::SUBTRACTION);
    require(kernel.open_cascade_kernel::convert_impl(boolean, results) == expected, "Boolean used an unavailable base or refused valid partial inputs");
    if (!expected) {
        require(results.empty(), "failed Boolean emitted a partial result");
    } else {
        require(results.size() == 1, "Boolean produced duplicate output");
        require(results[0].diagnostics().size() == 1 && results[0].diagnostics()[0].at("category") == "boolean_partial", "partial Boolean lost its expression/operand diagnostic");
        auto shape = std::dynamic_pointer_cast<open_cascade_shape>(results.front().shape());
        GProp_GProps properties;
        BRepGProp::VolumeProperties(shape->shape(), properties);
        double expected_volume = failed_operand == 1 ? 1. : 64.;
        if (operation == taxonomy::boolean_result::SUBTRACTION) {
            expected_volume = 63.;
        }
        if (operation == taxonomy::boolean_result::INTERSECTION) {
            expected_volume = 1.;
        }
        require(std::abs(properties.Mass() - expected_volume) < 1.e-8, "failed operand remnants contaminated the result or a valid operand was lost");
    }
}
void opening_material_failure_keeps_cut_geometry(bool bad_styles) {
    class OpeningShapeKernel : public open_cascade_kernel {
      public:
        using open_cascade_kernel::open_cascade_kernel;
        bool convert_impl(const taxonomy::solid::ptr solid, std::vector<conversion_result>& results) override {
            results.emplace_back(solid->instance.id(), new open_cascade_shape(BRepPrimAPI_MakeBox(1., 1., 1.).Shape()));
            return true;
        }
    };
    auto file = fixture();
    auto opening = taxonomy::make<taxonomy::solid>();
    opening->instance = file->instance_by_id(102);
    auto style = taxonomy::make<taxonomy::style>("host");
    std::vector<conversion_result> input{
        conversion_result(102, new open_cascade_shape(BRepPrimAPI_MakeBox(4., 4., 4.).Shape(), std::vector<taxonomy::style::ptr>(bad_styles ? 1 : 6, style)))};
    Settings settings;
    OpeningShapeKernel kernel(settings);
    std::vector<conversion_result> output;
    require(kernel.convert_openings(opening->instance, {{opening, taxonomy::matrix4()}}, input, taxonomy::matrix4(), output), "face materials prevented a valid cut");
    require(output.size() == 1, "opening lost its host");
    auto shape = std::dynamic_pointer_cast<open_cascade_shape>(output[0].shape());
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape->shape(), props);
    require(std::abs(props.Mass() - 63.) < 1.e-8, "face material fallback changed cut volume");
    require(shape->face_styles_complete() == !bad_styles, "material degradation not exposed to backend fallback");
    require(shape->face_styles().empty() == bad_styles, "untrusted face materials survived");
}

void opening_requires_complete_cutter(bool complete_fallback) {
    class OpeningBooleanKernel : public open_cascade_kernel {
        bool fail_second_;
        int& calls_;

      public:
        OpeningBooleanKernel(const Settings& settings, bool fail_second, int& calls)
            : open_cascade_kernel(settings), fail_second_(fail_second), calls_(calls) {}
        bool convert_impl(const taxonomy::solid::ptr item, std::vector<conversion_result>& results) override {
            ++calls_;
            const bool second = item->instance.id() == 8;
            const double side = second ? 1. : 2.;
            results.emplace_back(item->instance.id(), item->matrix, new open_cascade_shape(BRepPrimAPI_MakeBox(side, side, side).Shape()));
            return !(second && fail_second_);
        }
    };
    auto file = fixture();
    auto opening = taxonomy::make<taxonomy::boolean_result>();
    opening->instance = file->instance_by_id(102);
    opening->operation = taxonomy::boolean_result::SUBTRACTION;
    for (int id : {1, 8}) {
        auto solid = taxonomy::make<taxonomy::solid>();
        solid->instance = file->instance_by_id(id);
        opening->children.push_back(solid);
    }
    Settings settings;
    std::vector<conversion_result> host{
        conversion_result(102, new open_cascade_shape(BRepPrimAPI_MakeBox(4., 4., 4.).Shape()))};
    int first_calls = 0, second_calls = 0;
    auto first = std::make_unique<OpeningBooleanKernel>(settings, true, first_calls);
    auto second = std::make_unique<OpeningBooleanKernel>(settings, !complete_fallback, second_calls);
    auto* first_ptr = first.get();
    auto* second_ptr = second.get();
    std::vector<std::unique_ptr<kernels::abstract_kernel>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    kernels::hybrid_kernel hybrid("opening-contract", file.get(), settings, std::move(backends));
    // Repeat to check complete conversion after restoring each backend's policy.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<conversion_result> output;
        require(hybrid.convert_openings(opening->instance, {{opening, taxonomy::matrix4()}}, host, taxonomy::matrix4(), output) == complete_fallback,
                "an incomplete cutter was accepted or a complete fallback was refused");
        require(first_ptr->partial_success_is_success && second_ptr->partial_success_is_success, "opening changed backend partial-conversion policy");
        if (complete_fallback) {
            require(output.size() == 1, "opening lost its host");
            GProp_GProps properties;
            BRepGProp::VolumeProperties(std::static_pointer_cast<open_cascade_shape>(output.front().shape())->shape(), properties);
            require(std::abs(properties.Mass() - 57.) < 1.e-8, "incomplete cutter removed too much host geometry");
        } else {
            require(output.empty(), "failed cutter leaked partial output");
        }
    }
    require(first_calls > 0 && second_calls > 0, "opening did not try the later backend");
}
} // namespace

int main(int argc, char** argv) {
    auto& logger = ifcopenshell::logger::root();
    logger.verbosity(ifcopenshell::logger::LOG_ERROR);
    if (const char* plugins = std::getenv("IFCGEOM_CONTRACT_PLUGIN_DIR")) {
        ifcopenshell::plugin::set_search_paths({plugins});
    }
    std::ostringstream log;
    logger.set_output(static_cast<std::ostream*>(nullptr), &log);
    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0;
    int failed = 0;
    auto run = [&](const std::string& name, const std::function<void()>& test) {
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            return;
        }
        try {
            test();
            std::cout << "PASS " << name << std::endl;
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "FAIL " << name << ": " << e.what() << std::endl;
            ++failed;
        }
    };
    run("healing-value-cycle", [&]() { healing_rejects_cycles(false, false); });
    run("healing-containment-cycle", [&]() { healing_rejects_cycles(true, false); });
    run("healing-value-cycle-location-mode", [&]() { healing_rejects_cycles(false, true); });
    run("healing-containment-cycle-location-mode", [&]() { healing_rejects_cycles(true, true); });
    run("healing-shared-subshapes", healing_preserves_shared_subshapes);
    run("healing-location-changes", healing_preserves_location_changes);
    run("healing-long-replacement-chain", healing_preserves_long_replacement_chains);
    for (auto operation : {taxonomy::boolean_result::SUBTRACTION, taxonomy::boolean_result::UNION, taxonomy::boolean_result::INTERSECTION}) {
        for (bool partial : {false, true}) {
            for (int failed_operand : {1, 2}) {
                run("boolean-operand-" + std::to_string(operation) + "-" + std::to_string(partial) + "-" + std::to_string(failed_operand),
                    [&]() { boolean_operand_contract(failed_operand, partial, operation); });
            }
        }
    }
    run("opening-valid-face-materials", [&]() { opening_material_failure_keeps_cut_geometry(false); });
    run("opening-invalid-face-materials", [&]() { opening_material_failure_keeps_cut_geometry(true); });
    run("opening-complete-cutter-fallback", [&]() { opening_requires_complete_cutter(true); });
    run("opening-no-complete-cutter", [&]() { opening_requires_complete_cutter(false); });
    std::cout << "RESULT passed=" << passed << " failed=" << failed << std::endl;
    logger.set_output(static_cast<std::ostream*>(nullptr), static_cast<std::ostream*>(nullptr));
    return failed || !passed ? 1 : 0;
}
