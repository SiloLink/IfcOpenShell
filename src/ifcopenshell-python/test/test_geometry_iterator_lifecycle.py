import gc
import json
import subprocess
import sys

import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.guid
import ifcopenshell.util.shape


def model_with_context(schema="IFC4", context_type="Model"):
    model = ifcopenshell.file(schema=schema)
    origin = model.createIfcCartesianPoint((0.0, 0.0, 0.0))
    axis = model.createIfcAxis2Placement3D(origin, None, None)
    context = model.createIfcGeometricRepresentationContext(None, context_type, 3, 1e-5, axis, None)
    units = model.createIfcUnitAssignment((model.createIfcSIUnit(None, "LENGTHUNIT", None, "METRE"),))
    model.create_entity(
        "IfcProject", GlobalId=ifcopenshell.guid.new(), RepresentationContexts=(context,), UnitsInContext=units
    )
    return model, context


def box(model, context, width=2.0, length=3.0, depth=4.0):
    profile = model.create_entity(
        "IfcRectangleProfileDef",
        ProfileType="AREA",
        Position=model.createIfcAxis2Placement2D(model.createIfcCartesianPoint((0.0, 0.0)), None),
        XDim=width,
        YDim=length,
    )
    item = model.create_entity(
        "IfcExtrudedAreaSolid",
        SweptArea=profile,
        Position=model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((0.0, 0.0, 0.0)), None, None),
        ExtrudedDirection=model.createIfcDirection((0.0, 0.0, 1.0)),
        Depth=depth,
    )
    return model.createIfcShapeRepresentation(context, "Body", "SweptSolid", (item,))


def product(model, representation, index, ifc_class="IfcBuildingElementProxy", translation=(0.0, 0.0, 0.0)):
    axis = model.createIfcAxis2Placement3D(model.createIfcCartesianPoint(translation), None, None)
    return model.create_entity(
        ifc_class,
        GlobalId=ifcopenshell.guid.new(),
        Name=str(index),
        ObjectPlacement=model.createIfcLocalPlacement(None, axis),
        Representation=model.createIfcProductDefinitionShape(None, None, (representation,)),
    )


def iterator_result(model, products, threads, no_parallel_mapping=False):
    settings = ifcopenshell.geom.settings()
    settings.set("no-normals", True)
    settings.set("no-parallel-mapping", no_parallel_mapping)
    iterator = ifcopenshell.geom.iterator(settings, model, threads, include=products, geometry_library="opencascade")
    assert iterator.initialize()
    result = {}
    while True:
        shape = iterator.get()
        assert shape.id not in result
        native = iterator.get_native()
        assert native.id == shape.id
        result[shape.id] = (
            shape.geometry.verts,
            shape.geometry.faces,
            shape.geometry.edges,
            shape.geometry.material_ids,
            tuple(ifcopenshell.util.shape.get_shape_matrix(shape).reshape(-1)),
        )
        if not iterator.next():
            break
    assert iterator.progress() == 100
    return result


@pytest.mark.parametrize("schema", ("IFC2X3", "IFC4", "IFC4X3_ADD2"))
@pytest.mark.parametrize("no_parallel_mapping", (False, True))
def test_parallel_iterator_preserves_all_shared_and_unique_products(schema, no_parallel_mapping):
    model, context = model_with_context(schema)
    products = []
    for index in range(18):
        representation = box(model, context, width=1.0 + index / 10)
        for instance in range(2):
            products.append(
                product(model, representation, len(products), translation=(float(index), float(instance), 0.0))
            )
    expected = iterator_result(model, products, 1, no_parallel_mapping)
    assert set(expected) == {item.id() for item in products}
    for threads in (2, 4):
        assert iterator_result(model, products, threads, no_parallel_mapping) == expected


@pytest.mark.parametrize("only_invalid", (False, True))
def test_parallel_iterator_finishes_when_tasks_have_no_geometry(tmp_path, only_invalid):
    model, context = model_with_context()
    empty = model.createIfcShapeRepresentation(context, "Body", "SweptSolid", ())
    products = [product(model, empty, index) for index in range(8)]
    valid = [] if only_invalid else [product(model, box(model, context), 8)]
    products.extend(valid)
    path = tmp_path / "empty-representations.ifc"
    model.write(str(path))
    script = """
import ifcopenshell, ifcopenshell.geom, json, sys
model = ifcopenshell.open(sys.argv[1])
iterator = ifcopenshell.geom.iterator(ifcopenshell.geom.settings(), model, 4,
    include=model.by_type('IfcBuildingElementProxy'), geometry_library='opencascade')
ids = []
if iterator.initialize():
    while True:
        ids.append(iterator.get().id)
        if not iterator.next(): break
print(json.dumps(ids))
"""
    result = subprocess.run([sys.executable, "-c", script, str(path)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout.strip().splitlines()[-1]) == [item.id() for item in valid]


def test_parallel_iterator_can_be_destroyed_before_consuming_all_results(tmp_path):
    model, context = model_with_context()
    for index in range(48):
        product(model, box(model, context, width=1.0 + index / 10), index)
    path = tmp_path / "early-exit.ifc"
    model.write(str(path))
    script = """
import gc, ifcopenshell, ifcopenshell.geom, sys
model = ifcopenshell.open(sys.argv[1])
for count in (1, 3, 7):
    iterator = ifcopenshell.geom.iterator(ifcopenshell.geom.settings(), model, 4,
        geometry_library='opencascade')
    assert iterator.initialize()
    for _ in range(count):
        assert iterator.get().geometry.faces
        assert iterator.next()
    del iterator
    gc.collect()
print('finished')
"""
    result = subprocess.run([sys.executable, "-c", script, str(path)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip().endswith("finished")


@pytest.mark.parametrize("schema", ("IFC2X3", "IFC4", "IFC4X3_ADD2"))
@pytest.mark.parametrize("explicit_context", (False, True))
def test_openings_keep_context_selection_and_prefer_non_bounding_box(schema, explicit_context):
    model, context = model_with_context(schema)
    plan = model.createIfcGeometricRepresentationContext(None, "Plan", 3, 1e-5, context.WorldCoordinateSystem, None)
    model.by_type("IfcProject")[0].RepresentationContexts = (context, plan)
    hosts = []
    for index in range(6):
        host = product(model, box(model, context, 4.0, 4.0, 4.0), index, "IfcWall")
        hosts.append(host)
        opening = product(model, box(model, context, 1.0, 1.0, 6.0), index, "IfcOpeningElement")
        unwanted = box(model, plan, 3.0, 3.0, 6.0)
        bounds = model.createIfcBoundingBox(model.createIfcCartesianPoint((-1.5, -1.5, 0.0)), 3.0, 3.0, 6.0)
        bounding_rep = model.createIfcShapeRepresentation(context, "Box", "BoundingBox", (bounds,))
        opening.Representation.Representations = (unwanted, bounding_rep, *opening.Representation.Representations)
        model.create_entity(
            "IfcRelVoidsElement",
            GlobalId=ifcopenshell.guid.new(),
            RelatingBuildingElement=host,
            RelatedOpeningElement=opening,
        )
    settings = ifcopenshell.geom.settings()
    if explicit_context:
        settings.set("context-ids", [context.id()])
    iterator = ifcopenshell.geom.iterator(settings, model, 2, include=hosts, geometry_library="opencascade")
    assert iterator.initialize()
    seen = set()
    while True:
        shape = iterator.get()
        seen.add(shape.id)
        assert ifcopenshell.util.shape.get_volume(shape.geometry) == pytest.approx(60.0, abs=1e-7)
        vertices = ifcopenshell.util.shape.get_vertices(shape.geometry)
        assert np.min(vertices, axis=0) == pytest.approx((-2.0, -2.0, 0.0))
        assert np.max(vertices, axis=0) == pytest.approx((2.0, 2.0, 4.0))
        if not iterator.next():
            break
    assert seen == {host.id() for host in hosts}
    gc.collect()
