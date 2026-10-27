# Geometry contract tests

These tests exercise mutable representation settings, disabled caching,
concurrent readers, Iterator ownership, startup failures, early destruction,
paused consumers, concurrent worker logging and first-call entity serialization.
They require OpenCASCADE and use in-memory IFC fixtures.

`ifcgeom_deferred_opening_tests` covers the internal `opening-recovery-mode`
policy. `inline` preserves the default batch-then-individual behavior. `defer`
performs the same batch attempt, but after failure returns the original host
with `deferred_opening_ids()` and `deferred_opening_representation_id()` instead
of attempting individual cuts. `individual` runs only the individual cuts and
is intended for a separately bounded worker recovering that host/representation.
The native call itself has no deadline or cancellation guarantee, including
the initial batch. Consumers must supervise the recovery process externally.

Pending metadata survives native, triangulated and serialized element copies.
It is not an `opening_incomplete` or `fallback_recovered` diagnostic: the
consumer must publish either the validated recovery result or retained base
geometry with explicit incomplete-opening diagnostics. Opening mapping failures
remain final diagnostics because no cutter is available to retry. Consumers
must require capability `deferred_opening_recovery_v1` before using this policy.
The controlled-kernel tests check exact call sequences, failed-output discard,
mapping failures and per-representation identity. The corresponding Python API
tests are `test/geom/test_deferred_opening_recovery.py` and run each fixture in
a process with a 30-second limit. The C++ target uses the same plugin-directory
variable described below.

Build the non-default `ifcgeom_contract_tests` target. The test executable uses
schema and mapping plugins from the configured build. When running outside the
installed plugin directory, set `IFCGEOM_CONTRACT_PLUGIN_DIR` to the directory
containing those plugins. An optional argument filters test names.

`ifcgeom_boolean_contract_tests` (also non-default) covers the OpenCASCADE
Boolean crash-safety contracts: the healing context rejects cyclic ShapeFix
replacements, by value and by containment and with location mode on, while
shared sub-shapes, finite location changes and long replacement chains still
heal; in complete mode, a Boolean whose first or second operand fails to convert
returns failure without emitting partial geometry. Partial mode skips failed
operands, but subtraction still requires a valid first operand. It takes the
same plugin directory variable and name filter.

`ifcgeom_cgal_opening_contract_tests` is a non-default target available with
ordinary CGAL. It uses actual box, open-shell, collection and nested Boolean
conversion to check that complete opening conversion rejects a missing cutter
or operand without publishing partial output. A valid nested `A-B` cutter
retains the expected host volume. The complete-cutter contract now lives in
`abstract_kernel::convert_openings`, so a kernel called directly in partial
mode rejects an incomplete cutter too and restores the caller's mode on every
exit. The hybrid check verifies complete conversion and policy restoration
across repeated calls, including after a partial conversion with caching
enabled. Complete conversions remain reusable from the cache. It uses the same
plugin directory variable and filter.

Only successful complete-mode conversions populate the shape cache. Partial
conversions may omit operands, so their output must not satisfy a later complete
conversion of the same item. A complete cached result can serve either mode.

```sh
cmake --build build --target ifcgeom_cgal_opening_contract_tests
IFCGEOM_CONTRACT_PLUGIN_DIR=/path/to/build/plugins build/ifcgeom/ifcgeom_cgal_opening_contract_tests
```

The cold serialization test runs before any file is constructed. Eight callers
serialize entities and entity references concurrently, exercising the first-use
path used by worker logs. Header entities and typed values retain their STEP
format without an instance-id prefix.

The Iterator coordinator participates in conversion. Partial clone or worker
startup failure leaves available workers able to drain the queue; if no clone
or coordinator can start, the caller uses its original converter serially. The
error flag records failures even when startup recovery yields complete output.

The producer ready queue allows one result per worker plus in-flight
representation batches. Producers pause while the caller is behind and wake
when consumption resumes or the iterator is destroyed. A representation shared
by many products remains a single batch; this is not a byte-level memory cap.

In native output mode, `get()` transfers native ownership. For other output
modes, `get()` and `get_native()` each transfer ownership once. Tests retain
these v0.9 contracts; the removed v0.8 serializer cache is not reintroduced.

Linux fault injection interposes `pthread_create` only in this executable.
Use an external timeout for deadlock checks. Sanitizer coverage is limited to
the translation units actually compiled with sanitizer flags; linking an
instrumented test to uninstrumented libraries does not cover those libraries.
