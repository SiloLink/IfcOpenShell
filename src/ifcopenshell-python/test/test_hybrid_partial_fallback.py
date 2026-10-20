# This file was generated with the assistance of an AI coding tool.
import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.guid
import ifcopenshell.util.shape


def sweep_collection(mapped):
    model = ifcopenshell.file(schema="IFC4")
    origin = model.createIfcCartesianPoint((0.0, 0.0, 0.0))
    axis = model.createIfcAxis2Placement3D(origin, None, None)
    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
    units = model.createIfcUnitAssignment((model.createIfcSIUnit(None, "LENGTHUNIT", None, "METRE"),))
    model.create_entity(
        "IfcProject", GlobalId=ifcopenshell.guid.new(), RepresentationContexts=(context,), UnitsInContext=units
    )

    def sweep(points, colour):
        curve = model.createIfcPolyline(tuple(model.createIfcCartesianPoint(point) for point in points))
        solid = model.createIfcSweptDiskSolid(curve, 0.01075, None, 0.0, float(len(points)))
        shading = model.createIfcSurfaceStyleShading(model.createIfcColourRgb(None, *colour), 0.25)
        style = model.createIfcSurfaceStyle(None, "BOTH", (shading,))
        model.createIfcStyledItem(solid, (style,), None)
        return solid

    first = sweep(((-2.0, 0.0, 0.0), (-2.0, 0.0, 2.0)), (1.0, 0.0, 0.0))
    bad = sweep(((0.0, 0.0, 0.0), (0.0, 0.0, 0.24), (0.0, 0.0, -5.42)), (0.0, 1.0, 0.0))
    last = sweep(((2.0, 0.0, 0.0), (2.0, 0.0, 3.0)), (0.0, 0.0, 1.0))
    representation = model.createIfcShapeRepresentation(context, "Body", "SolidModel", (first, bad, last))
    if mapped:
        mapping = model.createIfcRepresentationMap(axis, representation)
        target = model.createIfcCartesianTransformationOperator3D(
            None, None, model.createIfcCartesianPoint((4.0, 5.0, 6.0)), 1.0, None
        )
        representation = model.createIfcShapeRepresentation(
            context, "Body", "MappedRepresentation", (model.createIfcMappedItem(mapping, target),)
        )
    placement = model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((10.0, 20.0, 30.0)), None, None)
    product = model.create_entity(
        "IfcFlowSegment",
        GlobalId=ifcopenshell.guid.new(),
        ObjectPlacement=model.createIfcLocalPlacement(None, placement),
        Representation=model.createIfcProductDefinitionShape(None, None, (representation,)),
    )
    return model, product, first, bad, last


def triangles_with_identity(shape):
    geometry = shape.geometry
    vertices = ifcopenshell.util.shape.get_vertices(geometry)
    faces = ifcopenshell.util.shape.get_faces(geometry)
    assert len(faces) > 0
    assert np.isfinite(vertices).all()
    assert faces.min() >= 0 and faces.max() < len(vertices)
    matrix = ifcopenshell.util.shape.get_shape_matrix(shape)
    world = vertices @ matrix[:3, :3].T + matrix[:3, 3]
    colours = ifcopenshell.util.shape.get_material_colors(geometry)
    material_ids = ifcopenshell.util.shape.get_faces_material_style_ids(geometry)
    assert np.all(material_ids >= 0)
    result = []
    for face, item_id, material_id in zip(faces, geometry.item_ids, material_ids):
        triangle = tuple(tuple(np.round(point, 9)) for point in world[face])
        oriented = min(triangle[i:] + triangle[:i] for i in range(3))
        result.append((item_id, tuple(np.round(colours[material_id], 9)), oriented))
    return sorted(result)


@pytest.mark.parametrize("mapped", (False, True), ids=("direct-representation", "mapped-representation"))
@pytest.mark.parametrize("kernel", ("opencascade", "hybrid-cgal-simple-opencascade-cgal"))
@pytest.mark.parametrize("threads", (None, 1, 4), ids=("create-shape", "iterator-1", "iterator-4"))
def test_failed_sweep_keeps_both_healthy_siblings_with_colours_and_transforms(mapped, kernel, threads):
    model, product, first, bad, last = sweep_collection(mapped)
    settings = ifcopenshell.geom.settings()
    settings.set("apply-default-materials", False)
    with pytest.raises(RuntimeError, match="Failed to process shape"):
        ifcopenshell.geom.create_shape(settings, bad, geometry_library="opencascade")
    reference = ifcopenshell.geom.create_shape(settings, product, geometry_library="opencascade")
    expected = triangles_with_identity(reference)
    assert {item_id for item_id, _, _ in expected} == {first.id(), last.id()}
    assert {colour for _, colour, _ in expected} == {(1.0, 0.0, 0.0, 0.75), (0.0, 0.0, 1.0, 0.75)}
    reference_vertices = np.asarray([point for _, _, triangle in expected for point in triangle])
    offset = np.asarray((14.0, 25.0, 36.0) if mapped else (10.0, 20.0, 30.0))
    np.testing.assert_allclose(reference_vertices[:, 2].min(), offset[2], atol=1e-9)
    np.testing.assert_allclose(reference_vertices[:, 2].max(), offset[2] + 3.0, atol=1e-9)
    assert reference_vertices[:, 0].min() < offset[0] - 2.0
    assert reference_vertices[:, 0].max() > offset[0] + 2.0

    if threads is None:
        shape = ifcopenshell.geom.create_shape(settings, product, geometry_library=kernel)
    else:
        iterator = ifcopenshell.geom.iterator(settings, model, threads, include=[product], geometry_library=kernel)
        assert iterator.initialize()
        shape = iterator.get()
    assert triangles_with_identity(shape) == expected
    assert ifcopenshell.util.shape.get_volume(shape.geometry) == pytest.approx(
        ifcopenshell.util.shape.get_volume(reference.geometry), abs=1e-12
    )
    if threads is not None:
        assert not iterator.next()
