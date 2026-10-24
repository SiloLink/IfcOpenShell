// This file was generated with the assistance of an AI coding tool.
// Crash-safety contracts for the OpenCASCADE kernel: ShapeFix replacement
// cycles are rejected before they exhaust the stack, and a Boolean operand
// whose conversion fails is never consumed as partial geometry.
#include "ifcgeom/iterator.h"
#include "ifcgeom/kernels/opencascade/opencascade_kernel.h"
#include "ifcgeom/kernels/opencascade/shape_healing_context.h"

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
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

void boolean_rejects_failed_operand(int failed_operand) {
    class OperandKernel : public open_cascade_kernel {
        int failed_operand_;
        int calls_ = 0;

      public:
        OperandKernel(const Settings& settings, int failed_operand)
            : open_cascade_kernel(settings), failed_operand_(failed_operand) {}
        bool convert_impl(const taxonomy::solid::ptr solid, std::vector<conversion_result>& results) override {
            ++calls_;
            // A failed nested conversion can already have appended partial geometry.
            results.emplace_back(solid->instance.id(), solid->matrix, new open_cascade_shape(BRepPrimAPI_MakeBox(1., 1., 1.).Shape()), nullptr);
            return calls_ != failed_operand_;
        }
    };
    auto file = fixture();
    auto boolean = taxonomy::make<taxonomy::boolean_result>();
    boolean->operation = taxonomy::boolean_result::SUBTRACTION;
    boolean->instance = file->instance_by_id(102);
    for (int i = 0; i < 2; ++i) {
        auto solid = taxonomy::make<taxonomy::solid>();
        solid->instance = boolean->instance;
        boolean->children.push_back(solid);
    }
    Settings settings;
    OperandKernel kernel(settings, failed_operand);
    std::vector<conversion_result> results;
    require(!kernel.open_cascade_kernel::convert_impl(boolean, results), "Boolean accepted a failed operand's partial geometry");
    require(results.empty(), "failed Boolean emitted a partial result");
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
    run("boolean-failed-first-operand", [&]() { boolean_rejects_failed_operand(1); });
    run("boolean-failed-second-operand", [&]() { boolean_rejects_failed_operand(2); });
    std::cout << "RESULT passed=" << passed << " failed=" << failed << std::endl;
    logger.set_output(static_cast<std::ostream*>(nullptr), static_cast<std::ostream*>(nullptr));
    return failed || !passed ? 1 : 0;
}
