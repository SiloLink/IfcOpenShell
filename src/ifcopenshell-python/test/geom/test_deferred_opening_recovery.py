"""Deferred opening recovery preserves usable geometry and explicit pending identity."""

import json
import subprocess
import sys
from pathlib import Path


def fixture(bad="native"):
    import ifcopenshell

    model = ifcopenshell.file(schema="IFC4")

    def axis():
        return model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((0.0, 0.0, 0.0)))

    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis(), None)
    units = model.createIfcUnitAssignment([model.createIfcSIUnit(None, "LENGTHUNIT", None, "METRE")])
    model.createIfcProject(ifcopenshell.guid.new(), None, None, None, None, None, None, [context], units)

    def box(width, height):
        profile = model.createIfcRectangleProfileDef("AREA", None, None, width, width)
        return model.createIfcExtrudedAreaSolid(profile, axis(), model.createIfcDirection((0.0, 0.0, 1.0)), height)

    def representation(item):
        representation_type = "SweptSolid" if item.is_a("IfcExtrudedAreaSolid") else "CSG"
        return model.createIfcShapeRepresentation(context, "Body", representation_type, [item])

    def product(ifc_class, representations):
        return model.create_entity(
            ifc_class,
            GlobalId=ifcopenshell.guid.new(),
            ObjectPlacement=model.createIfcLocalPlacement(None, axis()),
            Representation=model.createIfcProductDefinitionShape(None, None, representations)
            if representations
            else None,
        )

    representations = [representation(box(4.0, 4.0)), representation(box(2.0, 2.0))]
    host = product("IfcBuildingElementProxy", representations)
    openings = [product("IfcOpeningElement", [representation(box(1.0, 4.0))])]
    if bad == "native":
        # Mapping is well-defined, but the deliberately invalid negative-radius sphere
        # cannot produce an OCC solid. This exercises batch failure without a slow
        # Boolean or dependence on a real model's numerical instability.
        openings.append(product("IfcOpeningElement", [representation(model.createIfcSphere(axis(), -1.0))]))
    elif bad == "mapping":
        openings.append(product("IfcOpeningElement", []))
    for opening in openings:
        model.createIfcRelVoidsElement(ifcopenshell.guid.new(), None, None, None, host, opening)
    return model, host, representations, openings


def run_case(case):
    import ifcopenshell
    import ifcopenshell.geom
    import ifcopenshell.util.shape

    native = ifcopenshell.ifcopenshell_wrapper
    assert "deferred_opening_recovery_v1" in json.loads(native.silolink_capabilities())
    bad = "mapping" if case == "mapping-failure" else None if case == "batch-success" else "native"
    model, host, representations, openings = fixture(bad)
    settings = ifcopenshell.geom.settings()
    assert settings.get("opening-recovery-mode") == "inline"
    settings.set("opening-recovery-mode", "individual" if case == "individual" else "defer")
    settings.set("weld-vertices", True)
    representation = representations[1 if case == "explicit-representation" else 0]
    if case == "invalid-mode":
        settings.set("opening-recovery-mode", "unknown")
        try:
            ifcopenshell.geom.create_shape(settings, host, repr=representation, geometry_library="opencascade")
        except RuntimeError:
            return
        raise AssertionError("Unknown recovery mode was accepted")

    native.collect_diagnostics(True)
    native.take_diagnostics()
    iterator = None
    if case.startswith("deferred-"):
        output = case.removeprefix("deferred-").upper()
        settings.set("iterator-output", getattr(settings, output))
        iterator = ifcopenshell.geom.iterator(
            settings, model, num_threads=1, include=[host], geometry_library="opencascade"
        )
        assert iterator.initialize()
        shape = iterator.get()
    else:
        shape = ifcopenshell.geom.create_shape(settings, host, repr=representation, geometry_library="opencascade")

    pending = case.startswith("deferred-") or case == "explicit-representation"
    assert tuple(shape.deferred_opening_ids()) == (tuple(opening.id() for opening in openings) if pending else ())
    expected_representation = representation.id()
    if iterator is not None:
        assert shape.deferred_opening_representation_id() in {item.id() for item in representations}
        expected_representation = shape.deferred_opening_representation_id()
    assert shape.deferred_opening_representation_id() == (expected_representation if pending else 0)
    if case not in ("deferred-native", "deferred-serialized"):
        expected_volume = 8.0 if case == "explicit-representation" else 64.0 if pending else 60.0
        if iterator is not None and expected_representation == representations[1].id():
            expected_volume = 8.0
        volume = ifcopenshell.util.shape.get_volume(shape.geometry)
        assert abs(volume - expected_volume) < 1e-8, (case, volume, expected_volume)

    if iterator is not None:
        while iterator.next():
            other = iterator.get()
            assert tuple(other.deferred_opening_ids()) == tuple(opening.id() for opening in openings)
            assert other.deferred_opening_representation_id() in {item.id() for item in representations}

    events = json.loads(native.take_diagnostics())
    incomplete = [event for event in events if event["category"] == "opening_incomplete"]
    assert bool(incomplete) == (case in ("individual", "mapping-failure")), (case, events)
    if incomplete:
        assert {int(event["source_id"]) for event in incomplete} == {openings[-1].id()}
    if pending:
        assert not [event for event in events if event["category"] == "fallback_recovered"]
    native.collect_diagnostics(False)


if __name__ == "__main__":
    run_case(sys.argv[1])
else:
    import pytest

    @pytest.mark.parametrize(
        "case",
        (
            "deferred-triangulated",
            "deferred-native",
            "deferred-serialized",
            "individual",
            "batch-success",
            "mapping-failure",
            "explicit-representation",
            "invalid-mode",
        ),
    )
    def test_deferred_opening_recovery(case):
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), case], capture_output=True, text=True, timeout=30
        )
        assert result.returncode == 0, result.stdout + result.stderr
