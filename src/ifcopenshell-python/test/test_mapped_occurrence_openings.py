# This file was generated with the assistance of an AI coding tool.
import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.guid
import ifcopenshell.util.shape


def mapped_host_with_opening():
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

    source = box(4.0, 4.0)
    representation_map = model.createIfcRepresentationMap(axis, source)
    target = model.createIfcCartesianTransformationOperator3D(None, None, origin, 1.0, None)
    mapped_item = model.createIfcMappedItem(representation_map, target)
    occurrence = model.createIfcShapeRepresentation(context, "Body", "MappedRepresentation", (mapped_item,))
    host_axis = model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((10.0, 20.0, 30.0)), None, None)
    host = model.create_entity(
        "IfcWall",
        GlobalId=ifcopenshell.guid.new(),
        ObjectPlacement=model.createIfcLocalPlacement(None, host_axis),
        Representation=model.createIfcProductDefinitionShape(None, None, (occurrence,)),
    )
    opening_axis = model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((0.0, 0.0, -1.0)), None, None)
    opening = model.create_entity(
        "IfcOpeningElement",
        GlobalId=ifcopenshell.guid.new(),
        ObjectPlacement=model.createIfcLocalPlacement(host.ObjectPlacement, opening_axis),
        Representation=model.createIfcProductDefinitionShape(None, None, (box(1.0, 6.0),)),
    )
    model.create_entity(
        "IfcRelVoidsElement",
        GlobalId=ifcopenshell.guid.new(),
        RelatingBuildingElement=host,
        RelatedOpeningElement=opening,
    )
    return model, host, source.Items[0]


@pytest.mark.parametrize("kernel", ("opencascade", "hybrid-cgal-simple-opencascade-cgal"))
@pytest.mark.parametrize("openings_enabled", (True, False))
@pytest.mark.parametrize("threads", (None, 1, 4), ids=("create-shape", "iterator-1", "iterator-4"))
def test_single_mapped_occurrence_preserves_its_opening(kernel, openings_enabled, threads):
    model, host, item = mapped_host_with_opening()
    settings = ifcopenshell.geom.settings()
    settings.set("use-world-coords", False)
    settings.set("weld-vertices", True)
    settings.set("disable-opening-subtractions", not openings_enabled)
    if threads is None:
        shape = ifcopenshell.geom.create_shape(settings, host, geometry_library=kernel)
    else:
        iterator = ifcopenshell.geom.iterator(settings, model, threads, include=[host], geometry_library=kernel)
        assert iterator.initialize()
        shape = iterator.get()

    assert shape.id == host.id()
    geometry = shape.geometry
    vertices = ifcopenshell.util.shape.get_vertices(geometry)
    faces = ifcopenshell.util.shape.get_faces(geometry)
    assert len(faces) > 0, geometry.id
    assert np.isfinite(vertices).all()
    assert faces.min() >= 0 and faces.max() < len(vertices)
    assert ifcopenshell.util.shape.get_volume(geometry) == pytest.approx(60.0 if openings_enabled else 64.0, abs=1e-7)
    np.testing.assert_allclose(vertices.min(axis=0), (-2.0, -2.0, 0.0), atol=1e-9)
    np.testing.assert_allclose(vertices.max(axis=0), (2.0, 2.0, 4.0), atol=1e-9)
    np.testing.assert_allclose(
        ifcopenshell.util.shape.get_shape_matrix(shape),
        ((1.0, 0.0, 0.0, 10.0), (0.0, 1.0, 0.0, 20.0), (0.0, 0.0, 1.0, 30.0), (0.0, 0.0, 0.0, 1.0)),
        atol=1e-9,
    )
    assert set(geometry.item_ids) == {item.id()}
    edges = np.concatenate((faces[:, (0, 1)], faces[:, (1, 2)], faces[:, (2, 0)]))
    _, counts = np.unique(np.sort(edges, axis=1), axis=0, return_counts=True)
    assert np.all(counts == 2)
    if threads is not None:
        assert not iterator.next()


@pytest.mark.parametrize("openings_enabled", (True, False))
@pytest.mark.parametrize("threads", (None, 1, 4), ids=("create-shape", "iterator-1", "iterator-4"))
def test_unsupported_opening_subtraction_is_not_a_successful_shape(openings_enabled, threads):
    model, host, _ = mapped_host_with_opening()
    settings = ifcopenshell.geom.settings()
    settings.set("disable-opening-subtractions", not openings_enabled)
    if threads is None:
        if openings_enabled:
            with pytest.raises(RuntimeError, match="Failed to process shape"):
                ifcopenshell.geom.create_shape(settings, host, geometry_library="cgal-simple")
            return
        shape = ifcopenshell.geom.create_shape(settings, host, geometry_library="cgal-simple")
    else:
        iterator = ifcopenshell.geom.iterator(settings, model, threads, include=[host], geometry_library="cgal-simple")
        if openings_enabled:
            assert not iterator.initialize()
            return
        assert iterator.initialize()
        shape = iterator.get()

    assert shape.id == host.id()
    assert ifcopenshell.util.shape.get_volume(shape.geometry) == pytest.approx(64.0, abs=1e-7)
    if threads is not None:
        assert not iterator.next()
