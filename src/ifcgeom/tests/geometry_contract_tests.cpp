#include "ifcgeom/iterator.h"
#include "ifcgeom/kernels/opencascade/opencascade_conversion_result.h"
#include "ifcgeom/kernels/opencascade/opencascade_kernel.h"
#include "ifcgeom/kernels/opencascade/shape_healing_context.h"
#include "ifcparse/instance_data.h"

#include <atomic>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Vertex.hxx>

#if defined(__linux__)
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>

// Interpose only in this test executable. A positive counter fails exactly one
// thread creation, including failures after other workers have started.
static std::atomic<int> fail_thread_after{0};
static std::atomic<int> injected_thread_failures{0};
struct ScopedThreadFailure {
    explicit ScopedThreadFailure(int after) { fail_thread_after = after; }
    ~ScopedThreadFailure() { fail_thread_after = 0; }
};
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*start)(void*), void* argument) {
    using create_fn = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static auto real_create = reinterpret_cast<create_fn>(dlsym(RTLD_NEXT, "pthread_create"));
    auto remaining = fail_thread_after.load();
    while (remaining > 0) {
        if (fail_thread_after.compare_exchange_weak(remaining, remaining - 1)) {
            if (remaining == 1) {
                ++injected_thread_failures;
                return EAGAIN;
            }
            break;
        }
    }
    return real_create(thread, attributes, start, argument);
}
#endif

namespace {
using namespace ifcopenshell::geom;
using namespace ifcopenshell::geom::settings_detail;
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

void concurrent_entity_serialization() {
    // Deliberately avoid constructing a file or serializing on the caller:
    // the first entity serialization must be safe on concurrent workers too.
    const auto* schema = ifcopenshell::schema_by_name("IFC4");
    auto point_data = std::make_shared<ifcopenshell::instance_data>(nullptr,
        schema->declaration_by_name("IfcCartesianPoint"), 42, ifcopenshell::in_memory_attribute_storage(1));
#ifdef IFOPSH_SAFE_INSTANCE
    express::base point(point_data);
#else
    express::base point(point_data.get());
#endif
    point_data->set_attribute_value(0, std::vector<double>{1., 2., 3.});
    auto line_data = std::make_shared<ifcopenshell::instance_data>(nullptr,
        schema->declaration_by_name("IfcPolyline"), 43, ifcopenshell::in_memory_attribute_storage(1));
#ifdef IFOPSH_SAFE_INSTANCE
    express::base line(line_data);
#else
    express::base line(line_data.get());
#endif
    line_data->set_attribute_value(0, std::vector<express::base>{point});
    constexpr int threads = 8;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::future<void>> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back(std::async(std::launch::async, [&]() {
            ++ready;
            while (!start) {
                std::this_thread::yield();
            }
            for (int j = 0; j < 200; ++j) {
                std::ostringstream point_text, line_text;
                point.to_string(point_text, true);
                line.to_string(line_text, true);
                require(point_text.str() == "#42=IFCCARTESIANPOINT((1.,2.,3.))", "concurrent point serialization changed");
                require(line_text.str() == "#43=IFCPOLYLINE((#42))", "concurrent entity reference serialization changed");
            }
        }));
    }
    while (ready != threads) {
        std::this_thread::yield();
    }
    start = true;
    for (auto& worker : workers) {
        worker.get();
    }
}

void header_and_value_serialization() {
    const auto* header = ifcopenshell::schema_by_name("HEADER_SECTION_SCHEMA");
    auto header_data = std::make_shared<ifcopenshell::instance_data>(nullptr,
        header->declaration_by_name("file_schema"), 7, ifcopenshell::in_memory_attribute_storage(1));
    const auto* schema = ifcopenshell::schema_by_name("IFC4");
    auto label_data = std::make_shared<ifcopenshell::instance_data>(nullptr,
        schema->declaration_by_name("IfcLabel"), 8, ifcopenshell::in_memory_attribute_storage(1));
#ifdef IFOPSH_SAFE_INSTANCE
    express::base header_instance(header_data), label(label_data);
#else
    express::base header_instance(header_data.get()), label(label_data.get());
#endif
    header_data->set_attribute_value(0, std::vector<std::string>{"IFC4"});
    label_data->set_attribute_value(0, std::string("label"));
    std::ostringstream header_text, label_text;
    header_instance.to_string(header_text, true);
    label.to_string(label_text, true);
    require(header_text.str() == "FILE_SCHEMA(('IFC4'))", "header serialization acquired an instance id");
    require(label_text.str() == "IFCLABEL('label')", "value serialization acquired an instance id");
}

void concurrent_loggers(ifcopenshell::logger::format format) {
    constexpr int threads = 4;
    constexpr int messages = 400;
    std::array<ifcopenshell::logger, threads> loggers;
    std::array<std::ostringstream, threads> output;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::future<void>> workers;
    for (int i = 0; i < threads; ++i) {
        loggers[i].output_format(format);
        loggers[i].set_output(static_cast<std::ostream*>(nullptr), &output[i]);
        workers.emplace_back(std::async(std::launch::async, [&, i]() {
            ++ready;
            while (!start) {
                std::this_thread::yield();
            }
            for (int j = 0; j < messages; ++j) {
                loggers[i].error("GEO", 900, "parallel-log-" + std::to_string(i) + "-" + std::to_string(j));
            }
        }));
    }
    while (ready != threads) {
        std::this_thread::yield();
    }
    start = true;
    for (auto& worker : workers) {
        worker.get();
    }
    auto check_timestamp = [](const std::string& timestamp) {
        std::tm parsed{};
        std::istringstream input(timestamp);
        input >> std::get_time(&parsed, "%Y-%m-%d %H:%M:%S");
        require(timestamp.size() == 19 && !input.fail(), "invalid concurrent log timestamp");
    };
    for (int i = 0; i < threads; ++i) {
        if (format == ifcopenshell::logger::FMT_INMEMORY) {
            const auto& records = loggers[i].log_messages();
            require(records.size() == messages, "concurrent logger lost messages");
            for (int j = 0; j < messages; ++j) {
                require(records[j].message == "parallel-log-" + std::to_string(i) + "-" + std::to_string(j), "concurrent logger mixed messages");
                check_timestamp(records[j].timestamp);
            }
        } else {
            std::istringstream lines(output[i].str());
            std::string line;
            const std::string prefix = "[error] [GEO900] [";
            for (int j = 0; j < messages; ++j) {
                require(static_cast<bool>(std::getline(lines, line)), "concurrent logger lost a line");
                require(line.find(prefix) == 0, "invalid concurrent log prefix");
                check_timestamp(line.substr(prefix.size(), 19));
                require(line.substr(prefix.size() + 19) == "] parallel-log-" + std::to_string(i) + "-" + std::to_string(j), "concurrent logger mixed lines");
            }
            require(!std::getline(lines, line), "concurrent logger added lines");
        }
    }
}

std::unique_ptr<ifcopenshell::file> fixture(const std::string& schema, int tasks = 1, int instances = 1) {
    std::ostringstream data;
    data << "ISO-10303-21;\nHEADER;\n"
            "FILE_DESCRIPTION(('ViewDefinition [CoordinationView]'),'2;1');\n"
            "FILE_NAME('geometry-contract.ifc','2026-10-03T00:00:00',(),(),'','','');\n"
            "FILE_SCHEMA(('"
         << schema << "'));\nENDSEC;\nDATA;\n"
                      "#1=IFCCARTESIANPOINT((0.,0.,0.));\n"
                      "#2=IFCAXIS2PLACEMENT3D(#1,$,$);\n"
                      "#3=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,0.00001,#2,$);\n"
                      "#4=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Plan',3,0.00001,#2,$);\n"
                      "#5=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);\n"
                      "#6=IFCUNITASSIGNMENT((#5));\n"
                      "#7=IFCPROJECT('0000000000000000000001',$,$,$,$,$,$,(#3,#4),#6);\n"
                      "#8=IFCCARTESIANPOINT((1.,0.,0.));\n"
                      "#9=IFCPOLYLINE((#1,#8));\n"
                      "#10=IFCSHAPEREPRESENTATION(#3,'Body','Curve3D',(#9));\n"
                      "#11=IFCSHAPEREPRESENTATION(#4,'Axis','Curve3D',(#9));\n"
                      "#12=IFCPRODUCTDEFINITIONSHAPE($,$,(#10,#11));\n"
                      "#13=IFCLOCALPLACEMENT($,#2);\n";
    for (int i = 0; i < tasks; ++i) {
        const int base = 100 + i * (instances + 2);
        // Distinct representations retain distinct conversion tasks; instances
        // of each representation exercise the publication of result batches.
        data << '#' << base << "=IFCSHAPEREPRESENTATION(#3,'Body','Curve3D',(#9));\n";
        data << '#' << base + 1 << "=IFCPRODUCTDEFINITIONSHAPE($,$,(#" << base << ",#11));\n";
        for (int j = 0; j < instances; ++j) {
            const int id = base + 2 + j;
            const auto number = std::to_string(id);
            const auto guid = std::string(22 - number.size(), '0') + number;
            data << '#' << id << "=IFCBUILDINGELEMENTPROXY('" << guid << "',$,'" << id
                 << "',$,$,#13,#" << base + 1 << ",$,"
                 << (schema == "IFC2X3" ? ".ELEMENT." : ".NOTDEFINED.") << ");\n";
        }
    }
    data << "ENDSEC;\nEND-ISO-10303-21;\n";
    const auto text = data.str();
    std::istringstream stream(text);
    return std::make_unique<ifcopenshell::file>(stream, static_cast<int>(text.size()));
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
    auto file = fixture("IFC4");
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
    kernel.partial_success_is_success = false;
    std::vector<conversion_result> results;
    require(!kernel.open_cascade_kernel::convert_impl(boolean, results), "Boolean accepted a failed operand's partial geometry");
    require(results.empty(), "failed Boolean emitted a partial result");
}

void cache_settings(const std::string& schema) {
    auto file = fixture(schema);
    Settings settings;
    std::unique_ptr<abstract_mapping> mapping(
        ifcopenshell::geom::impl::mapping_implementations().construct(file.get(), settings));
    auto product = file->instance_by_id(102);
    auto expect = [&](int wanted) {
        auto representation = mapping->representation_of(product);
        const int actual = representation ? representation.id() : 0;
        require(actual == wanted, "representation " + std::to_string(actual) + " instead of " + std::to_string(wanted));
    };
    expect(100);
    mapping->settings().get<ContextIds>().value = std::set<int>{4};
    expect(11);
    mapping->settings().get<ContextIds>().value = std::set<int>{3};
    expect(100);
    mapping->settings().get<ContextIds>().value = std::set<int>{};
    expect(0); // Explicitly empty is different from unset.
    mapping->settings().get<ContextIds>().value = boost::none;
    expect(100);
    mapping->settings().get<OutputDimensionality>().value = CURVES;
    expect(11);
    mapping->settings().get<OutputDimensionality>().value = CURVES_SURFACES_AND_SOLIDS;
    expect(100); // Product representation order remains the tie breaker.
    mapping->settings().get<OutputDimensionality>().value = SURFACES_AND_SOLIDS;
    expect(100);
    mapping->settings().get<ContextIds>().value = std::set<int>{};
    mapping->settings().get<OutputDimensionality>().value = CURVES;
    expect(11); // The explicit-context Axis fallback is unchanged.
}

void cache_priorities(const std::string& schema) {
    auto file = fixture(schema);
    Settings settings;
    std::unique_ptr<abstract_mapping> mapping(ifcopenshell::geom::impl::mapping_implementations().construct(file.get(), settings));
    auto product = file->instance_by_id(102);
    auto expect = [&](std::vector<std::string> priorities, int wanted) {
        mapping->settings().get<ContextPriorities>().value = std::move(priorities);
        auto representation = mapping->representation_of(product);
        require((representation ? representation.id() : 0) == wanted, "context priority cache retained earlier selection");
    };
    expect({"*[ContextType=Plan]", "*[ContextType=Model]"}, 11);
    expect({"*[ContextType=Model]", "*[ContextType=Plan]"}, 100);
    expect({}, 0);
    mapping->settings().get<ContextIds>().value = std::set<int>{3};
    expect({"*[ContextType=Plan]"}, 100);
    mapping->settings().get<ContextIds>().value = boost::none;
    expect({"*[ContextType=Plan]"}, 11);
}

void cache_disabled(const std::string& schema) {
    auto file = fixture(schema);
    Settings settings;
    settings.get<ContextIds>().value = std::set<int>{3};
    std::unique_ptr<abstract_mapping> mapping(
        ifcopenshell::geom::impl::mapping_implementations().construct(file.get(), settings));
    mapping->use_caching() = false;
    auto product = file->instance_by_id(102);
    auto representation = file->instance_by_id(100);
    require(mapping->representation_of(product) == representation, "initial selection failed");
    representation.set_attribute_value("ContextOfItems", file->instance_by_id(4));
    require(!mapping->representation_of(product), "disabled cache retained the old context membership");
    representation.set_attribute_value("ContextOfItems", file->instance_by_id(3));
    mapping->use_caching() = true;
    require(mapping->representation_of(product) == representation, "re-enabled cache retained stale membership");
}

void cache_readers(const std::string& schema) {
    auto file = fixture(schema);
    Settings settings;
    std::unique_ptr<abstract_mapping> mapping(
        ifcopenshell::geom::impl::mapping_implementations().construct(file.get(), settings));
    auto product = file->instance_by_id(102);
    for (int context : {3, 4, 3}) {
        // Settings are changed only between groups of concurrent readers.
        mapping->settings().get<ContextIds>().value = std::set<int>{context};
        std::atomic<bool> start{false};
        std::atomic<int> wrong{0};
        std::vector<std::thread> readers;
        for (int i = 0; i < 8; ++i) {
            readers.emplace_back([&]() {
                while (!start) {
                    std::this_thread::yield();
                }
                for (int j = 0; j < 32; ++j) {
                    auto representation = mapping->representation_of(product);
                    if (!representation || representation.id() != (context == 3 ? 100 : 11)) {
                        ++wrong;
                    }
                }
            });
        }
        start = true;
        for (auto& reader : readers) {
            reader.join();
        }
        require(wrong == 0, "concurrent readers observed stale or incomplete cache");
    }
}

struct Control {
    std::atomic<int> kernels{0};
    std::atomic<int> shapes{0};
    std::atomic<int> attempted{0};
    std::atomic<int> clones{0};
    int fail_clone = -1;
    int fail_conversion = -1;
    bool fail_every_conversion = false;
    bool unknown_exception = false;
    bool fail_triangulation = false;
    bool fail_serialization = false;
    bool expect_error = false;
    int delay_us = 0;
};

class TrackedShape : public open_cascade_shape {
    std::shared_ptr<Control> control_;

  public:
    explicit TrackedShape(std::shared_ptr<Control> control)
        : open_cascade_shape(TopoDS_Shape()), control_(std::move(control)) {
        ++control_->shapes;
    }
    ~TrackedShape() override { --control_->shapes; }
    ifcopenshell::geom::conversion_result_shape* moved(taxonomy::matrix4::ptr) const override {
        return new TrackedShape(control_);
    }
    ifcopenshell::geom::conversion_result_shape* wrap_in_compound() override {
        return new TrackedShape(control_);
    }
    void triangulate(Settings, const taxonomy::matrix4&, ifcopenshell::geom::triangulation*, int, int, const std::vector<int>&, ifcopenshell::logger&) const override {
        if (control_->fail_triangulation) {
            throw std::runtime_error("injected triangulation failure");
        }
    }
    void serialize(const taxonomy::matrix4&, std::string&) const override {
        if (control_->fail_serialization) {
            throw std::runtime_error("injected serialization failure");
        }
    }
};

class ControlledKernel : public kernels::abstract_kernel {
    std::shared_ptr<Control> control_;

  public:
    ControlledKernel(const Settings& settings, std::shared_ptr<Control> control, ifcopenshell::logger& logger = ifcopenshell::logger::root())
        : abstract_kernel("test", settings, logger), control_(std::move(control)) {
        ++control_->kernels;
    }
    ~ControlledKernel() override { --control_->kernels; }
    bool supports_boolean_operations() const override { return false; }
    bool convert_openings(const express::base&, const std::vector<std::pair<taxonomy::ptr, taxonomy::matrix4>>&, const std::vector<ifcopenshell::geom::conversion_result>&, const taxonomy::matrix4&, std::vector<ifcopenshell::geom::conversion_result>&) override {
        throw std::runtime_error("unexpected opening conversion");
    }
    abstract_kernel* clone(ifcopenshell::logger& logger) const override {
        if (++control_->clones == control_->fail_clone) {
            if (control_->unknown_exception) {
                throw 7;
            }
            throw std::runtime_error("injected clone failure");
        }
        return new ControlledKernel(settings_, control_, logger);
    }
    bool convert(const taxonomy::ptr item, std::vector<ifcopenshell::geom::conversion_result>& results) override {
        const auto attempted = ++control_->attempted;
        if (control_->delay_us) {
            std::this_thread::sleep_for(std::chrono::microseconds(control_->delay_us));
        }
        if (control_->fail_every_conversion || attempted == control_->fail_conversion) {
            if (control_->unknown_exception) {
                throw 7;
            }
            throw std::runtime_error("injected conversion failure");
        }
        results.emplace_back(item->instance.id(), new TrackedShape(control_));
        return true;
    }
};

void iterator_case(int threads, bool no_parallel_mapping, IteratorOutputOptions output, int stop_after, const std::shared_ptr<Control>& control, int expected = -1) {
    constexpr int tasks = 32;
    constexpr int instances = 3;
    auto file = fixture("IFC4", tasks, instances);
    Settings settings;
    settings.get<OutputDimensionality>().value = CURVES_SURFACES_AND_SOLIDS;
    settings.get<ContextIds>().value = std::set<int>{3};
    settings.get<NoParallelMapping>().value = no_parallel_mapping;
    settings.get<IteratorOutput>().value = output;
    settings.get<ApplyDefaultMaterials>().value = false;
    std::set<int> seen;
    {
        auto kernel = std::make_unique<ControlledKernel>(settings, control);
        ifcopenshell::geom::iterator iterator(std::move(kernel), settings, file.get(), threads);
        if (iterator.initialize()) {
            do {
                auto element = iterator.get();
                require(element && seen.insert(element->id()).second, "duplicate or missing element");
                if (output != NATIVE) {
                    require(iterator.get_native()->id() == element->id(), "native/result batch mismatch");
                }
                if (stop_after >= 0 && static_cast<int>(seen.size()) >= stop_after) {
                    break;
                }
                std::this_thread::yield();
            } while (iterator.next());
        }
        if (control->expect_error || control->fail_clone > 0 || control->fail_conversion > 0 || control->fail_every_conversion ||
            control->fail_triangulation || control->fail_serialization) {
            require(iterator.had_error_processing_elements(), "worker failure not reported");
        }
        if (expected >= 0) {
            require(static_cast<int>(seen.size()) == expected,
                    "received " + std::to_string(seen.size()) + " elements, expected " + std::to_string(expected));
        }
    }
    require(control->kernels == 0, "kernel survived iterator destruction");
    require(control->shapes == 0, "geometry survived iterator destruction: " + std::to_string(control->shapes));
}
void paused_consumer(bool no_parallel_mapping, IteratorOutputOptions output, bool stop_early) {
    auto file = fixture("IFC4", 128, 3);
    auto control = std::make_shared<Control>();
    Settings settings;
    settings.get<OutputDimensionality>().value = CURVES_SURFACES_AND_SOLIDS;
    settings.get<ContextIds>().value = std::set<int>{3};
    settings.get<NoParallelMapping>().value = no_parallel_mapping;
    settings.get<IteratorOutput>().value = output;
    settings.get<ApplyDefaultMaterials>().value = false;
    std::set<int> seen;
    {
        ifcopenshell::geom::iterator iterator(std::make_unique<ControlledKernel>(settings, control), settings, file.get(), 4);
        require(iterator.initialize(), "paused consumer failed to initialize");
        // A paused caller must not cause the producer to materialize the whole
        // model. Leave room for every in-flight worker and representation batch.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(control->attempted <= 16, "paused consumer retained unbounded geometry: " + std::to_string(control->attempted));
        do {
            auto element = iterator.get();
            require(element && seen.insert(element->id()).second, "paused consumer lost or duplicated an element");
            if (output != NATIVE) {
                require(iterator.get_native()->id() == element->id(), "paused consumer native/result mismatch");
            }
            if (stop_early) {
                break;
            }
        } while (iterator.next());
        require(seen.size() == (stop_early ? 1u : 384u), "resumed consumer did not drain the model");
        require(!iterator.had_error_processing_elements(), "paused consumer reported an error");
    }
    require(control->kernels == 0 && control->shapes == 0, "paused iterator retained geometry after destruction");
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
    run("cold-concurrent-entity-serialization", concurrent_entity_serialization);
    run("healing-value-cycle", [&]() { healing_rejects_cycles(false, false); });
    run("healing-containment-cycle", [&]() { healing_rejects_cycles(true, false); });
    run("healing-value-cycle-location-mode", [&]() { healing_rejects_cycles(false, true); });
    run("healing-containment-cycle-location-mode", [&]() { healing_rejects_cycles(true, true); });
    run("healing-shared-subshapes", healing_preserves_shared_subshapes);
    run("healing-location-changes", healing_preserves_location_changes);
    run("healing-long-replacement-chain", healing_preserves_long_replacement_chains);
    run("boolean-failed-first-operand", [&]() { boolean_rejects_failed_operand(1); });
    run("boolean-failed-second-operand", [&]() { boolean_rejects_failed_operand(2); });
    run("header-and-value-serialization", header_and_value_serialization);
    run("concurrent-loggers-plain", [&]() { concurrent_loggers(ifcopenshell::logger::FMT_PLAIN); });
    run("concurrent-loggers-inmemory", [&]() { concurrent_loggers(ifcopenshell::logger::FMT_INMEMORY); });
    for (auto schema : ifcopenshell::schema_names()) {
        std::transform(schema.begin(), schema.end(), schema.begin(), [](unsigned char c) { return std::toupper(c); });
        if (schema != "HEADER_SECTION_SCHEMA") {
            run("cache-settings-" + schema, [&]() { cache_settings(schema); });
            run("cache-priorities-" + schema, [&]() { cache_priorities(schema); });
            run("cache-disabled-" + schema, [&]() { cache_disabled(schema); });
            run("cache-readers-" + schema, [&]() { cache_readers(schema); });
        }
    }
    for (bool serial_mapping : {false, true}) {
        const auto prefix = std::string("iterator-") + (serial_mapping ? "premapped-" : "parallel-map-");
        for (auto output : {NATIVE, TRIANGULATED, SERIALIZED}) {
            for (bool stop_early : {false, true}) {
                run(prefix + "paused-consumer-" + std::to_string(output) + "-stop" + std::to_string(stop_early), [&]() {
                    paused_consumer(serial_mapping, output, stop_early);
                });
            }
        }
        for (int threads : {1, 2, 4}) {
            for (auto output : {NATIVE, TRIANGULATED, SERIALIZED}) {
                for (int stop : {-1, 1, 5}) {
                    run(prefix + std::to_string(threads) + "-" + std::to_string(output) + "-stop" + std::to_string(stop), [&]() {
                        auto control = std::make_shared<Control>();
                        control->delay_us = 50;
                        iterator_case(threads, serial_mapping, output, stop, control, stop == -1 ? 96 : stop);
                    });
                }
            }
        }
        for (bool unknown : {false, true}) {
            run(prefix + "conversion-failure-" + std::to_string(unknown), [&]() {
                auto control = std::make_shared<Control>();
                control->fail_conversion = 2;
                control->unknown_exception = unknown;
                iterator_case(4, serial_mapping, NATIVE, -1, control, 93);
            });
            run(prefix + "all-conversions-fail-" + std::to_string(unknown), [&]() {
                auto control = std::make_shared<Control>();
                control->fail_every_conversion = true;
                control->unknown_exception = unknown;
                iterator_case(4, serial_mapping, NATIVE, -1, control, 0);
            });
        }
        for (int clone : {1, 2, 3, 4}) {
            for (bool unknown : {false, true}) {
                for (auto output : {NATIVE, TRIANGULATED, SERIALIZED}) {
                    run(prefix + "clone-failure-" + std::to_string(clone) + "-" + std::to_string(unknown) + "-" + std::to_string(output), [&]() {
                        auto control = std::make_shared<Control>();
                        control->fail_clone = clone;
                        control->unknown_exception = unknown;
                        iterator_case(4, serial_mapping, output, -1, control, 96);
                    });
                }
            }
        }
        for (int threads : {1, 4}) {
            for (auto output : {TRIANGULATED, SERIALIZED}) {
                run(prefix + "output-failure-" + std::to_string(threads) + "-" + std::to_string(output), [&]() {
                    auto control = std::make_shared<Control>();
                    control->fail_triangulation = output == TRIANGULATED;
                    control->fail_serialization = output == SERIALIZED;
                    iterator_case(threads, serial_mapping, output, -1, control, 0);
                });
            }
        }
#if defined(__linux__)
        for (auto output : {NATIVE, TRIANGULATED, SERIALIZED}) {
            for (int stop : {-1, 1, 5}) {
                run(prefix + "coordinator-start-failure-" + std::to_string(output) + "-stop" + std::to_string(stop), [&]() {
                    auto control = std::make_shared<Control>();
                    control->expect_error = true;
                    const auto before = injected_thread_failures.load();
                    ScopedThreadFailure failure_scope(1);
                    iterator_case(4, serial_mapping, output, stop, control, stop == -1 ? 96 : stop);
                    require(injected_thread_failures == before + 1, "coordinator failure injection was not reached");
                });
            }
        }
        for (int failure : {2, 3, 4}) {
            for (auto output : {NATIVE, TRIANGULATED, SERIALIZED}) {
                run(prefix + "thread-start-failure-" + std::to_string(failure) + "-" + std::to_string(output), [&]() {
                    auto control = std::make_shared<Control>();
                    control->expect_error = true;
                    control->delay_us = 1000;
                    const auto before = injected_thread_failures.load();
                    ScopedThreadFailure failure_scope(failure); // First creation is the coordinator.
                    iterator_case(4, serial_mapping, output, -1, control, 96);
                    require(injected_thread_failures == before + 1, "thread failure injection was not reached");
                });
            }
        }
        for (int failure : {2}) {
            run(prefix + "clone-and-thread-start-failure-" + std::to_string(failure), [&]() {
                auto control = std::make_shared<Control>();
                control->fail_clone = 3;
                control->delay_us = 1000;
                const auto before = injected_thread_failures.load();
                ScopedThreadFailure failure_scope(failure);
                iterator_case(4, serial_mapping, TRIANGULATED, -1, control, 96);
                require(injected_thread_failures == before + 1, "combined startup failure injection was not reached");
            });
        }
#endif
        for (auto output : {NATIVE, TRIANGULATED, SERIALIZED}) {
            for (int stop : {1, 5}) {
                run(prefix + "first-clone-failure-" + std::to_string(output) + "-stop" + std::to_string(stop), [&]() {
                    auto control = std::make_shared<Control>();
                    control->fail_clone = 1;
                    iterator_case(4, serial_mapping, output, stop, control, stop);
                });
            }
        }
    }
    for (bool deferred : {false, true}) {
        run("iterator-destroy-before-processing-" + std::to_string(deferred), [&]() {
            auto file = fixture("IFC4");
            auto control = std::make_shared<Control>();
            Settings settings;
            settings.get<DeferProcessingFirstElement>().value = deferred;
            settings.get<OutputDimensionality>().value = CURVES_SURFACES_AND_SOLIDS;
            {
                ifcopenshell::geom::iterator iterator(std::make_unique<ControlledKernel>(settings, control), settings, file.get(), 4);
                if (deferred) {
                    require(iterator.initialize(), "deferred initialization failed");
                }
            }
            require(control->attempted == 0 && control->kernels == 0, "unstarted iterator retained a kernel or started work");
        });
    }
    run("iterator-repeated-destruction-stress", [&]() {
        for (int i = 0; i < 50; ++i) {
            auto control = std::make_shared<Control>();
            control->delay_us = i % 2 ? 10 : 0;
            const auto stop = i % 3 ? -1 : 1;
            iterator_case(4, i % 2, TRIANGULATED, stop, control, stop == -1 ? 96 : 1);
        }
    });
    std::cout << "RESULT passed=" << passed << " failed=" << failed << std::endl;
    logger.set_output(static_cast<std::ostream*>(nullptr), static_cast<std::ostream*>(nullptr));
    return failed || !passed ? 1 : 0;
}
