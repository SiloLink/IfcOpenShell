import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.guid
import ifcopenshell.util.shape

PRECISION = 1e-5


def _placement(ifc_file, location=(0.0, 0.0, 0.0), x_axis=(1.0, 0.0, 0.0), relative_to=None):
    axis = ifc_file.createIfcAxis2Placement3D(
        ifc_file.createIfcCartesianPoint(location),
        ifc_file.createIfcDirection((0.0, 0.0, 1.0)),
        ifc_file.createIfcDirection(x_axis),
    )
    return ifc_file.createIfcLocalPlacement(relative_to, axis)


def _model():
    ifc_file = ifcopenshell.file(schema="IFC4")
    placement = _placement(ifc_file)
    context = ifc_file.createIfcGeometricRepresentationContext(
        None, "Model", 3, PRECISION, placement.RelativePlacement, None
    )
    ifc_file.createIfcProject(ifcopenshell.guid.new(), None, "Openings", None, None, None, None, (context,), None)
    return ifc_file, context


def _box(ifc_file, lower, upper, closed=True):
    x0, y0, z0 = lower
    x1, y1, z1 = upper
    points = ifc_file.createIfcCartesianPointList3D(
        (
            (x0, y0, z0),
            (x1, y0, z0),
            (x1, y1, z0),
            (x0, y1, z0),
            (x0, y0, z1),
            (x1, y0, z1),
            (x1, y1, z1),
            (x0, y1, z1),
        )
    )
    indices = (
        (1, 4, 3, 2),
        (5, 6, 7, 8),
        (1, 2, 6, 5),
        (4, 8, 7, 3),
        (1, 5, 8, 4),
        (2, 3, 7, 6),
    )
    faces = tuple(ifc_file.createIfcIndexedPolygonalFace(face) for face in (indices if closed else indices[:1]))
    return ifc_file.createIfcPolygonalFaceSet(points, closed, faces, None)


def _product(ifc_file, context, ifc_class, items, placement):
    representation = ifc_file.createIfcShapeRepresentation(context, "Body", "Tessellation", items)
    return ifc_file.create_entity(
        ifc_class,
        GlobalId=ifcopenshell.guid.new(),
        ObjectPlacement=placement,
        Representation=ifc_file.createIfcProductDefinitionShape(None, None, (representation,)),
    )


def _opening(ifc_file, context, host, item, placement=None):
    if placement is None:
        placement = _placement(ifc_file, relative_to=host.ObjectPlacement)
    opening = _product(ifc_file, context, "IfcOpeningElement", (item,), placement)
    ifc_file.createIfcRelVoidsElement(ifcopenshell.guid.new(), None, None, None, host, opening)


def _shape(host):
    settings = ifcopenshell.geom.settings()
    settings.set("weld-vertices", True)
    settings.set("precision", PRECISION)
    return ifcopenshell.geom.create_shape(settings, host, geometry_library="cgal")


def _assert_closed_mesh(geometry):
    faces = ifcopenshell.util.shape.get_faces(geometry)
    assert len(faces) > 0
    edges = np.concatenate((faces[:, (0, 1)], faces[:, (1, 2)], faces[:, (2, 0)]))
    _, counts = np.unique(np.sort(edges, axis=1), axis=0, return_counts=True)
    assert np.all(counts == 2)


def test_exact_cgal_unions_overlapping_openings_and_keeps_disjoint_holes():
    ifc_file, context = _model()
    host_item = _box(ifc_file, (0.0, 0.0, 0.0), (6.0, 1.0, 3.0))
    host = _product(ifc_file, context, "IfcWall", (host_item,), _placement(ifc_file))
    for lower, upper in (
        ((1.0, -0.5, 0.5), (3.0, 1.5, 1.5)),
        ((2.0, -0.5, 0.5), (4.0, 1.5, 1.5)),
        ((4.5, -0.5, 2.0), (5.5, 1.5, 2.5)),
        ((20.0, -0.5, 0.5), (21.0, 1.5, 1.5)),
    ):
        _opening(ifc_file, context, host, _box(ifc_file, lower, upper))

    shape = _shape(host)
    geometry = shape.geometry
    overlap_union = (3.0 + 2 * PRECISION) * (1.0 + 2 * PRECISION)
    disjoint_hole = (1.0 + 2 * PRECISION) * (0.5 + 2 * PRECISION)
    assert ifcopenshell.util.shape.get_volume(geometry) == pytest.approx(18.0 - overlap_union - disjoint_hole, abs=1e-8)
    vertices = ifcopenshell.util.shape.get_vertices(geometry)
    np.testing.assert_allclose(vertices.min(axis=0), (0.0, 0.0, 0.0), atol=1e-9)
    np.testing.assert_allclose(vertices.max(axis=0), (6.0, 1.0, 3.0), atol=1e-9)
    assert set(geometry.item_ids) == {host_item.id()}
    _assert_closed_mesh(geometry)


def test_exact_cgal_preserves_host_parts_and_transforms_when_an_opening_is_invalid():
    ifc_file, context = _model()
    host_placement = _placement(ifc_file, (10.0, 20.0, 2.0), x_axis=(0.0, 1.0, 0.0))
    host_items = (
        _box(ifc_file, (0.0, 0.0, 0.0), (3.0, 1.0, 3.0)),
        _box(ifc_file, (4.0, 0.0, 0.0), (6.0, 1.0, 3.0)),
    )
    host = _product(ifc_file, context, "IfcWall", host_items, host_placement)
    _opening(ifc_file, context, host, _box(ifc_file, (0.0, -0.5, 0.5), (6.0, 1.5, 2.5), closed=False))
    opening_placement = _placement(ifc_file, (2.0, -0.5, 1.0), x_axis=(0.0, 1.0, 0.0), relative_to=host_placement)
    _opening(
        ifc_file,
        context,
        host,
        _box(ifc_file, (0.0, 0.0, 0.0), (2.0, 1.0, 1.0)),
        opening_placement,
    )
    _opening(ifc_file, context, host, _box(ifc_file, (4.5, -0.5, 1.0), (5.5, 1.5, 2.0)))

    shape = _shape(host)
    geometry = shape.geometry
    assert ifcopenshell.util.shape.get_volume(geometry) == pytest.approx(
        15.0 - 2 * (1.0 + 2 * PRECISION) ** 2, abs=1e-8
    )
    np.testing.assert_allclose(
        ifcopenshell.util.shape.get_shape_matrix(shape),
        ((0.0, -1.0, 0.0, 10.0), (1.0, 0.0, 0.0, 20.0), (0.0, 0.0, 1.0, 2.0), (0.0, 0.0, 0.0, 1.0)),
        atol=1e-9,
    )
    assert set(geometry.item_ids) == {item.id() for item in host_items}
    vertices = ifcopenshell.util.shape.get_vertices(geometry)
    np.testing.assert_allclose(vertices.min(axis=0), (0.0, 0.0, 0.0), atol=1e-9)
    np.testing.assert_allclose(vertices.max(axis=0), (6.0, 1.0, 3.0), atol=1e-9)
    _assert_closed_mesh(geometry)
