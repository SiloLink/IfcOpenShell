// This file was generated with the assistance of an AI coding tool.
#include "ifcgeom/hybrid_kernel.h"
#include "ifcgeom/iterator.h"
#include "ifcgeom/kernels/opencascade/opencascade_conversion_result.h"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <csignal>
#include <cstdlib>
#include <gp_Pln.hxx>
#include <iostream>
#include <sstream>
#include <stdexcept>
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
        std::vector<conversion_result>&) override {
        return run();
    }
};

std::unique_ptr<ifcopenshell::file> fixture() {
    const std::string data =
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
    hybrid_kernel kernel("hybrid-controlled", &file, settings, std::move(backends));
    if (!invoke(kernel, file, operation) || first_calls != 1 || fallback_calls != 1) {
        throw std::runtime_error("ordinary unsupported/exception fallback did not reach the next backend");
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
    auto check = [&](const char* label, std::vector<conversion_result> output, bool expected) {
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
        ++checks;
    };
    check("discarded source style", {result(10, {green})}, true);
    check("missing styles", {result(10, {})}, false);
    check("null styled face", {result(10, {nullptr})}, false);
    check("foreign style", {result(10, {foreign})}, false);
    check("wrong representation item", {result(11, {green})}, false);
    check("unknown representation item", {result(99, {green})}, false);
    check("wrong face cardinality", {result(10, {red, green})}, false);
    check("unstyled sibling", {result(10, {green}), result(11, {})}, true);

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
            const bool expected = capable && style == red;
            if (kernel.convert(item, results) != expected || results.size() != (expected ? 1 : 0)) {
                throw std::runtime_error("partial retry bypassed backend eligibility or surviving face style validation");
            }
            ++checks;
        }
    }
    return checks;
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
        std::cout << "PASS hybrid contracts: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL hybrid contracts: " << error.what() << '\n';
        return 1;
    }
}
