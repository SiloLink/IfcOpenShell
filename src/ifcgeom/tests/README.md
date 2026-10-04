# Geometry contract tests

These tests exercise mutable representation settings, disabled caching,
concurrent readers, Iterator ownership, startup failures, early destruction,
paused consumers, concurrent worker logging and first-call entity serialization.
They require OpenCASCADE and use in-memory IFC fixtures.

Build the non-default `ifcgeom_contract_tests` target. The test executable uses
schema and mapping plugins from the configured build. When running outside the
installed plugin directory, set `IFCGEOM_CONTRACT_PLUGIN_DIR` to the directory
containing those plugins. An optional argument filters test names.

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
