#include "ifcgeom/converter.h"
#include "ifcgeom/kernels/opencascade/opencascade_conversion_result.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace ifcopenshell::geom;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::unique_ptr<ifcopenshell::file> fixture(bool unmapped = false) {
    std::string data = R"IFC(ISO-10303-21;
HEADER;
FILE_DESCRIPTION((''),'2;1');
FILE_NAME('opening-recovery.ifc','',(),(),'','','');
FILE_SCHEMA(('IFC4'));
ENDSEC;
DATA;
#1=IFCCARTESIANPOINT((0.,0.,0.));
#2=IFCAXIS2PLACEMENT3D(#1,$,$);
#3=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,0.00001,#2,$);
#4=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
#5=IFCUNITASSIGNMENT((#4));
#6=IFCPROJECT('0000000000000000000001',$,$,$,$,$,$,(#3),#5);
#8=IFCCARTESIANPOINT((1.,0.,0.));
#9=IFCPOLYLINE((#1,#8));
#13=IFCLOCALPLACEMENT($,#2);
#100=IFCSHAPEREPRESENTATION(#3,'Body','Curve3D',(#9));
#110=IFCSHAPEREPRESENTATION(#3,'Body','Curve3D',(#9));
#101=IFCPRODUCTDEFINITIONSHAPE($,$,(#100,#110));
#20=IFCWALL('0000000000000000000020',$,$,$,$,#13,#101,$,.NOTDEFINED.);
#21=IFCOPENINGELEMENT('0000000000000000000021',$,$,$,$,#13,#101,$,.OPENING.);
#22=IFCOPENINGELEMENT('0000000000000000000022',$,$,$,$,#13,#101,$,.OPENING.);
#23=IFCRELVOIDSELEMENT('0000000000000000000023',$,$,$,#20,#21);
#24=IFCRELVOIDSELEMENT('0000000000000000000024',$,$,$,#20,#22);
ENDSEC;
END-ISO-10303-21;
)IFC";
    if (unmapped) {
        data.insert(data.rfind("ENDSEC;"),
                    "#31=IFCOPENINGELEMENT('0000000000000000000031',$,$,$,$,#13,$,$,.OPENING.);\n"
                    "#32=IFCRELVOIDSELEMENT('0000000000000000000032',$,$,$,#20,#31);\n");
    }
    std::istringstream stream(data);
    return std::make_unique<ifcopenshell::file>(stream, static_cast<int>(data.size()));
}

struct behavior {
    bool batch_success = false;
    bool fail_last_single = false;
    bool throw_failure = false;
    std::vector<std::size_t> calls;
};

class opening_kernel : public kernels::abstract_kernel {
    behavior& behavior_;

  public:
    opening_kernel(const ifcopenshell::geom::settings& settings, behavior& behavior)
        : abstract_kernel("opencascade", settings), behavior_(behavior) {}
    abstract_kernel* clone(ifcopenshell::logger&) const override { return new opening_kernel(*this); }
    bool supports_boolean_operations() const override { return true; }
    bool supports_face_styles() const override { return true; }
    bool convert(const taxonomy::ptr item, std::vector<conversion_result>& output) override {
        output.emplace_back(item->instance.id(), new open_cascade_shape(BRepPrimAPI_MakeBox(4., 4., 4.).Shape()));
        return true;
    }
    bool convert_openings_impl(const express::base&,
                              const std::vector<std::pair<taxonomy::ptr, taxonomy::matrix4>>& openings,
                              const std::vector<conversion_result>& input,
                              const taxonomy::matrix4&,
                              std::vector<conversion_result>& output) override {
        behavior_.calls.push_back(openings.size());
        const auto single_calls = std::count(behavior_.calls.begin(), behavior_.calls.end(), 1u);
        const bool success = openings.size() > 1 ? behavior_.batch_success
                                                : !(behavior_.fail_last_single && single_calls == 2);
        // Failed conversions deliberately leave plausible geometry in output.
        // The converter must discard it and retain the last successful input.
        const double side = success ? (openings.size() > 1 ? 2. : 3. - 0.5 * single_calls) : 100.;
        output.emplace_back(input.front().ItemId(), new open_cascade_shape(BRepPrimAPI_MakeBox(side, side, side).Shape()));
        if (!success && behavior_.throw_failure) {
            throw std::runtime_error("controlled opening failure");
        }
        return success;
    }
};

double volume(const native_element& element) {
    require(std::distance(element.geometry().begin(), element.geometry().end()) == 1, "unexpected host shape count");
    auto shape = std::dynamic_pointer_cast<open_cascade_shape>(element.geometry().begin()->shape());
    require(bool(shape), "missing host geometry");
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape->shape(), properties);
    return properties.Mass();
}

void check_case(const std::string& mode, bool batch_success, bool fail_last, bool throws, bool unmapped,
                int representation = 100) {
    auto file = fixture(unmapped);
    settings settings;
    settings.set("opening-recovery-mode", mode);
    settings.get<settings::OutputDimensionality>().value = settings::CURVES_SURFACES_AND_SOLIDS;
    behavior behavior{batch_success, fail_last, throws, {}};
    ifcopenshell::logger logger;
    std::ostringstream log;
    logger.set_output(static_cast<std::ostream*>(nullptr), &log);
    converter converter(std::make_unique<opening_kernel>(settings, behavior), file.get(), settings, logger);
    std::unique_ptr<native_element> result(converter.create_brep_for_representation_and_product(
        file->instance_by_id(representation), file->instance_by_id(20)));
    require(bool(result), "opening conversion lost the host");

    const bool pending = mode == "defer" && !batch_success;
    const bool singles = mode == "individual" || (mode == "inline" && !batch_success);
    const auto expected_calls = mode == "individual" ? std::vector<std::size_t>{1, 1}
                                                    : singles ? std::vector<std::size_t>{2, 1, 1}
                                                              : std::vector<std::size_t>{2};
    require(behavior.calls == expected_calls, "wrong batch/individual call sequence");
    const double expected_volume = pending ? 64. : singles && fail_last ? 15.625 : 8.;
    require(std::abs(volume(*result) - expected_volume) < 1.e-8, "failed conversion contaminated retained host");
    require(result->deferred_opening_ids() == (pending ? std::vector<int>{21, 22} : std::vector<int>{}),
            "wrong pending opening ids");
    require(result->deferred_opening_representation_id() == (pending ? representation : 0),
            "pending representation identity lost");
    const auto diagnostics = log.str();
    require((diagnostics.find("\"category\":\"opening_incomplete\"") != std::string::npos) ==
                (unmapped || (singles && fail_last)),
            "pending opening was finalized, or final partial diagnostic was lost");
    require((diagnostics.find("\"category\":\"fallback_recovered\"") != std::string::npos) ==
                (singles && !fail_last && !unmapped),
            "incorrect recovered diagnostic");
    if (unmapped) {
        require(diagnostics.find("\"action\":\"skip_unmapped_cut\"") != std::string::npos,
                "mapping failure lost its final diagnostic");
    }
    if (pending) {
        native_element copied(*result);
        triangulation_element mesh(*result);
        triangulation_element reused(mesh, mesh.geometry_pointer());
        serialized_element serialized(*result);
        for (const element* converted : std::vector<const element*>{&copied, &mesh, &reused, &serialized}) {
            require(converted->deferred_opening_ids() == result->deferred_opening_ids(), "element copy lost pending ids");
            require(converted->deferred_opening_representation_id() == representation, "element copy lost representation");
        }
    }
}

void invalid_policy_is_rejected() {
    auto file = fixture();
    settings settings;
    require(settings.get<settings::OpeningRecoveryMode>().get() == "inline", "native default policy changed");
    settings.set("opening-recovery-mode", std::string("unknown"));
    behavior behavior;
    try {
        converter converter(std::make_unique<opening_kernel>(settings, behavior), file.get(), settings);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("invalid recovery policy was silently accepted");
}
} // namespace

int main() {
    if (const char* plugins = std::getenv("IFCGEOM_CONTRACT_PLUGIN_DIR")) {
        ifcopenshell::plugin::set_search_paths({plugins});
    }
    try {
        int checks = 0;
        for (const std::string mode : {"inline", "defer", "individual"}) {
            for (bool throws : {false, true}) {
                check_case(mode, false, false, throws, false);
                check_case(mode, false, true, throws, false);
                checks += 2;
            }
            check_case(mode, true, false, false, false);
            check_case(mode, false, false, false, true);
            checks += 2;
        }
        check_case("defer", false, false, false, false, 110);
        invalid_policy_is_rejected();
        std::cout << "deferred opening contracts passed: " << checks + 2 << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
