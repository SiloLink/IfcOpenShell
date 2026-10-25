# This file was generated with the assistance of an AI coding tool.
"""Mapped occurrences must be scheduled even when their source has direct owners."""

import json
import subprocess
import sys
from pathlib import Path


def run_case(schema, case, threads):
    import numpy as np

    import ifcopenshell
    import ifcopenshell.geom
    import ifcopenshell.util.shape

    model = ifcopenshell.file(schema=schema)

    def axis(x=0.0):
        return model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((x, 0.0, 0.0)))

    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis(), None)
    units = model.createIfcUnitAssignment([model.createIfcSIUnit(None, "LENGTHUNIT", None, "METRE")])
    model.createIfcProject(ifcopenshell.guid.new(), None, None, None, None, None, None, [context], units)
    profile_position = model.createIfcAxis2Placement2D(model.createIfcCartesianPoint((0.0, 0.0)))
    profile = model.createIfcRectangleProfileDef("AREA", None, profile_position, 4.0, 3.0)
    extrusion = model.createIfcExtrudedAreaSolid(profile, axis(), model.createIfcDirection((0.0, 0.0, 1.0)), 2.0)
    source_context = context
    if case == "context":
        source_context = model.createIfcGeometricRepresentationContext(None, "Other", 3, 1e-5, axis(), None)
    source = model.createIfcShapeRepresentation(source_context, "Body", "SweptSolid", [extrusion])

    def product(rep, x):
        return model.createIfcBuildingElementProxy(
            ifcopenshell.guid.new(),
            None,
            None,
            None,
            None,
            model.createIfcLocalPlacement(None, axis(x)),
            model.createIfcProductDefinitionShape(None, None, [rep]),
        )

    owner = product(source, 20.0)
    first_map = model.createIfcRepresentationMap(axis(), source)
    targets = []
    for i in range(3):
        mapping = (
            first_map if case in ("single", "opening", "filtered") else model.createIfcRepresentationMap(axis(), source)
        )
        transform = model.createIfcCartesianTransformationOperator3D(
            None, None, model.createIfcCartesianPoint((0.0, 0.0, 0.0)), 1.0, None
        )
        if case == "transform":
            transform.LocalOrigin = model.createIfcCartesianPoint((float(i + 1), 2.0, 3.0))
            transform.Scale = 2.0
            transform.Axis1 = model.createIfcDirection((-1.0, 0.0, 0.0))
        item = model.createIfcMappedItem(mapping, transform)
        if case == "styled":
            colour = model.createIfcColourRgb(None, 0.2, 0.3, 0.4 + i * 0.1)
            style = model.createIfcSurfaceStyle(None, "BOTH", [model.createIfcSurfaceStyleShading(colour)])
            styles = [model.createIfcPresentationStyleAssignment([style])] if schema == "IFC2X3" else [style]
            model.createIfcStyledItem(item, styles, None)
        rep = model.createIfcShapeRepresentation(context, "Body", "MappedRepresentation", [item])
        targets.append(product(rep, float(i * 8)))

    if case == "opening":
        hole_profile = model.createIfcRectangleProfileDef("AREA", None, profile_position, 1.0, 1.0)
        hole = model.createIfcExtrudedAreaSolid(hole_profile, axis(), model.createIfcDirection((0.0, 0.0, 1.0)), 2.0)
        rep = model.createIfcShapeRepresentation(context, "Body", "SweptSolid", [hole])
        opening = model.createIfcOpeningElement(
            ifcopenshell.guid.new(),
            None,
            None,
            None,
            None,
            model.createIfcLocalPlacement(None, axis()),
            model.createIfcProductDefinitionShape(None, None, [rep]),
        )
        model.createIfcRelVoidsElement(ifcopenshell.guid.new(), None, None, None, targets[0], opening)

    selected = targets if case in ("filtered", "context") else [owner, *targets]
    settings = ifcopenshell.geom.settings()
    if case == "context":
        settings.set("context-ids", [context.id()])

    def snapshot(shape):
        g = shape.geometry
        # Copy while the shape is alive; geometry is a borrowed Python wrapper.
        return (
            np.array(g.verts),
            np.array(g.faces),
            ifcopenshell.util.shape.get_shape_matrix(shape).copy(),
            list(g.material_ids),
            [tuple(m.diffuse.components) for m in g.materials],
        )

    expected = {
        p.id(): snapshot(ifcopenshell.geom.create_shape(settings, p, geometry_library="opencascade")) for p in selected
    }
    iterator = ifcopenshell.geom.iterator(
        settings, model, num_threads=threads, include=selected, geometry_library="opencascade"
    )
    observed = {}
    assert iterator.initialize()
    while True:
        shape = iterator.get()
        assert shape.id not in observed, (case, "duplicate", shape.id)
        observed[shape.id] = snapshot(shape)
        if not iterator.next():
            break
    assert observed.keys() == expected.keys(), (case, sorted(expected.keys() - observed.keys()))
    for pid, wanted in expected.items():
        got = observed[pid]
        np.testing.assert_allclose(got[0], wanted[0], atol=1e-9, rtol=0)
        np.testing.assert_array_equal(got[1], wanted[1])
        np.testing.assert_allclose(got[2], wanted[2], atol=1e-9, rtol=0)
        assert got[3:] == wanted[3:], (case, pid, got[3:], wanted[3:])
    print(json.dumps({"schema": schema, "case": case, "threads": threads, "products": len(observed)}), flush=True)


if __name__ == "__main__":
    run_case(sys.argv[1], sys.argv[2], int(sys.argv[3]))
else:
    import pytest

    @pytest.mark.parametrize("schema", ("IFC2X3", "IFC4"))
    @pytest.mark.parametrize("case", ("multiple", "single", "filtered", "context", "transform", "styled", "opening"))
    @pytest.mark.parametrize("threads", (1, 4))
    def test_mapped_task_coverage(schema, case, threads):
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), schema, case, str(threads)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert result.returncode == 0, result.stdout + result.stderr
