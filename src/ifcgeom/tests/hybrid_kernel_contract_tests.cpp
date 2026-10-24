// This file was generated with the assistance of an AI coding tool.
#include "ifcgeom/converter.h"
#include "ifcgeom/hybrid_kernel.h"
#include "ifcgeom/iterator.h"
#include "ifcgeom/kernels/opencascade/opencascade_conversion_result.h"

#include <boost/property_tree/json_parser.hpp>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <gp_Pln.hxx>
#include <GProp_GProps.hxx>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <TopoDS_Compound.hxx>

#if defined(__unix__)
#include <cerrno>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace ifcopenshell::geom;
using kernels::abstract_kernel;
using kernels::hybrid_kernel;

enum class Operation {
    convert,
    layerset,
    folded_layerset,
    openings
};
enum class Outcome {
    unsupported,
    exception,
    success,
    fatal_signal
};

class ControlledKernel : public abstract_kernel {
    Outcome outcome_;
    int& calls_;
    int signal_;
    std::vector<conversion_result> results_;

    bool run() {
        ++calls_;
        if (outcome_ == Outcome::fatal_signal) {
            std::raise(signal_);
            throw std::runtime_error("fatal signal unexpectedly returned");
        }
        if (outcome_ == Outcome::exception) {
            throw std::runtime_error("ordinary geometry conversion failure");
        }
        return outcome_ == Outcome::success;
    }

  public:
    ControlledKernel(
        const ifcopenshell::geom::settings& settings,
        Outcome outcome,
        int& calls,
        int signal = 0,
        std::vector<conversion_result> results = {})
        : abstract_kernel("controlled", settings), outcome_(outcome), calls_(calls), signal_(signal), results_(std::move(results)) {}

    bool supports_boolean_operations() const override { return true; }
    bool supports_face_styles() const override { return true; }
    abstract_kernel* clone(ifcopenshell::logger&) const override { return new ControlledKernel(*this); }
    bool convert(const taxonomy::ptr, std::vector<conversion_result>& results) override {
        if (!run()) {
            return false;
        }
        results.insert(results.end(), results_.begin(), results_.end());
        return true;
    }
    bool apply_layerset(std::vector<conversion_result>&, const layerset_information&) override { return run(); }
    bool apply_folded_layerset(
        std::vector<conversion_result>&,
        const layerset_information&,
        const std::map<express::base, layerset_information>&) override {
        return run();
    }
    bool convert_openings(
        const express::base&,
        const std::vector<std::pair<taxonomy::ptr, taxonomy::matrix4>>&,
        const std::vector<conversion_result>&,
        const taxonomy::matrix4&,
        std::vector<conversion_result>& output) override {
        if (partial_success_is_success) {
            throw std::runtime_error("opening conversion allowed an incomplete cutter");
        }
        output.insert(output.end(), results_.begin(), results_.end());
        return run();
    }
};

std::unique_ptr<ifcopenshell::file> fixture(bool openings = false) {
    std::string data =
        "ISO-10303-21;\nHEADER;\n"
        "FILE_DESCRIPTION(('ViewDefinition [CoordinationView]'),'2;1');\n"
        "FILE_NAME('hybrid-contract.ifc','2026-10-20T00:00:00',(),(),'','','');\n"
        "FILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n"
        "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
        "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
        "#3=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,0.00001,#2,$);\n"
        "#4=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);\n"
        "#5=IFCUNITASSIGNMENT((#4));\n"
        "#6=IFCPROJECT('0000000000000000000001',$,$,$,$,$,$,(#3),#5);\n"
        "#10=IFCWALL('0000000000000000000002',$,$,$,$,$,$,$,.NOTDEFINED.);\n"
        "#11=IFCWALL('0000000000000000000003',$,$,$,$,$,$,$,.NOTDEFINED.);\n"
        "ENDSEC;\nEND-ISO-10303-21;\n";
    if (openings) {
        const std::string extra =
            "#8=IFCCARTESIANPOINT((1.,0.,0.));\n"
            "#9=IFCPOLYLINE((#1,#8));\n"
            "#13=IFCLOCALPLACEMENT($,#2);\n"
            "#100=IFCSHAPEREPRESENTATION(#3,'Body','Curve3D',(#9));\n"
            "#101=IFCPRODUCTDEFINITIONSHAPE($,$,(#100));\n"
            "#20=IFCWALL('0000000000000000000020',$,$,$,$,#13,#101,$,.NOTDEFINED.);\n"
            "#21=IFCOPENINGELEMENT('0000000000000000000021',$,$,$,$,#13,#101,$,.OPENING.);\n"
            "#22=IFCOPENINGELEMENT('0000000000000000000022',$,$,$,$,#13,#101,$,.OPENING.);\n"
            "#23=IFCRELVOIDSELEMENT('0000000000000000000023',$,$,$,#20,#21);\n"
            "#24=IFCRELVOIDSELEMENT('0000000000000000000024',$,$,$,#20,#22);\n";
        data.insert(data.rfind("ENDSEC;"), extra);
    }
    std::istringstream input(data);
    return std::make_unique<ifcopenshell::file>(input, static_cast<int>(data.size()));
}

bool invoke(hybrid_kernel& kernel, ifcopenshell::file& file, Operation operation) {
    auto item = taxonomy::make<taxonomy::collection>();
    item->instance = file.instance_by_id(10);
    std::vector<conversion_result> results;
    layerset_information layers;
    switch (operation) {
    case Operation::convert:
        return kernel.convert(item, results);
    case Operation::layerset:
        return kernel.apply_layerset(results, layers);
    case Operation::folded_layerset:
        return kernel.apply_folded_layerset(results, layers, {});
    case Operation::openings:
        return kernel.convert_openings(item->instance, {}, {}, taxonomy::matrix4(), results);
    }
    throw std::runtime_error("unknown operation");
}

void ordinary_fallback(ifcopenshell::file& file, Operation operation, Outcome first_outcome) {
    settings settings;
    int first_calls = 0;
    int fallback_calls = 0;
    std::vector<std::unique_ptr<abstract_kernel>> backends;
    backends.emplace_back(std::make_unique<ControlledKernel>(settings, first_outcome, first_calls));
    backends.emplace_back(std::make_unique<ControlledKernel>(settings, Outcome::success, fallback_calls));
    auto* first = backends.front().get();
    auto* fallback = backends.back().get();
    hybrid_kernel kernel("hybrid-controlled", &file, settings, std::move(backends));
    if (!invoke(kernel, file, operation) || first_calls != 1 || fallback_calls != 1) {
        throw std::runtime_error("ordinary unsupported/exception fallback did not reach the next backend");
    }
    if (!first->partial_success_is_success || !fallback->partial_success_is_success) {
        throw std::runtime_error("fallback did not restore backend partial-conversion policy");
    }
}

class SettingsKernel : public ControlledKernel {
    std::vector<std::pair<double, double>>& observed_;

  public:
    SettingsKernel(const ifcopenshell::geom::settings& settings,
                   Outcome outcome,
                   int& calls,
                   std::vector<std::pair<double, double>>& observed)
        : ControlledKernel(settings, outcome, calls), observed_(observed) {}

    abstract_kernel* clone(ifcopenshell::logger&) const override { return new SettingsKernel(*this); }
    bool convert(const taxonomy::ptr item, std::vector<conversion_result>& results) override {
        observed_.emplace_back(
            settings().get<ifcopenshell::geom::settings::Precision>().get(),
            settings().get<ifcopenshell::geom::settings::LengthUnit>().get());
        return ControlledKernel::convert(item, results);
    }
};

void updated_settings_reach_backends_and_clones(ifcopenshell::file& file) {
    settings initial;
    settings updated;
    updated.get<settings::Precision>().value = 1.e-7;
    updated.get<settings::LengthUnit>().value = 0.001;
    int first_calls = 0;
    int fallback_calls = 0;
    std::vector<std::pair<double, double>> observed;
    std::vector<std::unique_ptr<abstract_kernel>> backends;
    backends.emplace_back(std::make_unique<SettingsKernel>(initial, Outcome::unsupported, first_calls, observed));
    backends.emplace_back(std::make_unique<SettingsKernel>(initial, Outcome::success, fallback_calls, observed));
    std::unique_ptr<abstract_kernel> kernel =
        std::make_unique<hybrid_kernel>("hybrid-controlled", &file, initial, std::move(backends));
    kernel->apply_settings(updated);
    auto item = taxonomy::make<taxonomy::collection>();
    item->instance = file.instance_by_id(10);
    std::vector<conversion_result> results;
    if (!kernel->convert(item, results)) {
        throw std::runtime_error("settings probe did not reach fallback backend");
    }
    std::unique_ptr<abstract_kernel> clone(kernel->clone(ifcopenshell::logger::root()));
    if (!clone->convert(item, results) || observed != std::vector<std::pair<double, double>>(4, {1.e-7, 0.001})) {
        throw std::runtime_error("mapped precision and units did not reach all hybrid backends and clones");
    }
}

class RetryKernel : public ControlledKernel {
    bool& complete_;
    std::vector<conversion_result> output_;
    bool cached_ = false;
    bool face_styles_;

  public:
    RetryKernel(const ifcopenshell::geom::settings& settings,
                bool& complete,
                int& calls,
                std::vector<conversion_result> output,
                bool face_styles = true)
        : ControlledKernel(settings, Outcome::success, calls), complete_(complete), output_(std::move(output)), face_styles_(face_styles) {}

    abstract_kernel* clone(ifcopenshell::logger&) const override {
        auto copy = new RetryKernel(*this);
        copy->cached_ = false;
        return copy;
    }
    bool supports_face_styles() const override { return face_styles_; }
    bool convert(const taxonomy::ptr, std::vector<conversion_result>& results) override {
        results.insert(results.end(), output_.begin(), output_.end());
        bool success = cached_ || complete_ || partial_success_is_success;
        if (success && settings().get<ifcopenshell::geom::settings::CacheShapes>().get()) {
            cached_ = true;
        }
        return success;
    }
};

class FragmentKernel : public ControlledKernel {
  public:
    using ControlledKernel::ControlledKernel;
    abstract_kernel* clone(ifcopenshell::logger&) const override { return new FragmentKernel(*this); }
    bool convert(const taxonomy::ptr item, std::vector<conversion_result>& results) override {
        return abstract_kernel::convert(item, results);
    }
    bool convert_impl(const taxonomy::shell::ptr shell, std::vector<conversion_result>& results) override {
        const int fragment = static_cast<int>(shell->matrix->ccomponents()(0, 3));
        if (fragment == 1) {
            return false;
        }
        const TopoDS_Shape face = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 0., 1., 0., 1.).Face();
        results.emplace_back(fragment, new open_cascade_shape(face));
        return true;
    }
};

int partial_retry_contracts(ifcopenshell::file& file) {
    settings settings;
    settings.get<settings::CacheShapes>().value = true;
    auto item = taxonomy::make<taxonomy::collection>();
    item->instance = file.instance_by_id(10);
    const TopoDS_Shape face = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 0., 1., 0., 1.).Face();
    auto output = [&](int id) { return std::vector<conversion_result>{conversion_result(id, new open_cascade_shape(face))}; };
    bool first_complete = false;
    bool second_complete = true;
    int calls = 0;
    auto backends = [&]() {
        std::vector<std::unique_ptr<abstract_kernel>> result;
        result.emplace_back(std::make_unique<RetryKernel>(settings, first_complete, calls, output(10)));
        result.emplace_back(std::make_unique<RetryKernel>(settings, second_complete, calls, output(11)));
        result.emplace_back(std::make_unique<ControlledKernel>(settings, Outcome::unsupported, calls));
        return result;
    };
    hybrid_kernel kernel("hybrid-controlled", &file, settings, backends());
    auto results = output(99);
    if (!kernel.convert(item, results) || results.size() != 2 || results[0].ItemId() != 99 || results[1].ItemId() != 11) {
        throw std::runtime_error("complete conversion did not take priority, or failed output leaked");
    }
    if (results[1].diagnostics().size() != 1 || results[1].diagnostics()[0].at("category") != "fallback_recovered") {
        throw std::runtime_error("complete fallback was recorded as a final loss");
    }

    second_complete = false;
    hybrid_kernel retry("hybrid-controlled", &file, settings, backends());
    results.clear();
    if (!retry.convert(item, results) || results.size() != 1 || results[0].ItemId() != 10) {
        throw std::runtime_error("partial retry did not retain backend order after all complete attempts failed");
    }
    second_complete = true;
    results.clear();
    if (!retry.convert(item, results) || results.size() != 1 || results[0].ItemId() != 11) {
        throw std::runtime_error("partial retry polluted a later complete conversion through its cache");
    }

    settings.get<settings::CacheShapes>().value = false;
    auto child = taxonomy::make<taxonomy::collection>();
    child->instance = file.instance_by_id(10);
    child->matrix = taxonomy::make<taxonomy::matrix4>();
    child->matrix->components()(0, 3) = 4.;
    for (int i = 0; i != 3; ++i) {
        auto fragment = taxonomy::make<taxonomy::shell>();
        fragment->instance = file.instance_by_id(10);
        fragment->matrix = taxonomy::make<taxonomy::matrix4>();
        fragment->matrix->components()(0, 3) = double(i);
        child->children.push_back(fragment);
    }
    item->children = {child};
    item->matrix = taxonomy::make<taxonomy::matrix4>();
    item->matrix->components()(2, 3) = 6.;
    auto style = taxonomy::make<taxonomy::style>("collection");
    item->surface_style = style;
    std::vector<std::unique_ptr<abstract_kernel>> fragments;
    fragments.emplace_back(std::make_unique<FragmentKernel>(settings, Outcome::success, calls));
    fragments.emplace_back(std::make_unique<ControlledKernel>(settings, Outcome::unsupported, calls));
    hybrid_kernel transformed("hybrid-controlled", &file, settings, std::move(fragments));
    results.clear();
    if (!transformed.convert(item, results) || results.size() != 2 || results[0].ItemId() != 0 || results[1].ItemId() != 2) {
        throw std::runtime_error("partial retry did not restart the complete collection after a failed fragment");
    }
    for (auto& result : results) {
        Eigen::Matrix4d expected = Eigen::Matrix4d::Identity();
        expected(0, 3) = 4.;
        expected(2, 3) = 6.;
        if (!result.placement()->ccomponents().isApprox(expected) || result.style_ptr() != style) {
            throw std::runtime_error("partial retry lost or duplicated collection transforms or inherited style");
        }
    }
    return 4;
}

int face_style_contracts(ifcopenshell::file& file) {
    auto red = taxonomy::make<taxonomy::style>("red");
    auto green = taxonomy::make<taxonomy::style>("green");
    auto foreign = taxonomy::make<taxonomy::style>("foreign");
    auto item = taxonomy::make<taxonomy::collection>();
    item->instance = file.instance_by_id(10);
    auto styled = taxonomy::make<taxonomy::shell>();
    styled->instance = file.instance_by_id(10);
    for (const auto& style : {red, green}) {
        auto face = taxonomy::make<taxonomy::face>();
        face->surface_style = style;
        styled->children.push_back(face);
    }
    auto unstyled = taxonomy::make<taxonomy::shell>();
    unstyled->instance = file.instance_by_id(11);
    unstyled->children.push_back(taxonomy::make<taxonomy::face>());
    item->children = {styled, unstyled};

    const TopoDS_Shape face = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 0., 1., 0., 1.).Face();
    auto result = [&](int id, std::vector<taxonomy::style::ptr> styles) {
        return conversion_result(id, new open_cascade_shape(face, std::move(styles)));
    };
    int checks = 0;
    auto check = [&](const char* label, std::vector<conversion_result> output, bool expected, bool fallback = false) {
        settings settings;
        int calls = 0;
        std::vector<std::unique_ptr<abstract_kernel>> backends;
        backends.emplace_back(
            std::make_unique<ControlledKernel>(settings, Outcome::success, calls, 0, std::move(output)));
        hybrid_kernel kernel("hybrid-controlled", &file, settings, std::move(backends));
        std::vector<conversion_result> results;
        if (kernel.convert(item, results) != expected || (!expected && !results.empty())) {
            throw std::runtime_error(std::string("face style contract failed: ") + label);
        }
        if (fallback) {
            for (const auto& r : results) {
                if (!r.shape()->face_styles().empty() || r.shape()->num_faces() != 1 ||
                    !std::static_pointer_cast<open_cascade_shape>(r.shape())->shape().IsSame(face)) {
                    throw std::runtime_error("material fallback changed topology or retained untrusted styles");
                }
            }
        }
        ++checks;
    };
    check("discarded source style", {result(10, {green})}, true);
    check("missing styles", {result(10, {})}, true, true);
    check("null styled face", {result(10, {nullptr})}, true, true);
    check("foreign style", {result(10, {foreign})}, true, true);
    check("wrong representation item", {result(11, {green})}, true, true);
    check("unknown representation item", {result(99, {green})}, false);
    check("wrong face cardinality", {result(10, {red, green})}, true, true);
    check("unstyled sibling", {result(10, {green}), result(11, {})}, true);

    {
        settings settings;
        int calls = 0;
        std::vector<std::unique_ptr<abstract_kernel>> backends;
        backends.emplace_back(std::make_unique<ControlledKernel>(
            settings, Outcome::success, calls, 0, std::vector<conversion_result>{result(10, {foreign})}));
        backends.emplace_back(std::make_unique<ControlledKernel>(
            settings, Outcome::success, calls, 0, std::vector<conversion_result>{result(10, {green})}));
        hybrid_kernel kernel("hybrid-controlled", &file, settings, std::move(backends));
        std::vector<conversion_result> results;
        if (!kernel.convert(item, results) || results.size() != 1 ||
            results.front().shape()->face_styles() != std::vector<taxonomy::style::ptr>{green}) {
            throw std::runtime_error("complete material mapping did not take priority over fallback");
        }
        ++checks;
    }

    {
        open_cascade_shape degraded(face, {}, false, false);
        std::unique_ptr<conversion_result_shape> clone(degraded.clone());
        std::unique_ptr<conversion_result_shape> compound(degraded.wrap_in_compound());
        std::unique_ptr<conversion_result_shape> moved(degraded.moved(taxonomy::make<taxonomy::matrix4>()));
        if (clone->face_styles_complete() || compound->face_styles_complete() || moved->face_styles_complete()) {
            throw std::runtime_error("shape transforms lost incomplete material diagnostics");
        }
        ++checks;
    }

    styled->children[1]->surface_style = nullptr;
    TopoDS_Compound two_faces;
    BRep_Builder builder;
    builder.MakeCompound(two_faces);
    builder.Add(two_faces, face);
    builder.Add(two_faces, BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 2., 3., 0., 1.).Face());
    check("unstyled surviving face",
          {conversion_result(10, new open_cascade_shape(two_faces, {red, nullptr}))},
          true);

    bool complete = false;
    for (bool capable : {true, false}) {
        for (const auto& style : {red, foreign}) {
            settings settings;
            int calls = 0;
            std::vector<std::unique_ptr<abstract_kernel>> backends;
            backends.emplace_back(std::make_unique<RetryKernel>(
                settings, complete, calls, std::vector<conversion_result>{result(10, {style})}, capable));
            hybrid_kernel kernel("hybrid-controlled", &file, settings, std::move(backends));
            std::vector<conversion_result> results;
            const bool expected = true;
            if (kernel.convert(item, results) != expected || results.size() != (expected ? 1 : 0)) {
                throw std::runtime_error("partial retry did not preserve geometry with material fallback");
            }
            std::set<std::string> categories;
            for (const auto& event : results.front().diagnostics()) {
                categories.insert(event.at("category"));
            }
            if (!categories.count("geometry_partial") || (style == foreign && !categories.count("material_degraded"))) {
                throw std::runtime_error("combined partial geometry/material degradation lost a diagnostic");
            }
            ++checks;
        }
    }
    return checks;
}

void opening_fallback_discards_failed_output(ifcopenshell::file& file) {
    settings settings;
    int calls = 0;
    auto face = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 0., 1., 0., 1.).Face();
    std::vector<std::unique_ptr<abstract_kernel>> backends;
    for (auto outcome : {Outcome::unsupported, Outcome::exception, Outcome::success}) {
        backends.emplace_back(std::make_unique<ControlledKernel>(settings, outcome, calls, 0, std::vector<conversion_result>{conversion_result(outcome == Outcome::success ? 10 : 99, new open_cascade_shape(face))}));
    }
    hybrid_kernel kernel("hybrid-controlled", &file, settings, std::move(backends));
    std::vector<conversion_result> output;
    if (!kernel.convert_openings(file.instance_by_id(10), {}, {}, taxonomy::matrix4(), output) ||
        output.size() != 1 || output[0].ItemId() != 10) {
        throw std::runtime_error("failed opening backend leaked its output");
    }
}

void opening_partial_contract(bool complete_batch, bool all_fail, bool throws, bool all_recover = false) {
    class OpeningKernel : public ControlledKernel {
        bool complete_batch_, all_fail_, throws_, all_recover_;
        int& cuts_;

      public:
        OpeningKernel(const ifcopenshell::geom::settings& s, int& calls, int& cuts, bool complete_batch, bool all_fail, bool throws, bool all_recover)
            : ControlledKernel(s, Outcome::success, calls), complete_batch_(complete_batch), all_fail_(all_fail), throws_(throws), all_recover_(all_recover), cuts_(cuts) {}
        bool convert(const taxonomy::ptr item, std::vector<conversion_result>& output) override {
            output.emplace_back(item->instance.id(), new open_cascade_shape(BRepPrimAPI_MakeBox(4., 4., 4.).Shape()));
            return true;
        }
        bool convert_openings(const express::base&, const std::vector<std::pair<taxonomy::ptr, taxonomy::matrix4>>& ops, const std::vector<conversion_result>& input, const taxonomy::matrix4&, std::vector<conversion_result>& output) override {
            ++cuts_;
            const bool success = complete_batch_ || (all_recover_ && ops.size() == 1) || (!all_fail_ && cuts_ == 2);
            const double side = success ? (complete_batch_ ? 2. : 3.) : 100.;
            output.emplace_back(input.front().ItemId(), new open_cascade_shape(BRepPrimAPI_MakeBox(side, side, side).Shape()));
            if (!success && throws_) {
                throw std::runtime_error("injected cut failure");
            }
            return success;
        }
    };
    auto file = fixture(true);
    settings settings;
    settings.get<settings::OutputDimensionality>().value = settings::CURVES_SURFACES_AND_SOLIDS;
    int calls = 0, cuts = 0;
    ifcopenshell::logger diagnostic_logger;
    std::ostringstream diagnostic_log;
    diagnostic_logger.set_output(static_cast<std::ostream*>(nullptr), &diagnostic_log);
    converter converter(std::make_unique<OpeningKernel>(settings, calls, cuts, complete_batch, all_fail, throws, all_recover), file.get(), settings, diagnostic_logger);
    std::unique_ptr<native_element> result(converter.create_brep_for_representation_and_product(file->instance_by_id(100), file->instance_by_id(20)));
    if (!result || std::distance(result->geometry().begin(), result->geometry().end()) != 1) {
        throw std::runtime_error("failed cut lost its valid host");
    }
    auto shape = std::dynamic_pointer_cast<open_cascade_shape>(result->geometry().begin()->shape());
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape->shape(), props);
    const double volume = complete_batch ? 8. : (all_fail ? 64. : 27.);
    if (std::abs(props.Mass() - volume) > 1.e-8 || cuts != (complete_batch ? 1 : 3)) {
        throw std::runtime_error("opening retry mismatch: volume=" + std::to_string(props.Mass()) + " cuts=" + std::to_string(cuts));
    }
    if (all_recover) {
        if (diagnostic_log.str().find("\"category\":\"opening_incomplete\"") != std::string::npos ||
            diagnostic_log.str().find("\"category\":\"fallback_recovered\"") == std::string::npos) {
            throw std::runtime_error("all recovered single cuts did not produce a recovered summary");
        }
    } else if (!complete_batch) {
        if (diagnostic_log.str().find("\"category\":\"opening_incomplete\"") == std::string::npos) {
            throw std::runtime_error("final incomplete opening did not retain a diagnostic");
        }
    }
}

#if defined(__unix__)
void fatal_fault_is_contained(ifcopenshell::file& file, Operation operation, int fatal_signal) {
    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        const rlimit no_core{0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        std::signal(SIGSEGV, SIG_DFL);
        std::signal(SIGBUS, SIG_DFL);
        alarm(5);
        settings settings;
        int first_calls = 0;
        int fallback_calls = 0;
        std::vector<std::unique_ptr<abstract_kernel>> backends;
        backends.emplace_back(
            std::make_unique<ControlledKernel>(settings, Outcome::fatal_signal, first_calls, fatal_signal));
        backends.emplace_back(std::make_unique<ControlledKernel>(settings, Outcome::success, fallback_calls));
        hybrid_kernel kernel("hybrid-controlled", &file, settings, std::move(backends));
        invoke(kernel, file, operation);
        _exit(0);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFSIGNALED(status) || WTERMSIG(status) != fatal_signal) {
        throw std::runtime_error("fatal native fault was swallowed, retried, or exceeded its deadline");
    }
}
#endif

void concurrent_diagnostic_events() {
    ifcopenshell::logger logger;
    std::ostringstream output;
    logger.set_output(static_cast<std::ostream*>(nullptr), &output);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker]() {
            for (int i = 0; i < 20; ++i) {
                logger.diagnostic(ifcopenshell::logger::LOG_WARNING,
                                  {{"category", "geometry_partial"}, {"entity_id", std::to_string(worker * 20 + i)}, {"reason", "quoted \"reason\"\nsecond line"}});
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    std::istringstream lines(output.str());
    std::string line;
    std::set<int> identities;
    while (std::getline(lines, line)) {
        const auto position = line.find("PARSER_EVENT_V1 ");
        if (position == std::string::npos) {
            throw std::runtime_error("diagnostic lines were interleaved or split");
        }
        std::istringstream encoded(line.substr(position + 16));
        boost::property_tree::ptree event;
        boost::property_tree::read_json(encoded, event);
        if (event.get<std::string>("reason") != "quoted \"reason\"\nsecond line") {
            throw std::runtime_error("diagnostic reason did not survive JSON escaping");
        }
        identities.insert(event.get<int>("entity_id"));
    }
    if (identities.size() != 80) {
        throw std::runtime_error("concurrent diagnostics lost or merged identities");
    }
}
} // namespace

int main() {
    if (const char* plugins = std::getenv("IFCGEOM_CONTRACT_PLUGIN_DIR")) {
        ifcopenshell::plugin::set_search_paths({plugins});
    }
    try {
        auto file = fixture();
        int checks = 0;
        for (auto operation :
             {Operation::convert, Operation::layerset, Operation::folded_layerset, Operation::openings}) {
            for (auto outcome : {Outcome::unsupported, Outcome::exception}) {
                ordinary_fallback(*file, operation, outcome);
                ++checks;
            }
#if defined(__unix__)
            for (int fatal_signal : {SIGSEGV, SIGBUS}) {
                fatal_fault_is_contained(*file, operation, fatal_signal);
                ++checks;
            }
#endif
        }
        updated_settings_reach_backends_and_clones(*file);
        ++checks;
        checks += partial_retry_contracts(*file);
        checks += face_style_contracts(*file);
        opening_fallback_discards_failed_output(*file);
        ++checks;
        for (bool throws : {false, true}) {
            opening_partial_contract(false, false, throws);
            opening_partial_contract(false, true, throws);
            checks += 2;
        }
        opening_partial_contract(true, false, false);
        ++checks;
        opening_partial_contract(false, false, false, true);
        ++checks;
        concurrent_diagnostic_events();
        ++checks;
        std::cout << "PASS hybrid contracts: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL hybrid contracts: " << error.what() << '\n';
        return 1;
    }
}
