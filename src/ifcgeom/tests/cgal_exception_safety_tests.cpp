// Exercise the real CGAL external-structure rebuild with failures in the
// locator it clones. Failed rebuilds must remain safely destructible, while
// successful rebuilds transfer ownership exactly once and preserve geometry.
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Nef_polyhedron_3.h>
#include <CGAL/Polyhedron_3.h>

// This internal CGAL header requires Nef's declarations and debug-macro setup.
// clang-format off
#include <CGAL/Convex_decomposition_3/External_structure_builder.h>
// clang-format on

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Kernel = CGAL::Exact_predicates_exact_constructions_kernel;
using Polyhedron = CGAL::Polyhedron_3<Kernel>;
using Nef = CGAL::Nef_polyhedron_3<Kernel>;
using Structure = Nef::SNC_structure;
using Decorator = CGAL::SNC_decorator<Structure>;
using Locator = CGAL::SNC_point_locator<Decorator>;
using SubdivisionLocator = CGAL::SNC_point_locator_by_spatial_subdivision<Decorator>;

enum class Failure {
    None,
    Clone,
    BeforeInitialize,
    AfterInitialize,
};

struct InjectedFailure : std::runtime_error {
    explicit InjectedFailure(Failure point)
        : std::runtime_error("injected point-locator failure"), point(point) {}
    Failure point;
};

struct Lifetime {
    int clone_calls = 0;
    int replacement_initializations = 0;
    int original_destructions = 0;
    int replacement_destructions = 0;
};

class TrackingLocator : public SubdivisionLocator {
    Lifetime& lifetime_;
    Failure failure_;
    bool replacement_;

  public:
    TrackingLocator(Lifetime& lifetime, Failure failure, bool replacement = false)
        : lifetime_(lifetime), failure_(failure), replacement_(replacement) {}

    TrackingLocator* clone() const override {
        ++lifetime_.clone_calls;
        if (failure_ == Failure::Clone) {
            throw InjectedFailure(Failure::Clone);
        }
        return new TrackingLocator(lifetime_, failure_, true);
    }

    void initialize(Structure* structure) override {
        if (replacement_) {
            ++lifetime_.replacement_initializations;
            if (failure_ == Failure::BeforeInitialize) {
                throw InjectedFailure(Failure::BeforeInitialize);
            }
        }
        // Allocate and populate the actual CGAL search tree. The later failure
        // verifies cleanup of its owned nodes, not just an empty test double.
        SubdivisionLocator::initialize(structure);
        if (replacement_ && failure_ == Failure::AfterInitialize) {
            throw InjectedFailure(Failure::AfterInitialize);
        }
    }

    ~TrackingLocator() override {
        if (replacement_) {
            ++lifetime_.replacement_destructions;
        } else {
            ++lifetime_.original_destructions;
        }
    }
};

// Access is restricted to test setup and ownership observations. Production
// delegate and External_structure_builder execute without replacement hooks.
class ProbeNef : public Nef {
  public:
    explicit ProbeNef(const Polyhedron& polyhedron) : Nef(polyhedron) {}

    void install(std::unique_ptr<TrackingLocator> locator) {
        locator->initialize(&snc());
        delete pl();
        pl() = locator.release();
    }

    const Locator* locator() const { return pl(); }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        // The uncorrected builder can leave a freed locator owned by Nef.
        // Report that contract failure without unwinding through its dangling
        // pointer and replacing the useful diagnosis with a second crash.
        std::cerr << "CGAL exception-safety contract failed: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Polyhedron tetrahedron() {
    Polyhedron result;
    result.make_tetrahedron(Kernel::Point_3(0, 0, 0),
                            Kernel::Point_3(1, 0, 0),
                            Kernel::Point_3(0, 1, 0),
                            Kernel::Point_3(0, 0, 1));
    return result;
}

void failure_contract(Failure failure) {
    Lifetime lifetime;
    const auto shape = tetrahedron();
    {
        ProbeNef nef(shape);
        require(nef.is_valid() && !nef.is_empty() && nef.is_bounded(), "invalid bounded fixture");
        nef.install(std::make_unique<TrackingLocator>(lifetime, failure));
        const auto* original = nef.locator();
        CGAL::External_structure_builder<Nef> rebuild;
        bool caught = false;
        try {
            nef.delegate(rebuild);
        } catch (const InjectedFailure& error) {
            require(error.point == failure, "failure came from the wrong operation");
            caught = true;
        } catch (const std::exception& error) {
            require(false, std::string("unexpected rebuild exception: ") + error.what());
        }
        require(caught, "real external-structure rebuild did not reach the injected failure");
        require(lifetime.clone_calls == 1, "rebuild did not clone exactly once");
        require(nef.locator() == original, "failed rebuild changed the owning locator");
        require(lifetime.original_destructions == 0, "failed rebuild destroyed the owning locator");
        const int replacements = failure == Failure::Clone ? 0 : 1;
        require(lifetime.replacement_initializations == replacements, "replacement initialization count differs");
        require(lifetime.replacement_destructions == replacements, "failed rebuild leaked its replacement locator");
        // Rebuilding can already have changed the SNC. Do not query or reuse
        // that geometry after failure; its guaranteed operation is destruction.
    }
    require(lifetime.original_destructions == 1, "Nef did not destroy its retained locator exactly once");
    const int replacements = failure == Failure::Clone ? 0 : 1;
    require(lifetime.replacement_destructions == replacements, "Nef destroyed a replacement more than once");
}

void success_contract() {
    Lifetime lifetime;
    const auto shape = tetrahedron();
    {
        ProbeNef nef(shape);
        nef.install(std::make_unique<TrackingLocator>(lifetime, Failure::None));
        const auto* original = nef.locator();
        CGAL::External_structure_builder<Nef> rebuild;
        nef.delegate(rebuild);
        require(lifetime.clone_calls == 1 && lifetime.replacement_initializations == 1,
                "successful rebuild did not initialize exactly one replacement");
        require(nef.locator() != original, "successful rebuild did not transfer ownership");
        require(lifetime.original_destructions == 1, "successful rebuild did not destroy the old locator exactly once");
        require(lifetime.replacement_destructions == 0, "successful rebuild destroyed its new locator");
        require(nef.is_valid() && !nef.is_empty() && nef.is_bounded(), "rebuild damaged the bounded solid");
        const Nef expected(shape);
        require(nef == expected, "rebuild changed the represented solid");
    }
    require(lifetime.original_destructions == 1, "Nef destroyed its old locator again");
    require(lifetime.replacement_destructions == 1, "Nef did not destroy its new locator exactly once");
}
} // namespace

int main() {
    try {
        failure_contract(Failure::Clone);
        failure_contract(Failure::BeforeInitialize);
        failure_contract(Failure::AfterInitialize);
        success_contract();
        std::cout << "4 CGAL exception-safety contracts passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "CGAL exception-safety contract failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
