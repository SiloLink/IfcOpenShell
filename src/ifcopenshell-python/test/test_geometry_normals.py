import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom

KERNELS = ("cgal-simple", "cgal", "hybrid-cgal-simple-opencascade-cgal")


def make_geometry(kind, placed):
    model = ifcopenshell.file(schema="IFC4")
    origin = model.create_entity("IfcCartesianPoint", (0.0, 0.0, 0.0))
    axis = model.create_entity("IfcAxis2Placement3D", Location=origin)
    if kind == "box":
        profile = model.create_entity("IfcRectangleProfileDef", ProfileType="AREA", XDim=2.0, YDim=3.0)
        direction = model.create_entity("IfcDirection", (0.0, 0.0, 1.0))
        item = model.create_entity(
            "IfcExtrudedAreaSolid", SweptArea=profile, Position=axis, ExtrudedDirection=direction, Depth=4.0
        )
    else:
        points = model.create_entity(
            "IfcCartesianPointList3D",
            CoordList=((0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (0.0, 3.0, 0.0), (0.0, 0.0, 4.0)),
        )
        item = model.create_entity(
            "IfcTriangulatedFaceSet",
            Coordinates=points,
            Closed=True,
            CoordIndex=((1, 3, 2), (1, 2, 4), (2, 3, 4), (3, 1, 4)),
        )

    colour = model.create_entity("IfcColourRgb", Red=0.2, Green=0.4, Blue=0.6)
    shading = model.create_entity("IfcSurfaceStyleShading", SurfaceColour=colour)
    style = model.create_entity("IfcSurfaceStyle", Name="Mesh colour", Side="BOTH", Styles=(shading,))
    model.create_entity("IfcStyledItem", Item=item, Styles=(style,))
    source_item_id = item.id()

    if placed:
        context = model.create_entity(
            "IfcGeometricRepresentationContext", CoordinateSpaceDimension=3, WorldCoordinateSystem=axis
        )
        representation = model.create_entity(
            "IfcShapeRepresentation",
            ContextOfItems=context,
            RepresentationIdentifier="Body",
            RepresentationType="SweptSolid" if kind == "box" else "Tessellation",
            Items=(item,),
        )
        mapping = model.create_entity("IfcRepresentationMap", MappingOrigin=axis, MappedRepresentation=representation)
        target = model.create_entity(
            "IfcCartesianTransformationOperator3D",
            Axis1=model.create_entity("IfcDirection", (0.0, 1.0, 0.0)),
            Axis2=model.create_entity("IfcDirection", (-1.0, 0.0, 0.0)),
            Axis3=model.create_entity("IfcDirection", (0.0, 0.0, 1.0)),
            LocalOrigin=model.create_entity("IfcCartesianPoint", (7.0, -3.0, 2.0)),
            Scale=1.0,
        )
        item = model.create_entity("IfcMappedItem", MappingSource=mapping, MappingTarget=target)
    return model, item, source_item_id


def convert(item, kernel, weld, no_normals):
    settings = ifcopenshell.geom.settings()
    settings.set("weld-vertices", weld)
    settings.set("no-normals", no_normals)
    return ifcopenshell.geom.create_shape(settings, item, geometry_library=kernel)


@pytest.mark.parametrize("kernel", KERNELS)
@pytest.mark.parametrize("kind", ("box", "tetrahedron"))
@pytest.mark.parametrize("weld", (False, True))
@pytest.mark.parametrize("placed", (False, True))
def test_no_normals_preserves_geometry(kernel, kind, weld, placed):
    model, item, source_item_id = make_geometry(kind, placed)
    with_normals = convert(item, kernel, weld, False)
    without_normals = convert(item, kernel, weld, True)

    for attribute in ("verts", "faces", "edges", "material_ids", "item_ids", "edges_item_ids"):
        assert getattr(without_normals, attribute) == getattr(with_normals, attribute), attribute
    assert without_normals.faces
    assert without_normals.edges
    assert set(without_normals.item_ids) == {source_item_id}
    assert set(without_normals.edges_item_ids) == {source_item_id}
    assert set(without_normals.material_ids) == {0}
    assert len(without_normals.materials) == len(with_normals.materials) == 1
    for mesh in (with_normals, without_normals):
        diffuse = mesh.materials[0].diffuse
        assert (diffuse.r(), diffuse.g(), diffuse.b()) == pytest.approx((0.2, 0.4, 0.6))

    vertices = np.asarray(without_normals.verts).reshape(-1, 3)
    assert len(vertices) == ({"box": 8, "tetrahedron": 4} if weld else {"box": 24, "tetrahedron": 12})[kind]
    assert len(without_normals.faces) // 3 == {"box": 12, "tetrahedron": 4}[kind]
    expected_min, expected_max = (
        ((-1.0, -1.5, 0.0), (1.0, 1.5, 4.0))
        if kind == "box"
        else (
            (0.0, 0.0, 0.0),
            (2.0, 3.0, 4.0),
        )
    )
    if placed:
        expected_min, expected_max = (
            (7.0 - expected_max[1], -3.0 + expected_min[0], 2.0 + expected_min[2]),
            (7.0 - expected_min[1], -3.0 + expected_max[0], 2.0 + expected_max[2]),
        )
    np.testing.assert_allclose(vertices.min(axis=0), expected_min)
    np.testing.assert_allclose(vertices.max(axis=0), expected_max)
    assert with_normals.normals
    assert without_normals.normals == ()
