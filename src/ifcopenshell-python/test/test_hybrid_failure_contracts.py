# This file was generated with the assistance of an AI coding tool.
import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.util.shape

HYBRID = "hybrid-cgal-simple-opencascade-cgal"


def styled_wall(degenerate, entire_fragment_degenerate=False):
    model = ifcopenshell.file(schema="IFC4")
    origin = model.createIfcCartesianPoint((0.0, 0.0, 0.0))
    axis = model.createIfcAxis2Placement3D(origin, None, None)
    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
    model.createIfcProject(ifcopenshell.guid.new(), None, "Test", None, None, None, None, (context,), None)

    fragment_points = model.createIfcCartesianPointList3D(
        (
            (0.0, 0.0, 0.0),
            (1.0, 0.0, 0.0),
            (1.0, 1.0, 0.0),
            (0.0, 1.0, 0.0),
            (0.0, 0.0, 1.0),
            (1.0, 0.0, 1.0),
            (0.0, 1.0, 1.0),
        )
    )
    fragment = model.createIfcPolygonalFaceSet(
        fragment_points,
        False,
        (
            model.createIfcIndexedPolygonalFace((1, 1, 1) if degenerate else (5, 6, 7)),
            model.createIfcIndexedPolygonalFace((1, 1, 1) if entire_fragment_degenerate else (1, 2, 3, 4)),
        ),
        None,
    )
    palette = model.createIfcColourRgbList(((1.0, 0.0, 0.0), (0.0, 1.0, 0.0)))
    model.createIfcIndexedColourMap(fragment, 0.6, palette, (1, 2))

    slab_points = model.createIfcCartesianPointList3D(
        (
            (2.0, 0.0, 0.0),
            (3.0, 0.0, 0.0),
            (3.0, 1.0, 0.0),
            (2.0, 1.0, 0.0),
            (2.0, 0.0, 1.0),
            (3.0, 0.0, 1.0),
            (3.0, 1.0, 1.0),
            (2.0, 1.0, 1.0),
        )
    )
    slab = model.createIfcPolygonalFaceSet(
        slab_points,
        True,
        tuple(
            model.createIfcIndexedPolygonalFace(indices)
            for indices in ((1, 4, 3, 2), (5, 6, 7, 8), (1, 2, 6, 5), (4, 8, 7, 3), (1, 5, 8, 4), (2, 3, 7, 6))
        ),
        None,
    )
    blue = model.createIfcColourRgbList(((0.0, 0.0, 1.0),))
    model.createIfcIndexedColourMap(slab, 0.8, blue, (1, 1, 1, 1, 1, 1))
    representation = model.createIfcShapeRepresentation(context, "Body", "Tessellation", (fragment, slab))
    wall = model.createIfcWall(
        ifcopenshell.guid.new(),
        None,
        "Styled wall",
        None,
        None,
        model.createIfcLocalPlacement(None, axis),
        model.createIfcProductDefinitionShape(None, None, (representation,)),
        None,
        None,
    )
    return model, wall, fragment, slab


@pytest.mark.parametrize("degenerate", [False, True])
@pytest.mark.parametrize("entrypoint", ["create_shape", "iterator"])
def test_hybrid_preserves_surviving_geometry_and_its_face_colours(degenerate, entrypoint):
    model, wall, fragment, slab = styled_wall(degenerate)
    settings = ifcopenshell.geom.settings()
    settings.set("apply-default-materials", False)
    if entrypoint == "create_shape":
        shape = ifcopenshell.geom.create_shape(settings, wall, geometry_library=HYBRID)
    else:
        iterator = ifcopenshell.geom.iterator(settings, model, include=[wall], num_threads=2, geometry_library=HYBRID)
        assert iterator.initialize()
        shape = iterator.get()

    geometry = shape.geometry
    vertices = ifcopenshell.util.shape.get_vertices(geometry)
    faces = ifcopenshell.util.shape.get_faces(geometry)
    items = ifcopenshell.util.shape.get_faces_representation_item_ids(geometry)
    materials = ifcopenshell.util.shape.get_material_colors(geometry)
    material_ids = ifcopenshell.util.shape.get_faces_material_style_ids(geometry)
    assert np.isfinite(vertices).all()
    assert len(faces) == (14 if degenerate else 15)
    assert all(material_id >= 0 for material_id in material_ids)
    assert list(items).count(slab.id()) == 12
    for face, item_id, material_id in zip(faces, items, material_ids):
        colour = tuple(round(float(channel), 6) for channel in materials[material_id])
        if item_id == slab.id():
            assert colour == (0.0, 0.0, 1.0, 0.8)
            assert np.all(vertices[face, 0] >= 2.0)
        else:
            assert item_id == fragment.id()
            if np.all(vertices[face, 2] == 0.0):
                assert colour == (0.0, 1.0, 0.0, 0.6)
            else:
                assert not degenerate
                assert colour == (1.0, 0.0, 0.0, 0.6)
    if entrypoint == "iterator":
        assert not iterator.next()


@pytest.mark.parametrize("fragment_first", [False, True])
@pytest.mark.parametrize("entrypoint", ["create_shape", "iterator"])
def test_last_colour_capable_backend_keeps_healthy_sibling(fragment_first, entrypoint):
    model, wall, fragment, slab = styled_wall(True, entire_fragment_degenerate=True)
    wall.Representation.Representations[0].Items = (fragment, slab) if fragment_first else (slab, fragment)
    settings = ifcopenshell.geom.settings()
    settings.set("apply-default-materials", False)
    if entrypoint == "create_shape":
        shape = ifcopenshell.geom.create_shape(settings, wall, geometry_library=HYBRID)
    else:
        iterator = ifcopenshell.geom.iterator(settings, model, include=[wall], num_threads=2, geometry_library=HYBRID)
        assert iterator.initialize()
        shape = iterator.get()

    geometry = shape.geometry
    assert np.isfinite(ifcopenshell.util.shape.get_vertices(geometry)).all()
    assert len(ifcopenshell.util.shape.get_faces(geometry)) == 12
    assert set(ifcopenshell.util.shape.get_faces_representation_item_ids(geometry)) == {slab.id()}
    colours = ifcopenshell.util.shape.get_material_colors(geometry)
    for material_id in ifcopenshell.util.shape.get_faces_material_style_ids(geometry):
        assert material_id >= 0
        assert tuple(round(float(channel), 6) for channel in colours[material_id]) == (0.0, 0.0, 1.0, 0.8)
    if entrypoint == "iterator":
        assert not iterator.next()
