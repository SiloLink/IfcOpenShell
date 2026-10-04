"""Exercise repeated planar-face pairs while retaining real opening cuts."""

import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.guid
import ifcopenshell.util.shape


def _axis(model, location=(0.0, 0.0, 0.0), x_axis=(1.0, 0.0, 0.0)):
    return model.createIfcAxis2Placement3D(
        model.createIfcCartesianPoint(tuple(float(v) for v in location)),
        model.createIfcDirection((0.0, 0.0, 1.0)),
        model.createIfcDirection(x_axis),
    )


def _box(model, lower, upper):
    x0, y0, z0 = lower
    x1, y1, z1 = upper
    profile = model.createIfcRectangleProfileDef("AREA", None, None, float(x1 - x0), float(y1 - y0))
    return model.createIfcExtrudedAreaSolid(
        profile,
        _axis(model, ((x0 + x1) / 2.0, (y0 + y1) / 2.0, z0)),
        model.createIfcDirection((0.0, 0.0, 1.0)),
        float(z1 - z0),
    )


def _fixture(opening_bounds, precision=1e-5, rotated=False, concave=False):
    model = ifcopenshell.file(schema="IFC4")
    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, precision, _axis(model), None)
    model.createIfcProject(ifcopenshell.guid.new(), None, "Touching operands", None, None, None, None, (context,), None)
    placement = model.createIfcLocalPlacement(
        None,
        _axis(model, (20.0, -10.0, 4.0), (0.0, 1.0, 0.0)) if rotated else _axis(model),
    )

    def product(ifc_class, item, item_placement):
        representation = model.createIfcShapeRepresentation(context, "Body", "SweptSolid", (item,))
        return model.create_entity(
            ifc_class,
            GlobalId=ifcopenshell.guid.new(),
            ObjectPlacement=item_placement,
            Representation=model.createIfcProductDefinitionShape(None, None, (representation,)),
        )

    host_item = _box(model, (0.0, 0.0, 0.0), (12.0, 1.0, 3.0))
    if concave:
        points = ((0.0, 0.0), (12.0, 0.0), (12.0, 0.5), (6.0, 0.5), (6.0, 1.0), (0.0, 1.0), (0.0, 0.0))
        wire = model.createIfcPolyline(tuple(model.createIfcCartesianPoint(point) for point in points))
        profile = model.createIfcArbitraryClosedProfileDef("AREA", None, wire)
        host_item = model.createIfcExtrudedAreaSolid(
            profile, _axis(model), model.createIfcDirection((0.0, 0.0, 1.0)), 3.0
        )
    host = product("IfcWall", host_item, placement)
    color = model.createIfcColourRgb(None, 0.2, 0.4, 0.8)
    shading = model.createIfcSurfaceStyleShading(color, 0.0)
    style = model.createIfcSurfaceStyle("Host blue", "BOTH", (shading,))
    model.createIfcStyledItem(host_item, (style,), None)
    for lower, upper in opening_bounds:
        opening_placement = model.createIfcLocalPlacement(placement, _axis(model))
        opening = product("IfcOpeningElement", _box(model, lower, upper), opening_placement)
        model.createIfcRelVoidsElement(ifcopenshell.guid.new(), None, None, None, host, opening)
    settings = ifcopenshell.geom.settings()
    settings.set("weld-vertices", True)
    settings.set("precision", precision)
    shape = ifcopenshell.geom.create_shape(settings, host, geometry_library="opencascade")
    return model, shape


def _check_shape(shape, volume):
    mesh = shape.geometry
    vertices = ifcopenshell.util.shape.get_vertices(mesh)
    faces = ifcopenshell.util.shape.get_faces(mesh)
    np.testing.assert_allclose(vertices.min(axis=0), (0.0, 0.0, 0.0), atol=1e-9)
    np.testing.assert_allclose(vertices.max(axis=0), (12.0, 1.0, 3.0), atol=1e-9)
    assert ifcopenshell.util.shape.get_volume(mesh) == pytest.approx(volume, abs=1e-8)
    edges = np.concatenate((faces[:, (0, 1)], faces[:, (1, 2)], faces[:, (2, 0)]))
    _, counts = np.unique(np.sort(edges, axis=1), axis=0, return_counts=True)
    assert np.all(counts == 2)
    assert mesh.materials
    for material in mesh.materials:
        color = material.diffuse
        np.testing.assert_allclose((color.r(), color.g(), color.b()), (0.2, 0.4, 0.8), atol=1e-12)


@pytest.mark.parametrize("reverse", (False, True))
@pytest.mark.parametrize("rotated", (False, True))
def test_many_touching_and_disjoint_openings_leave_host_unchanged(reverse, rotated):
    openings = []
    for x in (1.0, 3.0, 5.0, 7.0, 9.0):
        openings.extend(
            (
                ((x, -1.0, 1.0), (x + 1.0, 0.0, 2.0)),
                ((x, 1.0, 1.0), (x + 1.0, 2.0, 2.0)),
                ((x, 2.0, 1.0), (x + 1.0, 3.0, 2.0)),
            )
        )
    if reverse:
        openings.reverse()
    model, shape = _fixture(openings, rotated=rotated)
    _check_shape(shape, 36.0)
    expected = (
        ((0.0, -1.0, 0.0, 20.0), (1.0, 0.0, 0.0, -10.0), (0.0, 0.0, 1.0, 4.0), (0.0, 0.0, 0.0, 1.0))
        if rotated
        else np.eye(4)
    )
    np.testing.assert_allclose(ifcopenshell.util.shape.get_shape_matrix(shape), expected, atol=1e-12)


@pytest.mark.parametrize("reverse", (False, True))
@pytest.mark.parametrize("precision", (1e-6, 1e-5))
def test_touching_candidates_do_not_remove_real_cuts(reverse, precision):
    openings = []
    for x in (1.0, 4.0, 7.0, 10.0):
        openings.extend(
            (
                ((x, -1.0, 1.0), (x + 0.5, 0.0, 2.0)),
                ((x, -0.5, 1.0), (x + 1.0, 1.5, 2.0)),
                ((x, 1.0, 0.5), (x + 0.5, 2.0, 2.5)),
            )
        )
    if reverse:
        openings.reverse()
    model, shape = _fixture(openings, precision=precision)
    _check_shape(shape, 32.0)


@pytest.mark.parametrize("precision", (1e-6, 1e-5))
@pytest.mark.parametrize("side", (-1.0, 1.0))
def test_small_clearance_and_penetration_keep_existing_distance_predicates(precision, side):
    penetration = 100.0 * precision
    openings = [((x, -1.0, 1.0), (x + 1.0, side * penetration, 2.0)) for x in (1.0, 4.0, 7.0, 10.0)]
    model, shape = _fixture(openings, precision=precision)
    _check_shape(shape, 36.0 - (4.0 * penetration if side > 0.0 else 0.0))


@pytest.mark.parametrize("reverse", (False, True))
def test_concave_host_keeps_cuts_when_other_vertices_are_in_front_of_face(reverse):
    openings = [
        ((8.0, 0.5, 1.0), (9.0, 1.5, 2.0)),
        ((10.0, 0.5, 1.0), (11.0, 1.5, 2.0)),
        ((1.0, 1.0, 1.0), (2.0, 2.0, 2.0)),
        ((4.0, 0.25, 1.0), (7.0, 0.75, 2.0)),
    ]
    if reverse:
        openings.reverse()
    model, shape = _fixture(openings, concave=True)
    _check_shape(shape, 25.75)
