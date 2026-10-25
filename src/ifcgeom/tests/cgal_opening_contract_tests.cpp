// Complete opening cutters must retain every Boolean operand, including
// operands whose ordinary CGAL conversion fails after producing geometry.
// The contract holds for direct kernel calls in either conversion mode.
#include "ifcgeom/hybrid_kernel.h"
#include "ifcgeom/kernels/cgal/cgal_kernel.h"
#include "ifcparse/file.h"

#include <algorithm>
#include <cmath>
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
using Kernel = kernels::cgal_kernel;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::unique_ptr<ifcopenshell::file> fixture() {
    const std::string text =
        "ISO-10303-21;\nHEADER;\n"
        "FILE_DESCRIPTION(('ViewDefinition [CoordinationView]'),'2;1');\n"
        "FILE_NAME('cgal-opening-contract.ifc','2026-10-24T00:00:00',(),(),'','','');\n"
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

taxonomy::solid::ptr outward_box(double origin, double size) {
    auto result = create_box(origin, origin, origin, size, size, size);
    // create_box's loops face inward. Supply outward-oriented boundary faces
    // so the Nef operations in this fixture represent bounded solid volumes.
    for (auto& face : result->children.front()->children) {
        for (auto& loop : face->children) {
            std::reverse(loop->children.begin(), loop->children.end());
            for (auto& edge : loop->children) {
                std::swap(edge->start, edge->end);
            }
        }
    }
    return result;
}

taxonomy::solid::ptr box(ifcopenshell::file& file, double size = 1.) {
    auto result = outward_box(0., size);
    result->instance = file.instance_by_id(102);
    return result;
}

taxonomy::solid::ptr failed_solid(ifcopenshell::file& file, bool open_shell) {
    auto result = open_shell ? box(file) : taxonomy::make<taxonomy::solid>();
    result->instance = file.instance_by_id(102);
    if (open_shell) {
        // Conversion produces a surface, but Boolean preprocessing must reject
        // its missing face rather than silently discard this cutting body.
        result->children.front()->children.pop_back();
    }
    return result;
}

std::vector<conversion_result> host(ifcopenshell::file& file) {
    settings settings;
    Kernel kernel(settings);
    auto solid = outward_box(0., 4.);
    solid->instance = file.instance_by_id(102);
    std::vector<conversion_result> result;
    require(kernel.convert_impl(solid, result), "host conversion failed");
    require(result.size() == 1 && std::abs(result.front().shape()->volume().to_double() - 64.) < 1.e-8, "host is not an outward-oriented box");
    return result;
}

double volume(const std::vector<conversion_result>& result) {
    require(result.size() == 1, "expected one host shape");
    return result.front().shape()->volume().to_double();
}

void cut_contract(bool partial, bool open_shell, bool collection) {
    auto file = fixture();
    settings settings;
    Kernel kernel(settings);
    kernel.partial_success_is_success = partial;
    auto valid = box(*file);
    auto invalid = failed_solid(*file, open_shell);
    std::vector<std::pair<taxonomy::ptr, taxonomy::matrix4>> openings;
    if (collection) {
        auto combined = taxonomy::make<taxonomy::collection>();
        combined->instance = file->instance_by_id(102);
        combined->children = {valid, invalid};
        openings.push_back({combined, taxonomy::matrix4()});
    } else {
        openings = {{valid, taxonomy::matrix4()}, {invalid, taxonomy::matrix4()}};
    }
    const auto input = host(*file);
    std::vector<conversion_result> output;
    const bool success = kernel.convert_openings(file->instance_by_id(102), openings, input, taxonomy::matrix4(), output);
    // The complete-cutter contract applies to a kernel used directly, whatever
    // conversion mode the caller runs in; the mode itself is restored.
    require(!success, "incomplete opening cutter was accepted by a direct kernel call");
    require(output.empty(), "failed opening published partial output");
    require(kernel.partial_success_is_success == partial, "convert_openings did not restore the caller's conversion mode");
    require(std::abs(volume(input) - 64.) < 1.e-8, "opening mutated its input host");
}

taxonomy::boolean_result::ptr difference(ifcopenshell::file& file, taxonomy::geom_item::ptr second) {
    auto result = taxonomy::make<taxonomy::boolean_result>();
    result->instance = file.instance_by_id(102);
    result->operation = taxonomy::boolean_result::SUBTRACTION;
    result->children = {box(file, 2.), second};
    return result;
}

void nested_cutter_contract(bool partial, bool open_shell) {
    auto file = fixture();
    settings settings;
    Kernel kernel(settings);
    kernel.partial_success_is_success = partial;
    auto cutter = difference(*file, failed_solid(*file, open_shell));
    std::vector<conversion_result> output;
    require(!kernel.convert_openings(file->instance_by_id(102), {{cutter, taxonomy::matrix4()}}, host(*file), taxonomy::matrix4(), output),
            "nested A-B lost B but was accepted as a complete cutting body");
    require(output.empty(), "failed nested cutting body published output");
    require(kernel.partial_success_is_success == partial, "convert_openings did not restore the caller's conversion mode");
}

void complete_nested_cutter() {
    auto file = fixture();
    settings settings;
    Kernel kernel(settings);
    kernel.partial_success_is_success = false;
    auto cutter = difference(*file, box(*file));
    std::vector<conversion_result> output;
    require(kernel.convert_openings(file->instance_by_id(102), {{cutter, taxonomy::matrix4()}}, host(*file), taxonomy::matrix4(), output), "complete nested cutter failed");
    require(std::abs(volume(output) - 57.) < 0.001, "nested cutter removed the uncut A volume instead of A-B");
}

void hybrid_requires_complete_cutter(bool warm_partial_cache) {
    auto file = fixture();
    settings settings;
    settings.get<settings::CacheShapes>().value = warm_partial_cache;
    auto backend = std::make_unique<Kernel>(settings);
    auto* backend_ptr = backend.get();
    auto cutter = difference(*file, failed_solid(*file, false));
    if (warm_partial_cache) {
        std::vector<conversion_result> partial;
        require(backend->abstract_kernel::convert(cutter, partial), "partial cache warmup failed");
        require(partial.size() == 1, "partial cache warmup produced no geometry");
    }
    std::vector<std::unique_ptr<kernels::abstract_kernel>> backends;
    backends.push_back(std::move(backend));
    kernels::hybrid_kernel hybrid("cgal-opening-contract", file.get(), settings, std::move(backends));
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<conversion_result> output;
        require(!hybrid.convert_openings(file->instance_by_id(102), {{cutter, taxonomy::matrix4()}}, host(*file), taxonomy::matrix4(), output), "hybrid accepted an incomplete ordinary CGAL cutter");
        require(output.empty(), "hybrid emitted a failed cutter result");
        require(backend_ptr->partial_success_is_success, "hybrid did not restore the backend policy");
    }
}

void complete_conversion_cache() {
    auto file = fixture();
    settings settings;
    settings.get<settings::CacheShapes>().value = true;
    Kernel kernel(settings);
    kernel.partial_success_is_success = false;
    auto solid = box(*file);
    std::vector<conversion_result> first, second;
    require(kernel.abstract_kernel::convert(solid, first) && kernel.abstract_kernel::convert(solid, second), "complete conversion cache failed");
    require(first.size() == 1 && second.size() == 1 && first.front().shape() == second.front().shape(), "complete conversion was not reused from the cache");
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
    int passed = 0, failed = 0;
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
    for (bool partial : {false, true}) {
        const std::string mode = partial ? "partial" : "complete";
        run(mode + "-missing-cutter", [&]() { cut_contract(partial, false, false); });
        run(mode + "-nonclosed-cutter", [&]() { cut_contract(partial, true, false); });
        run(mode + "-failed-collection-with-output", [&]() { cut_contract(partial, false, true); });
        run(mode + "-nested-missing-operand", [&]() { nested_cutter_contract(partial, false); });
        run(mode + "-nested-nonclosed-operand", [&]() { nested_cutter_contract(partial, true); });
    }
    run("complete-valid-nested-cutter", complete_nested_cutter);
    run("hybrid-complete-cutter-policy", [&]() { hybrid_requires_complete_cutter(false); });
    run("hybrid-warm-partial-cache", [&]() { hybrid_requires_complete_cutter(true); });
    run("complete-conversion-cache", complete_conversion_cache);
    std::cout << "RESULT passed=" << passed << " failed=" << failed << std::endl;
    logger.set_output(static_cast<std::ostream*>(nullptr), static_cast<std::ostream*>(nullptr));
    return failed || !passed ? 1 : 0;
}
