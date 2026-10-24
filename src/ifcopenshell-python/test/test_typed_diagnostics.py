# This file was generated with the assistance of an AI coding tool.
"""Typed diagnostics and capabilities come from the loaded binary, not from parsed log text."""

import json

import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.guid

HEADER = (
    "ISO-10303-21;HEADER;FILE_DESCRIPTION((''),'2;1');FILE_NAME('','',(),(),'','','');"
    "FILE_SCHEMA(('IFC4'));ENDSEC;DATA;"
)


@pytest.fixture(autouse=True)
def typed_sink():
    ifcopenshell.get_log()
    ifcopenshell.collect_diagnostics(True)
    ifcopenshell.take_diagnostics()
    yield
    ifcopenshell.collect_diagnostics(False)


def duplicate_definition_file(tmp_path, copies):
    first = "#1=IFCQUANTITYLENGTH('a',$,$,1.,$);"
    path = tmp_path / "duplicate.ifc"
    path.write_text(HEADER + first * copies + "ENDSEC;END-ISO-10303-21;")
    return path


def host_with_unsupported_opening():
    model = ifcopenshell.file(schema="IFC4")
    origin = model.createIfcCartesianPoint((0.0, 0.0, 0.0))
    axis = model.createIfcAxis2Placement3D(origin, None, None)
    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
    units = model.createIfcUnitAssignment((model.createIfcSIUnit(None, "LENGTHUNIT", None, "METRE"),))
    model.create_entity(
        "IfcProject", GlobalId=ifcopenshell.guid.new(), RepresentationContexts=(context,), UnitsInContext=units
    )

    def box(width, depth):
        profile = model.createIfcRectangleProfileDef("AREA", None, None, width, width)
        solid = model.createIfcExtrudedAreaSolid(profile, axis, model.createIfcDirection((0.0, 0.0, 1.0)), depth)
        return model.createIfcShapeRepresentation(context, "Body", "SweptSolid", (solid,))

    host = model.create_entity(
        "IfcWall",
        GlobalId=ifcopenshell.guid.new(),
        ObjectPlacement=model.createIfcLocalPlacement(None, axis),
        Representation=model.createIfcProductDefinitionShape(None, None, (box(4.0, 4.0),)),
    )
    opening = model.create_entity(
        "IfcOpeningElement",
        GlobalId=ifcopenshell.guid.new(),
        ObjectPlacement=model.createIfcLocalPlacement(host.ObjectPlacement, axis),
        Representation=model.createIfcProductDefinitionShape(None, None, (box(1.0, 6.0),)),
    )
    model.create_entity(
        "IfcRelVoidsElement",
        GlobalId=ifcopenshell.guid.new(),
        RelatingBuildingElement=host,
        RelatedOpeningElement=opening,
    )
    return model, host, opening


def test_diagnostic_contract_is_exported_by_the_binary():
    assert "typed_diagnostics" in ifcopenshell.capabilities
    contract = ifcopenshell.diagnostic_contract()
    assert set(contract) == {"categories", "outcomes"}
    assert {"step_deduplicated", "opening_incomplete", "fallback_recovered"} <= set(contract["categories"])
    assert contract["outcomes"] == ["missing", "partial", "degraded", "recovered"]


def test_duplicate_definition_events_are_typed_and_taken_once(tmp_path):
    ifcopenshell.open(duplicate_definition_file(tmp_path, copies=3))
    events = ifcopenshell.take_diagnostics()
    assert [event["category"] for event in events] == ["step_deduplicated", "step_deduplicated"]
    assert all(event["entity_id"] == "1" and event["outcome"] == "recovered" for event in events)
    assert len({event["detail_id"] for event in events}) == 2
    assert ifcopenshell.take_diagnostics() == []
    # The text envelope is unchanged for consumers that still read the log.
    assert ifcopenshell.get_log().count("PARSER_EVENT_V1 ") == 2


def test_disabled_sink_keeps_nothing_and_discards_pending_events(tmp_path):
    path = duplicate_definition_file(tmp_path, copies=2)
    ifcopenshell.open(path)
    ifcopenshell.collect_diagnostics(False)
    assert ifcopenshell.take_diagnostics() == []
    ifcopenshell.open(path)
    assert ifcopenshell.take_diagnostics() == []
    assert ifcopenshell.get_log().count("PARSER_EVENT_V1 ") == 2


@pytest.mark.parametrize("threads", (1, 4), ids=("iterator-1", "iterator-4"))
def test_iterator_worker_events_merge_into_the_root_sink(threads):
    model, host, opening = host_with_unsupported_opening()
    settings = ifcopenshell.geom.settings()
    iterator = ifcopenshell.geom.iterator(settings, model, threads, include=[host], geometry_library="cgal-simple")
    assert iterator.initialize()
    shape = iterator.get()
    assert shape.id == host.id()
    assert not iterator.next()
    events = [event for event in ifcopenshell.take_diagnostics() if event["category"] == "opening_incomplete"]
    assert len(events) == 1
    assert events[0]["entity_id"] == str(host.id())
    assert events[0]["source_id"] == str(opening.id())
    assert events[0]["action"] == "skip_cut"
    assert events[0]["outcome"] == "partial"
