import math

import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.util.shape
from ifcopenshell.util.shape_builder import ShapeBuilder


def settings(weld=True):
    result = ifcopenshell.geom.settings()
    result.set("apply-default-materials", False)
    result.set("weld-vertices", weld)
    result.set("no-normals", True)
    result.set("mesher-linear-deflection", 0.03)
    result.set("mesher-angular-deflection", 1.0)
    return result


def tessellation(style_mode, mapped):
    model = ifcopenshell.file(schema="IFC4")
    origin = model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((0.0, 0.0, 0.0)))
    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, origin, None)
    model.createIfcProject(ifcopenshell.guid.new(), None, "Test", None, None, None, None, (context,), None)
    points = model.createIfcCartesianPointList3D(((0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (2.0, 1.0, 0.0), (0.0, 1.0, 0.0)))
    mesh = model.createIfcTriangulatedFaceSet(points, None, False, ((1, 2, 3), (1, 3, 4)), None)
    if style_mode == "faces":
        colours = model.createIfcColourRgbList(((1.0, 0.0, 0.0), (0.0, 1.0, 0.0)))
        model.createIfcIndexedColourMap(mesh, 0.6, colours, (1, 2))
    elif style_mode == "item":
        colour = model.createIfcColourRgb(None, 0.25, 0.5, 0.75)
        shading = model.createIfcSurfaceStyleShading(colour, 0.2)
        style = model.createIfcSurfaceStyle("Whole item", "BOTH", (shading,))
        model.createIfcStyledItem(mesh, (style,), None)
    representation = model.createIfcShapeRepresentation(context, "Body", "Tessellation", (mesh,))
    if mapped:
        mapping = model.createIfcRepresentationMap(origin, representation)
        target = model.createIfcCartesianTransformationOperator3D(
            model.createIfcDirection((0.0, 1.0, 0.0)),
            model.createIfcDirection((-1.0, 0.0, 0.0)),
            model.createIfcCartesianPoint((3.0, -2.0, 5.0)),
            1.0,
            model.createIfcDirection((0.0, 0.0, 1.0)),
        )
        representation = model.createIfcShapeRepresentation(
            context, "Body", "MappedRepresentation", (model.createIfcMappedItem(mapping, target),)
        )
    return model, representation


def assert_same_mesh(actual, expected):
    for attribute in ("verts", "faces", "edges", "material_ids", "item_ids", "edges_item_ids"):
        assert getattr(actual, attribute) == getattr(expected, attribute), attribute
    np.testing.assert_array_equal(
        ifcopenshell.util.shape.get_material_colors(actual), ifcopenshell.util.shape.get_material_colors(expected)
    )


@pytest.mark.parametrize("mapped", (False, True))
@pytest.mark.parametrize("weld", (False, True))
@pytest.mark.parametrize("kernel", ("hybrid-cgal-simple-opencascade-cgal", "hybrid-cgal-opencascade"))
def test_face_coloured_tessellation_matches_occ_output(kernel, weld, mapped):
    model, representation = tessellation("faces", mapped)
    expected = ifcopenshell.geom.create_shape(settings(weld), representation, geometry_library="opencascade")
    actual = ifcopenshell.geom.create_shape(settings(weld), representation, geometry_library=kernel)
    assert_same_mesh(actual, expected)
    colours = ifcopenshell.util.shape.get_material_colors(actual)
    face_colours = colours[ifcopenshell.util.shape.get_faces_material_style_ids(actual)]
    np.testing.assert_allclose(face_colours, ((1.0, 0.0, 0.0, 0.6), (0.0, 1.0, 0.0, 0.6)))


@pytest.mark.parametrize("mapped", (False, True))
@pytest.mark.parametrize("style_mode", ("none", "item"))
def test_tessellation_without_face_colours_keeps_first_kernel_output(style_mode, mapped):
    model, representation = tessellation(style_mode, mapped)
    expected = ifcopenshell.geom.create_shape(settings(), representation, geometry_library="cgal-simple")
    actual = ifcopenshell.geom.create_shape(
        settings(), representation, geometry_library="hybrid-cgal-simple-opencascade-cgal"
    )
    assert_same_mesh(actual, expected)
    assert len(actual.faces) == 6
    if style_mode == "item":
        np.testing.assert_allclose(ifcopenshell.util.shape.get_material_colors(actual), ((0.25, 0.5, 0.75, 0.8),))
    else:
        assert actual.material_ids == (-1, -1)


@pytest.mark.parametrize("forward", (False, True))
@pytest.mark.parametrize("weld", (False, True))
def test_occ_reversed_face_retains_winding_and_boundary(forward, weld):
    model = ifcopenshell.file(schema="IFC4")
    points = tuple(
        model.createIfcCartesianPoint(point) for point in ((0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (0.0, 3.0, 0.0))
    )
    bound = model.createIfcFaceOuterBound(model.createIfcPolyLoop(points), forward)
    face = model.createIfcFace((bound,))
    surface = model.createIfcShellBasedSurfaceModel((model.createIfcOpenShell((face,)),))
    shape = ifcopenshell.geom.create_shape(settings(weld), surface, geometry_library="opencascade")
    vertices = ifcopenshell.util.shape.get_vertices(shape)
    faces = ifcopenshell.util.shape.get_faces(shape)
    assert len(faces) == 1
    triangle = vertices[faces[0]]
    assert np.cross(triangle[1] - triangle[0], triangle[2] - triangle[0])[2] == pytest.approx(6.0 if forward else -6.0)
    edges = ifcopenshell.util.shape.get_edges(shape)
    assert {tuple(sorted(edge)) for edge in edges} == {
        tuple(sorted((faces[0][i], faces[0][(i + 1) % 3]))) for i in range(3)
    }
    assert shape.material_ids == (-1,)


@pytest.mark.parametrize("profile_kind", ("circle", "void"))
@pytest.mark.parametrize("weld", (False, True))
def test_occ_curved_and_inner_boundary_meshes_keep_outward_volume(profile_kind, weld):
    model = ifcopenshell.file(schema="IFC4")
    builder = ShapeBuilder(model)
    if profile_kind == "circle":
        profile = builder.circle(radius=1.5)
        expected_volume = math.pi * 1.5**2 * 2.0
        tolerance = 0.04
    else:
        profile = builder.profile(
            builder.rectangle((4.0, 3.0)), inner_curves=(builder.rectangle((1.0, 1.0), position=(1.0, 1.0)),)
        )
        expected_volume = 22.0
        tolerance = 1e-12
    solid = builder.extrude(profile, magnitude=2.0, position=(3.0, -2.0, 5.0))
    shape = ifcopenshell.geom.create_shape(settings(weld), solid, geometry_library="opencascade")
    vertices = ifcopenshell.util.shape.get_vertices(shape)
    faces = ifcopenshell.util.shape.get_faces(shape)
    triangles = vertices[faces] - np.array((3.0, -2.0, 5.0))
    volume = np.einsum("ij,ij->i", triangles[:, 0], np.cross(triangles[:, 1], triangles[:, 2])).sum() / 6.0
    assert volume == pytest.approx(expected_volume, rel=tolerance)
    assert np.isfinite(vertices).all()
    assert vertices[:, 2].min() == pytest.approx(5.0)
    assert vertices[:, 2].max() == pytest.approx(7.0)
    assert len(shape.material_ids) == len(faces)
    assert set(shape.item_ids) == {solid.id()}
    assert all(0 <= index < len(vertices) for index in shape.edges)
    assert len(shape.normals) == 0
