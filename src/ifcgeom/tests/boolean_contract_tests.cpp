// This file was generated with the assistance of an AI coding tool.
// Crash-safety contracts for the OpenCASCADE kernel: ShapeFix replacement
// cycles are rejected before they exhaust the stack.
#include "ifcgeom/iterator.h"
#include "ifcgeom/kernels/opencascade/opencascade_kernel.h"
#include "ifcgeom/kernels/opencascade/shape_healing_context.h"

#include <BRep_Builder.hxx>
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
    std::cout << "RESULT passed=" << passed << " failed=" << failed << std::endl;
    logger.set_output(static_cast<std::ostream*>(nullptr), static_cast<std::ostream*>(nullptr));
    return failed || !passed ? 1 : 0;
}
