import functools
import itertools
import math
import multiprocessing
import operator
import os
from typing import get_args

import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.api.context
import ifcopenshell.api.owner.settings
import ifcopenshell.api.project
import ifcopenshell.api.root
import ifcopenshell.api.unit
import ifcopenshell.geom
import ifcopenshell.ifcopenshell_wrapper as W
import ifcopenshell.util.shape
import test.bootstrap
from ifcopenshell.util.shape_builder import ShapeBuilder

fn = os.path.join(os.path.dirname(__file__), "fixtures/ColumnPSetsOfSets.ifc")


class TestGeomSettings:
    def test_settings(self):
        settings = ifcopenshell.geom.settings()
        assert set(get_args(ifcopenshell.geom.SETTING)) == set(settings.setting_names()), (
            "Also need to update IfcPython.i, if new settings were added/removed."
        )

        assert "use-python-opencascade" in settings.setting_names()
        assert settings.get(settings.USE_PYTHON_OPENCASCADE) is False
        assert settings.get("use-python-opencascade") is False
        assert "USE_PYTHON_OPENCASCADE = False" in repr(settings)

        # Testing both new and old ways of setting geometry settings.
        if ifcopenshell.geom.has_occ:
            settings.set("use-python-opencascade", True)
            settings.set(settings.USE_PYTHON_OPENCASCADE, True)
            assert settings.get(settings.USE_PYTHON_OPENCASCADE) is True
            assert "USE_PYTHON_OPENCASCADE = True" in repr(settings)
        else:
            with pytest.raises(AttributeError):
                settings.set("use-python-opencascade", True)
            with pytest.raises(AttributeError):
                settings.set(settings.USE_PYTHON_OPENCASCADE, True)
            assert "USE_PYTHON_OPENCASCADE = False" in repr(settings)

        settings.set("base-uri", "https://example.test/")
        assert settings.get("base-uri") == "https://example.test/"


class TestTriangulationAttributes(test.bootstrap.IFC4):
    def test_faces_representation_item_ids(self):
        ifc_file = ifcopenshell.file()
        ifcopenshell.api.root.create_entity(ifc_file, ifc_class="IfcProject", name="Test")
        context = ifcopenshell.api.context.add_context(ifc_file, context_type="Model")

        builder = ShapeBuilder(ifc_file)
        extrusion = builder.extrude(builder.rectangle(), magnitude=1.0)
        representation = builder.get_representation(context, extrusion)
        settings = ifcopenshell.geom.settings()
        shape = ifcopenshell.geom.create_shape(settings, representation)
        faces_item_ids = ifcopenshell.util.shape.get_faces_representation_item_ids(shape)
        faces = ifcopenshell.util.shape.get_faces(shape)
        assert set(faces_item_ids) == {extrusion.id()}
        assert len(faces) == 12  # Cube has 12 tris.
        assert len(faces_item_ids) == len(faces)

        edges_item_ids = ifcopenshell.util.shape.get_edges_representation_item_ids(shape)
        edges = ifcopenshell.util.shape.get_edges(shape)
        assert set(edges_item_ids) == {extrusion.id()}
        assert len(edges) == 12  # Cube has 12 edges.
        assert len(edges_item_ids) == len(edges)

    @pytest.mark.parametrize(
        "geometry_library",
        ("opencascade", "hybrid-cgal-simple-opencascade-cgal"),
    )
    def test_indexed_colours_follow_polygon_faces_after_triangulation(self, geometry_library):
        ifc_file = ifcopenshell.file(schema="IFC4")
        project = ifc_file.createIfcProject(ifcopenshell.guid.new(), None, "Test", None, None, None, None, None, None)
        origin = ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0))
        axis = ifc_file.createIfcAxis2Placement3D(origin, None, None)
        context = ifc_file.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
        project.RepresentationContexts = [context]

        points = ifc_file.createIfcCartesianPointList3D(
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
        polygonal_faces = [
            ifc_file.createIfcIndexedPolygonalFace((1, 2, 3, 4)),
            ifc_file.createIfcIndexedPolygonalFace((5, 6, 7)),
        ]
        face_set = ifc_file.createIfcPolygonalFaceSet(points, False, polygonal_faces, None)
        colours = ifc_file.createIfcColourRgbList(((1.0, 0.0, 0.0), (0.0, 1.0, 0.0)))
        ifc_file.createIfcIndexedColourMap(face_set, 0.6, colours, (1, 2))
        representation = ifc_file.createIfcShapeRepresentation(context, "Body", "Tessellation", (face_set,))

        settings = ifcopenshell.geom.settings()
        settings.set("apply-default-materials", False)
        shape = ifcopenshell.geom.create_shape(settings, representation, geometry_library=geometry_library)

        faces = ifcopenshell.util.shape.get_faces(shape)
        material_ids = ifcopenshell.util.shape.get_faces_material_style_ids(shape)
        materials = ifcopenshell.util.shape.get_material_colors(shape)
        face_colours = [tuple(round(float(channel), 6) for channel in materials[index]) for index in material_ids]

        assert len(faces) == 3
        assert set(ifcopenshell.util.shape.get_faces_representation_item_ids(shape)) == {face_set.id()}
        assert sorted(face_colours) == [(0.0, 1.0, 0.0, 0.6), (1.0, 0.0, 0.0, 0.6), (1.0, 0.0, 0.0, 0.6)]

    def test_indexed_colours_survive_opening_subtraction(self):
        def make_face_set(coordinates):
            points = ifc_file.createIfcCartesianPointList3D(coordinates)
            faces = tuple(
                ifc_file.createIfcIndexedPolygonalFace(indices)
                for indices in (
                    (1, 4, 3, 2),
                    (5, 6, 7, 8),
                    (1, 2, 6, 5),
                    (4, 8, 7, 3),
                    (1, 5, 8, 4),
                    (2, 3, 7, 6),
                )
            )
            return ifc_file.createIfcPolygonalFaceSet(points, True, faces, None)

        ifc_file = ifcopenshell.file(schema="IFC4")
        origin = ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0))
        axis = ifc_file.createIfcAxis2Placement3D(origin, None, None)
        context = ifc_file.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
        ifc_file.createIfcProject(ifcopenshell.guid.new(), None, "Test", None, None, None, None, (context,), None)
        placement = ifc_file.createIfcLocalPlacement(None, axis)
        host = make_face_set(
            (
                (0.0, 0.0, 0.0),
                (4.0, 0.0, 0.0),
                (4.0, 1.0, 0.0),
                (0.0, 1.0, 0.0),
                (0.0, 0.0, 3.0),
                (4.0, 0.0, 3.0),
                (4.0, 1.0, 3.0),
                (0.0, 1.0, 3.0),
            )
        )
        palette = (
            (1.0, 0.0, 0.0),
            (0.0, 1.0, 0.0),
            (0.0, 0.0, 1.0),
            (1.0, 1.0, 0.0),
            (1.0, 0.0, 1.0),
            (0.0, 1.0, 1.0),
        )
        colours = ifc_file.createIfcColourRgbList(palette)
        ifc_file.createIfcIndexedColourMap(host, 0.8, colours, (1, 2, 3, 4, 5, 6))
        host_representation = ifc_file.createIfcShapeRepresentation(context, "Body", "Tessellation", (host,))
        wall = ifc_file.createIfcWall(
            ifcopenshell.guid.new(),
            None,
            "Wall",
            None,
            None,
            placement,
            ifc_file.createIfcProductDefinitionShape(None, None, (host_representation,)),
            None,
            None,
        )
        void = make_face_set(
            (
                (1.0, -0.1, 1.0),
                (3.0, -0.1, 1.0),
                (3.0, 1.1, 1.0),
                (1.0, 1.1, 1.0),
                (1.0, -0.1, 2.0),
                (3.0, -0.1, 2.0),
                (3.0, 1.1, 2.0),
                (1.0, 1.1, 2.0),
            )
        )
        void_representation = ifc_file.createIfcShapeRepresentation(context, "Body", "Tessellation", (void,))
        opening = ifc_file.createIfcOpeningElement(
            ifcopenshell.guid.new(),
            None,
            "Opening",
            None,
            None,
            ifc_file.createIfcLocalPlacement(placement, axis),
            ifc_file.createIfcProductDefinitionShape(None, None, (void_representation,)),
            None,
            None,
        )
        ifc_file.createIfcRelVoidsElement(ifcopenshell.guid.new(), None, None, None, wall, opening)

        settings = ifcopenshell.geom.settings()
        settings.set("apply-default-materials", False)
        shape = ifcopenshell.geom.create_shape(
            settings,
            wall,
            geometry_library="hybrid-cgal-simple-opencascade-cgal",
        )
        material_ids = ifcopenshell.util.shape.get_faces_material_style_ids(shape.geometry)
        materials = ifcopenshell.util.shape.get_material_colors(shape.geometry)
        vertices = ifcopenshell.util.shape.get_vertices(shape.geometry)
        faces = ifcopenshell.util.shape.get_faces(shape.geometry)
        actual = {
            tuple(round(float(channel), 6) for channel in materials[index]) for index in material_ids if index >= 0
        }
        expected = {(*colour, 0.8) for colour in palette}

        boundary_colours = []
        for axis_index, coordinate in (
            (2, 0.0),
            (2, 3.0),
            (1, 0.0),
            (1, 1.0),
            (0, 0.0),
            (0, 4.0),
        ):
            boundary_colours.append(
                {
                    tuple(round(float(channel), 6) for channel in materials[material_id])
                    for face, material_id in zip(faces, material_ids)
                    if material_id >= 0
                    and all(abs(float(vertices[index][axis_index]) - coordinate) < 1e-6 for index in face)
                }
            )

        assert len(material_ids) == 32
        assert -1 in material_ids
        assert actual == expected
        assert boundary_colours == [{(*colour, 0.8)} for colour in palette]

    def test_indexed_colours_survive_opening_processing_for_nearly_coincident_face_set(self):
        ifc_file = ifcopenshell.file(schema="IFC4")
        origin = ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0))
        axis = ifc_file.createIfcAxis2Placement3D(origin, None, None)
        context = ifc_file.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
        ifc_file.createIfcProject(ifcopenshell.guid.new(), None, "Test", None, None, None, None, (context,), None)
        placement = ifc_file.createIfcLocalPlacement(None, axis)

        coordinates = (
            (1.640000408390691, 0.022681824835978, 0.557061086792769),
            (1.640000408390691, 0.018927687707254, 0.557061086792769),
            (1.740000659060463, 0.018927687707262, 0.557061086792769),
            (1.740000659060463, 0.022681824835978, 0.557061086792769),
            (1.640000408390691, 0.018927687707254, 0.520538737857773),
            (1.640000408390691, 0.022681824835978, 0.524042694680803),
            (1.640000408390698, 0.023010180472617, 0.524005324449751),
            (1.640000408390691, 0.104999999999997, 0.514670205772497),
            (1.640000408390691, 0.104999999999997, 0.489744853696384),
            (1.640000408390691, 0.101241721424906, 0.489744853696384),
            (1.640000408390691, 0.101241721424906, 0.511166721110848),
            (1.640000408390691, 0.023000593718599, 0.520075075873156),
            (4.120165585406163, 0.018927687707262, 0.520538737857773),
            (4.120165585406163, 0.018927687707262, 0.557061086792769),
            (3.605000145273927, 0.018927687707262, 0.557061086792769),
            (3.605000145273927, 0.018927687707262, 0.53),
            (1.740000659060463, 0.018927687707262, 0.53),
            (1.740000659060463, 0.022681824835978, 0.53),
            (3.605000145273927, 0.022681824835978, 0.53),
            (3.605000145273927, 0.022681824835978, 0.557061086792769),
            (4.120165585406163, 0.022681824835978, 0.557061086792769),
            (4.120165585406163, 0.022681824835978, 0.524042694680803),
            (4.120165585406163, 0.023025586176352, 0.524003573717252),
            (3.969999857057502, 0.023024653412378, 0.524003679923738),
            (3.969999347862419, 0.104999999999997, 0.514670205772497),
            (3.969999347862419, 0.104999999999997, 0.489744853696384),
            (3.969999371207209, 0.101241721424906, 0.489744853696384),
            (3.969999371207209, 0.101241721424906, 0.511166721110848),
            (3.969999857057502, 0.023024653412378, 0.5200722736815),
            (4.120165585406156, 0.023000041888999, 0.520075075873156),
        )
        face_indices = (
            (1, 2, 3, 4),
            (5, 2, 1, 6, 7, 8, 9, 10, 11, 12),
            (2, 5, 13, 14, 15, 16, 17, 3),
            (3, 17, 18, 4),
            (6, 1, 4, 18, 19, 20, 21, 22),
            (7, 6, 22, 23),
            (8, 7, 23, 24, 25),
            (9, 8, 25, 26),
            (10, 9, 26, 27),
            (11, 10, 27, 28),
            (12, 11, 28, 29, 30),
            (5, 12, 30, 13),
            (30, 23, 22, 21, 14, 13),
            (15, 14, 21, 20),
            (15, 20, 19, 16),
            (16, 19, 18, 17),
            (23, 30, 29, 24),
            (26, 25, 24, 29, 28, 27),
        )
        points = ifc_file.createIfcCartesianPointList3D(coordinates)
        faces = tuple(ifc_file.createIfcIndexedPolygonalFace(indices) for indices in face_indices)
        face_set = ifc_file.createIfcPolygonalFaceSet(points, True, faces, None)
        colours = ifc_file.createIfcColourRgbList(((1.0, 1.0, 1.0), (0.47, 0.52, 0.47), (0.69, 0.59, 0.48)))
        ifc_file.createIfcIndexedColourMap(
            face_set,
            1.0,
            colours,
            (1, 2, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 1, 1),
        )
        representation = ifc_file.createIfcShapeRepresentation(context, "Body", "Tessellation", (face_set,))
        wall = ifc_file.createIfcWall(
            ifcopenshell.guid.new(),
            None,
            "Wall",
            None,
            None,
            placement,
            ifc_file.createIfcProductDefinitionShape(None, None, (representation,)),
            None,
            None,
        )

        opening_points = ifc_file.createIfcCartesianPointList3D(
            (
                (2.0, 0.0, 0.50),
                (2.5, 0.0, 0.50),
                (2.5, 0.12, 0.50),
                (2.0, 0.12, 0.50),
                (2.0, 0.0, 0.55),
                (2.5, 0.0, 0.55),
                (2.5, 0.12, 0.55),
                (2.0, 0.12, 0.55),
            )
        )
        opening_faces = tuple(
            ifc_file.createIfcIndexedPolygonalFace(indices)
            for indices in (
                (1, 4, 3, 2),
                (5, 6, 7, 8),
                (1, 2, 6, 5),
                (4, 8, 7, 3),
                (1, 5, 8, 4),
                (2, 3, 7, 6),
            )
        )
        opening_face_set = ifc_file.createIfcPolygonalFaceSet(opening_points, True, opening_faces, None)
        opening_representation = ifc_file.createIfcShapeRepresentation(
            context, "Body", "Tessellation", (opening_face_set,)
        )
        opening = ifc_file.createIfcOpeningElement(
            ifcopenshell.guid.new(),
            None,
            "Opening",
            None,
            None,
            ifc_file.createIfcLocalPlacement(placement, axis),
            ifc_file.createIfcProductDefinitionShape(None, None, (opening_representation,)),
            None,
            None,
        )
        ifc_file.createIfcRelVoidsElement(ifcopenshell.guid.new(), None, None, None, wall, opening)

        settings = ifcopenshell.geom.settings()
        settings.set("apply-default-materials", False)
        shape = ifcopenshell.geom.create_shape(
            settings,
            wall,
            geometry_library="hybrid-cgal-simple-opencascade-cgal",
        ).geometry
        material_ids = ifcopenshell.util.shape.get_faces_material_style_ids(shape)

        assert len(ifcopenshell.util.shape.get_faces(shape)) == 54
        assert set(material_ids) == {0, 1, 2}

    def test_indexed_colours_follow_triangulated_faces(self):
        ifc_file = ifcopenshell.file(schema="IFC4")
        project = ifc_file.createIfcProject(ifcopenshell.guid.new(), None, "Test", None, None, None, None, None, None)
        origin = ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0))
        axis = ifc_file.createIfcAxis2Placement3D(origin, None, None)
        context = ifc_file.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
        project.RepresentationContexts = [context]

        points = ifc_file.createIfcCartesianPointList3D(
            (
                (0.0, 0.0, 0.0),
                (1.0, 0.0, 0.0),
                (0.0, 1.0, 0.0),
                (1.0, 1.0, 0.0),
            )
        )
        face_set = ifc_file.createIfcTriangulatedFaceSet(points, None, False, ((1, 2, 3), (2, 4, 3)), None)
        colours = ifc_file.createIfcColourRgbList(((1.0, 0.0, 0.0), (0.0, 1.0, 0.0)))
        ifc_file.createIfcIndexedColourMap(face_set, 0.6, colours, (1, 2))
        representation = ifc_file.createIfcShapeRepresentation(context, "Body", "Tessellation", (face_set,))

        settings = ifcopenshell.geom.settings()
        settings.set("apply-default-materials", False)
        shape = ifcopenshell.geom.create_shape(settings, representation)

        material_ids = ifcopenshell.util.shape.get_faces_material_style_ids(shape)
        materials = ifcopenshell.util.shape.get_material_colors(shape)
        face_colours = [tuple(round(float(channel), 6) for channel in materials[index]) for index in material_ids]

        assert face_colours == [(1.0, 0.0, 0.0, 0.6), (0.0, 1.0, 0.0, 0.6)]

    def test_indexed_colours_do_not_change_polygonal_face_set_geometry(self):
        def create_shape(with_colours):
            ifc_file = ifcopenshell.file(schema="IFC4")
            project = ifc_file.createIfcProject(
                ifcopenshell.guid.new(), None, "Test", None, None, None, None, None, None
            )
            origin = ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0))
            axis = ifc_file.createIfcAxis2Placement3D(origin, None, None)
            context = ifc_file.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis, None)
            project.RepresentationContexts = [context]

            profile = (
                (10.595, 0.0),
                (10.595, 2.135),
                (9.585, 2.135),
                (9.585, 0.0),
                (0.35, 0.0),
                (0.35, 2.96),
                (13.825, 2.96),
                (13.825, 0.0),
            )
            points = ifc_file.createIfcCartesianPointList3D(tuple((x, y, z) for y in (0.2, 0.0) for x, z in profile))
            face_indices = (
                (1, 2, 3, 4, 5, 6, 7, 8),
                (2, 1, 9, 10),
                (3, 2, 10, 11),
                (4, 3, 11, 12),
                (4, 12, 13, 5),
                (14, 6, 5, 13),
                (7, 6, 14, 15),
                (7, 15, 16, 8),
                (9, 1, 8, 16),
                (10, 9, 16, 15, 14, 13, 12, 11),
            )
            faces = [ifc_file.createIfcIndexedPolygonalFace(indices) for indices in face_indices]
            face_set = ifc_file.createIfcPolygonalFaceSet(points, True, faces, None)
            if with_colours:
                colours = ifc_file.createIfcColourRgbList(
                    ((0.635294, 0.772549, 0.843137), (0.592157, 0.584314, 0.572549))
                )
                ifc_file.createIfcIndexedColourMap(
                    face_set,
                    1.0,
                    colours,
                    (1, 2, 2, 2, 1, 1, 1, 1, 1, 1),
                )
            representation = ifc_file.createIfcShapeRepresentation(context, "Body", "Tessellation", (face_set,))
            settings = ifcopenshell.geom.settings()
            settings.set("apply-default-materials", False)
            return ifcopenshell.geom.create_shape(settings, representation)

        def triangle_coordinates(shape):
            vertices = ifcopenshell.util.shape.get_vertices(shape)
            return sorted(
                tuple(
                    sorted(
                        tuple(round(float(channel), 9) for channel in vertices[vertex_index]) for vertex_index in face
                    )
                )
                for face in ifcopenshell.util.shape.get_faces(shape)
            )

        uncoloured = create_shape(False)
        coloured = create_shape(True)

        assert len(ifcopenshell.util.shape.get_faces(uncoloured)) == 28
        assert len(ifcopenshell.util.shape.get_faces(coloured)) == 28
        assert triangle_coordinates(coloured) == triangle_coordinates(uncoloured)
        assert set(ifcopenshell.util.shape.get_faces_material_style_ids(coloured)) == {0, 1}

    def test_curve_representation_item_ids(self):
        ifc_file = ifcopenshell.file()
        ifcopenshell.api.root.create_entity(ifc_file, ifc_class="IfcProject", name="Test")
        context = ifcopenshell.api.context.add_context(ifc_file, context_type="Model")

        builder = ShapeBuilder(ifc_file)
        curve = builder.rectangle()
        representation = builder.get_representation(context, curve)
        settings = ifcopenshell.geom.settings()
        settings.set("dimensionality", W.CURVES_SURFACES_AND_SOLIDS)
        shape = ifcopenshell.geom.create_shape(settings, representation)

        faces_item_ids = ifcopenshell.util.shape.get_faces_representation_item_ids(shape)
        assert len(faces_item_ids) == 0

        edges_item_ids = ifcopenshell.util.shape.get_edges_representation_item_ids(shape)
        edges = ifcopenshell.util.shape.get_edges(shape)
        assert set(edges_item_ids) == {curve.id()}
        assert len(edges) == 4
        assert len(edges_item_ids) == len(edges)

    def test_mixed_representation_item_ids(self):
        ifc_file = ifcopenshell.file()
        ifcopenshell.api.root.create_entity(ifc_file, ifc_class="IfcProject", name="Test")
        context = ifcopenshell.api.context.add_context(ifc_file, context_type="Model")

        builder = ShapeBuilder(ifc_file)
        curve = builder.rectangle()

        fill = ifc_file.create_entity("IfcAnnotationFillArea", builder.rectangle())
        representation = builder.get_representation(context, (curve, fill))
        settings = ifcopenshell.geom.settings()
        settings.set("dimensionality", W.CURVES_SURFACES_AND_SOLIDS)
        shape = ifcopenshell.geom.create_shape(settings, representation)

        faces_item_ids = ifcopenshell.util.shape.get_faces_representation_item_ids(shape)
        faces = ifcopenshell.util.shape.get_faces(shape)
        assert len(faces) == 2  # Fill area will produce a triangulated face.
        assert set(faces_item_ids) == {fill.id()}
        assert len(faces_item_ids) == len(faces)

        edges_item_ids = ifcopenshell.util.shape.get_edges_representation_item_ids(shape)
        edges = ifcopenshell.util.shape.get_edges(shape)
        assert set(edges_item_ids) == {fill.id(), curve.id()}
        assert len(edges) == 8  # 4 edges rectangle curve + 4 edges fill area
        assert len(edges_item_ids) == len(edges)


class TestAssignObject:
    def test_no_welding_on_distinct_items(self):
        self.file = ifcopenshell.api.project.create_file()
        ifcopenshell.api.owner.settings.get_user = lambda ifc: (ifc.by_type("IfcPersonAndOrganization") or [None])[0]
        ifcopenshell.api.owner.settings.get_application = lambda ifc: (ifc.by_type("IfcApplication") or [None])[0]

        ifcopenshell.api.root.create_entity(self.file, ifc_class="IfcProject", name="Test")
        unit = ifcopenshell.api.unit.add_si_unit(self.file, unit_type="LENGTHUNIT")
        ifcopenshell.api.unit.assign_unit(self.file, units=[unit])
        context = ifcopenshell.api.context.add_context(self.file, context_type="Model")
        element = ifcopenshell.api.root.create_entity(self.file, ifc_class="IfcWall")

        def create_extrusion(x, y):
            points = (
                (x + 0.0, y + 0.0),
                (x + 0.0, y + 1.0),
                (x + 1.0, y + 1.0),
                (x + 1.0, y + 0.0),
                (x + 0.0, y + 0.0),
            )
            curve = self.file.createIfcPolyline([self.file.createIfcCartesianPoint(p) for p in points])
            extrusion_direction = self.file.createIfcDirection((0.0, 0.0, 1.0))
            return self.file.createIfcExtrudedAreaSolid(
                self.file.createIfcArbitraryClosedProfileDef("AREA", None, curve),
                self.file.createIfcAxis2Placement3D(
                    self.file.createIfcCartesianPoint((0.0, 0.0, 0.0)),
                ),
                extrusion_direction,
                1.0,
            )

        extrusions = [create_extrusion(x, 0.0) for x in [0.0, 1.0]]
        element.Representation = self.file.createIfcProductDefinitionShape(
            Representations=[
                self.file.createIfcShapeRepresentation(
                    context,
                    context.ContextIdentifier,
                    "SweptSolid",
                    extrusions,
                )
            ]
        )

        obj = ifcopenshell.geom.create_shape(ifcopenshell.geom.settings(WELD_VERTICES=True), element)

        # item_ids is a per-triangle array, so we have 12 triangles per cube
        # even though not documented, the order in representation items should match
        assert obj.geometry.item_ids == (extrusions[0].id(),) * 12 + (extrusions[1].id(),) * 12

        # group the vertices
        vs = [obj.geometry.verts[i : i + 3] for i in range(0, len(obj.geometry.verts), 3)]

        # welding should not happen between distinct items so the total number of verts should be 2 times 8
        assert len(vs) == 16

        # even though there are only 12 unique vertices as the cubes are touching
        assert len(set(vs)) == 12


def test_iterator():
    # just test some permutations of invocation
    settings = ifcopenshell.geom.settings()
    file_or_filename = [fn, ifcopenshell.open(fn)]
    with_or_without_threads = [[], [multiprocessing.cpu_count()]]
    includes = [
        {},
        {"include": ["IfcColumn"]},
        {"include": [file_or_filename[1].by_type("IfcColumn")[0]]},
    ]
    for args in itertools.product(file_or_filename, with_or_without_threads, includes):
        kwargs = functools.reduce(operator.or_, (a for a in args if isinstance(a, dict)))
        pargs = []
        for a in (_ for _ in args if not isinstance(_, dict)):
            if isinstance(a, list):
                pargs.extend(a)
            else:
                pargs.append(a)
        iterator = ifcopenshell.geom.iterator(settings, *pargs, **kwargs)
        assert iterator.initialize()


@pytest.mark.parametrize("num_threads", [1, 2])
def test_iterator_get_transfers_ownership(num_threads):
    settings = ifcopenshell.geom.settings()
    iterator = ifcopenshell.geom.iterator(settings, fn, num_threads)
    assert iterator.initialize()

    element = iterator.get()
    element_id = element.id
    with pytest.raises(RuntimeError, match="already been retrieved"):
        iterator.get()

    iterator.next()
    assert element.id == element_id


@pytest.mark.parametrize("num_threads", [1, 2])
def test_iterator_get_native_transfers_ownership(num_threads):
    settings = ifcopenshell.geom.settings()
    iterator = ifcopenshell.geom.iterator(settings, fn, num_threads)
    assert iterator.initialize()

    element = iterator.get_native()
    element_id = element.id
    with pytest.raises(RuntimeError, match="already been retrieved"):
        iterator.get_native()

    iterator.next()
    assert element.id == element_id


@pytest.mark.parametrize("num_threads", [1, 2])
def test_iterator_native_output_is_retrieved_with_get(num_threads):
    settings = ifcopenshell.geom.settings()
    settings.set("iterator-output", W.NATIVE)
    iterator = ifcopenshell.geom.iterator(settings, fn, num_threads)
    assert iterator.initialize()

    with pytest.raises(RuntimeError, match=r"use get\(\) instead"):
        iterator.get_native()

    element = iterator.get()
    element_id = element.id
    iterator.next()
    assert element.id == element_id


@pytest.mark.parametrize("num_threads", [1, 2])
@pytest.mark.parametrize("geometry_library", ["opencascade", "hybrid-cgal-simple-opencascade-cgal"])
def test_iterator_skips_duplicate_cartesian_trims(num_threads, geometry_library):
    ifc_file = ifcopenshell.open(os.path.join(os.path.dirname(__file__), "fixtures/geom/duplicate-cartesian-trims.ifc"))
    product = ifc_file.by_type("IfcBuildingElementProxy")[0]
    block = ifc_file.by_type("IfcBlock")[0]
    logger = ifcopenshell.logger()
    logger.output_format(logger.FMT_INMEMORY)
    iterator = ifcopenshell.geom.iterator(
        ifcopenshell.geom.settings(),
        ifc_file,
        num_threads=num_threads,
        include=[product],
        geometry_library=geometry_library,
        logger=logger,
    )

    assert iterator.initialize()
    shape = iterator.get()
    assert shape.guid == product.GlobalId
    # The invalid revolution must not prevent its valid sibling block from being converted.
    assert len(shape.geometry.verts) == 8 * 3
    assert len(shape.geometry.faces) == 12 * 3
    assert set(shape.geometry.item_ids) == {block.id()}
    assert not iterator.next()
    assert [message.code for message in logger].count("GEO295") == 1


def test_logging():
    assert ifcopenshell.logger
    logger = ifcopenshell.logger()
    logger.output_format(logger.FMT_INMEMORY)
    settings = ifcopenshell.geom.settings()
    f = ifcopenshell.open(fn)
    col = f.by_type("IfcColumn")[0]
    _ = ifcopenshell.geom.create_shape(settings, col, logger=logger)

    num_log_items = len(list(logger))
    col.Representation.Representations[0].Items[0].MappingSource.MappedRepresentation.Items[0].Depth *= -1.0

    with pytest.raises(RuntimeError):
        _ = ifcopenshell.geom.create_shape(settings, col, logger=logger)
    new_items = list(logger)[num_log_items:]

    assert ("GEO089", "Non-positive extrusion height encountered for:") in [
        (msg.code, msg.message) for msg in new_items
    ]


class TestSectionedSolidHorizontalRakedEndCut(test.bootstrap.IFC4X3):
    """An IfcSectionedSolidHorizontal whose two IfcAxis2PlacementLinear cross
    section positions use direction vectors inconsistently -- one raked
    RefDirection + width scale, the other plain -- must still loft a uniform
    prism (raked at one end, square at the other), not a wedge, and must not
    log GEO 42."""

    def _build_wingwall(self, theta):
        f = self.file
        ifcopenshell.api.root.create_entity(f, ifc_class="IfcProject", name="Test")
        ctx = ifcopenshell.api.context.add_context(f, context_type="Model")
        body = ifcopenshell.api.context.add_context(
            f, context_type="Model", context_identifier="Body", target_view="MODEL_VIEW", parent=ctx
        )

        self.L, self.w, self.h = 40.0, 20.0 / 12.0, 6.0
        width_scale = 1.0 / math.cos(theta)

        directrix = f.createIfcPolyline(
            Points=[f.createIfcCartesianPoint((0.0, 0.0, 0.0)), f.createIfcCartesianPoint((self.L, 0.0, 0.0))]
        )

        def rect(half_width):
            coords = (
                (-half_width, 0.0),
                (half_width, 0.0),
                (half_width, self.h),
                (-half_width, self.h),
                (-half_width, 0.0),
            )
            return f.createIfcArbitraryClosedProfileDef(
                ProfileType="AREA",
                OuterCurve=f.createIfcIndexedPolyCurve(
                    Points=f.createIfcCartesianPointList2D(coords), Segments=None, SelfIntersect=False
                ),
            )

        axis = f.createIfcDirection((0.0, 0.0, 1.0))
        # Rakes the near end cap about the vertical axis by theta while keeping
        # the section's width axis (Axis x RefDirection) perpendicular extent at w.
        ref_direction = f.createIfcDirection((math.cos(theta), -math.sin(theta), 0.0))

        def location(distance_along):
            return f.createIfcPointByDistanceExpression(
                DistanceAlong=f.createIfcLengthMeasure(distance_along), BasisCurve=directrix
            )

        near = f.createIfcAxis2PlacementLinear(Location=location(0.0), Axis=axis, RefDirection=ref_direction)
        far = f.createIfcAxis2PlacementLinear(Location=location(self.L), Axis=axis)

        solid = f.createIfcSectionedSolidHorizontal(
            Directrix=directrix,
            CrossSections=[rect(0.5 * self.w * width_scale), rect(0.5 * self.w)],
            CrossSectionPositions=[near, far],
        )
        return f.createIfcShapeRepresentation(
            ContextOfItems=body,
            RepresentationIdentifier="Body",
            RepresentationType="AdvancedSweptSolid",
            Items=[solid],
        )

    @pytest.mark.skipif(
        not ifcopenshell.geom.has_geometry_library("opencascade"), reason="requires the OpenCASCADE kernel"
    )
    def test_raked_end_keeps_uniform_perpendicular_thickness(self):
        theta = math.radians(25.0 + 22.0 / 60.0 + 58.0 / 3600.0)
        representation = self._build_wingwall(theta)

        logger = ifcopenshell.logger()
        logger.output_format(logger.FMT_INMEMORY)
        settings = ifcopenshell.geom.settings()
        settings.set("use-world-coords", True)
        geometry = ifcopenshell.geom.create_shape(settings, representation, logger=logger)

        assert "GEO42" not in [msg.code for msg in logger]

        v = ifcopenshell.util.shape.get_vertices(geometry)
        xs = v[:, 0]

        # Uniform thickness perpendicular to the (X aligned) directrix. The whole
        # solid spans exactly w in Y; a wedge (the pre fix behaviour) does not.
        assert np.ptp(v[:, 1]) == pytest.approx(self.w, abs=1e-4)
        for x0 in np.linspace(3.0, self.L - 3.0, 12):
            slab = v[np.abs(xs - x0) < 1.0]
            if len(slab) < 4:
                continue
            assert np.ptp(slab[:, 1]) == pytest.approx(self.w, abs=1e-4), f"thickness at x={x0:.1f}"

        # Far end square to the directrix (all vertices at x == L), near end raked
        # about the vertical axis by exactly theta (its extreme vertex sits at
        # -w/2 * tan(theta) along the directrix).
        assert xs.max() == pytest.approx(self.L, abs=1e-4)
        assert np.ptp(v[xs > xs.max() - 1e-4][:, 0]) < 1e-4
        assert xs.min() == pytest.approx(-0.5 * self.w * math.tan(theta), abs=1e-4)


class TestSectionedSolidHorizontalHonoursAxis(test.bootstrap.IFC4X3):
    """An IfcSectionedSolidHorizontal whose cross section placements carry an
    explicit Axis = (0,0,1) but no RefDirection must sweep the profile with its
    Y axis on that Axis and its normal on the directrix tangent (buildingSMART
    IFC4.x-IF #147) -- i.e. following the curve. Regression test for the case
    where the directrix does not run along the global X axis: the profile used
    to be placed with a fixed [e_y | e_z | e_x] permutation that ignored the
    directrix, collapsing the swept solid (a road pavement running north-south
    would come out a sliver a few centimetres wide)."""

    def _build(self, theta=0.0):
        f = self.file
        ifcopenshell.api.root.create_entity(f, ifc_class="IfcProject", name="Test")
        ctx = ifcopenshell.api.context.add_context(f, context_type="Model")
        body = ifcopenshell.api.context.add_context(
            f, context_type="Model", context_identifier="Body", target_view="MODEL_VIEW", parent=ctx
        )

        self.L, self.width, self.height = 60.0, 12.0, 0.5

        # Directrix runs along +Y, not +X.
        directrix = f.createIfcPolyline(
            Points=[f.createIfcCartesianPoint((0.0, 0.0, 0.0)), f.createIfcCartesianPoint((0.0, self.L, 0.0))]
        )

        def rect(half_width):
            coords = (
                (-half_width, 0.0),
                (half_width, 0.0),
                (half_width, self.height),
                (-half_width, self.height),
                (-half_width, 0.0),
            )
            return f.createIfcArbitraryClosedProfileDef(
                ProfileType="AREA",
                OuterCurve=f.createIfcIndexedPolyCurve(
                    Points=f.createIfcCartesianPointList2D(coords), Segments=None, SelfIntersect=False
                ),
            )

        axis = f.createIfcDirection((0.0, 0.0, 1.0))

        def location(distance_along):
            return f.createIfcPointByDistanceExpression(
                DistanceAlong=f.createIfcLengthMeasure(distance_along), BasisCurve=directrix
            )

        near = f.createIfcAxis2PlacementLinear(Location=location(0.0), Axis=axis)
        if theta:
            # Directrix tangent is +Y; rake the far cap about the vertical Axis.
            ref_direction = f.createIfcDirection((math.sin(theta), math.cos(theta), 0.0))
            far = f.createIfcAxis2PlacementLinear(Location=location(self.L), Axis=axis, RefDirection=ref_direction)
            far_half = 0.5 * self.width / math.cos(theta)
        else:
            far = f.createIfcAxis2PlacementLinear(Location=location(self.L), Axis=axis)
            far_half = 0.5 * self.width

        solid = f.createIfcSectionedSolidHorizontal(
            Directrix=directrix,
            CrossSections=[rect(0.5 * self.width), rect(far_half)],
            CrossSectionPositions=[near, far],
        )
        return f.createIfcShapeRepresentation(
            ContextOfItems=body,
            RepresentationIdentifier="Body",
            RepresentationType="AdvancedSweptSolid",
            Items=[solid],
        )

    def _shape(self, representation):
        logger = ifcopenshell.logger()
        logger.output_format(logger.FMT_INMEMORY)
        settings = ifcopenshell.geom.settings()
        settings.set("use-world-coords", True)
        geometry = ifcopenshell.geom.create_shape(settings, representation, logger=logger)
        assert "GEO42" not in [msg.code for msg in logger]
        return ifcopenshell.util.shape.get_vertices(geometry)

    @pytest.mark.skipif(
        not ifcopenshell.geom.has_geometry_library("opencascade"), reason="requires the OpenCASCADE kernel"
    )
    def test_square_run_follows_the_directrix(self):
        v = self._shape(self._build())
        # width across the road -> world X, crown -> world Z, length -> world Y.
        assert np.ptp(v[:, 0]) == pytest.approx(self.width, abs=1e-4)
        assert np.ptp(v[:, 1]) == pytest.approx(self.L, abs=1e-4)
        assert np.ptp(v[:, 2]) == pytest.approx(self.height, abs=1e-4)

    @pytest.mark.skipif(
        not ifcopenshell.geom.has_geometry_library("opencascade"), reason="requires the OpenCASCADE kernel"
    )
    def test_raked_far_end_on_a_non_axis_aligned_directrix(self):
        theta = math.radians(25.0 + 22.0 / 60.0 + 58.0 / 3600.0)
        v = self._shape(self._build(theta))
        ys = v[:, 1]
        # Uniform width perpendicular to the directrix, square near end, raked far end.
        assert np.ptp(v[:, 0]) == pytest.approx(self.width, abs=1e-4)
        for y0 in np.linspace(5.0, self.L - 5.0, 10):
            slab = v[np.abs(ys - y0) < 1.0]
            if len(slab) >= 4:
                assert np.ptp(slab[:, 0]) == pytest.approx(self.width, abs=1e-4), f"width at y={y0:.1f}"
        assert ys.min() == pytest.approx(0.0, abs=1e-4)
        assert np.ptp(v[ys < ys.min() + 1e-4][:, 1]) < 1e-4
        assert ys.max() == pytest.approx(self.L + 0.5 * self.width * math.tan(theta), abs=1e-4)


class TestSectionedSolidHorizontalOffsetUnits(test.bootstrap.IFC4X3):
    """IfcPointByDistanceExpression.OffsetLateral / OffsetVertical are
    IfcLengthMeasure and must be scaled by the model length unit, exactly like
    DistanceAlong. Regression test: in a millimetre model a 3000 mm lateral
    offset must move the swept solid 3 m, not 3000 m."""

    @pytest.mark.skipif(
        not ifcopenshell.geom.has_geometry_library("opencascade"), reason="requires the OpenCASCADE kernel"
    )
    def test_lateral_offset_is_scaled_by_the_length_unit(self):
        f = self.file
        ifcopenshell.api.root.create_entity(f, ifc_class="IfcProject", name="Test")
        unit = ifcopenshell.api.unit.add_si_unit(f, unit_type="LENGTHUNIT", prefix="MILLI")
        ifcopenshell.api.unit.assign_unit(f, units=[unit])
        ctx = ifcopenshell.api.context.add_context(f, context_type="Model")
        body = ifcopenshell.api.context.add_context(
            f, context_type="Model", context_identifier="Body", target_view="MODEL_VIEW", parent=ctx
        )

        length_mm, width_mm, offset_mm = 20000.0, 4000.0, 3000.0
        directrix = f.createIfcPolyline(
            Points=[f.createIfcCartesianPoint((0.0, 0.0, 0.0)), f.createIfcCartesianPoint((length_mm, 0.0, 0.0))]
        )
        hw = 0.5 * width_mm
        profile = f.createIfcArbitraryClosedProfileDef(
            ProfileType="AREA",
            OuterCurve=f.createIfcIndexedPolyCurve(
                Points=f.createIfcCartesianPointList2D(((-hw, 0.0), (hw, 0.0), (hw, 500.0), (-hw, 500.0), (-hw, 0.0))),
                Segments=None,
                SelfIntersect=False,
            ),
        )
        axis = f.createIfcDirection((0.0, 0.0, 1.0))

        def position(distance_along):
            return f.createIfcAxis2PlacementLinear(
                Location=f.createIfcPointByDistanceExpression(
                    DistanceAlong=f.createIfcLengthMeasure(distance_along),
                    OffsetLateral=offset_mm,
                    BasisCurve=directrix,
                ),
                Axis=axis,
            )

        solid = f.createIfcSectionedSolidHorizontal(
            Directrix=directrix,
            CrossSections=[profile, profile],
            CrossSectionPositions=[position(0.0), position(length_mm)],
        )
        representation = f.createIfcShapeRepresentation(
            ContextOfItems=body,
            RepresentationIdentifier="Body",
            RepresentationType="AdvancedSweptSolid",
            Items=[solid],
        )

        settings = ifcopenshell.geom.settings()
        settings.set("use-world-coords", True)
        geometry = ifcopenshell.geom.create_shape(settings, representation)
        v = ifcopenshell.util.shape.get_vertices(geometry)  # metres

        # Directrix +X, Axis +Z -> profile local x is world +Y; +OffsetLateral shifts there.
        assert np.ptp(v[:, 0]) == pytest.approx(length_mm / 1000.0, abs=1e-4)
        assert np.ptp(v[:, 1]) == pytest.approx(width_mm / 1000.0, abs=1e-4)
        assert v[:, 1].mean() == pytest.approx(offset_mm / 1000.0, abs=1e-3)


@pytest.mark.skipif(not ifcopenshell.geom.has_geometry_library("opencascade"), reason="requires the OpenCASCADE kernel")
@pytest.mark.parametrize("representation", ["IfcFacetedBrep", "IfcPolygonalFaceSet"])
@pytest.mark.parametrize("duplicate", ["none", "same_face", "distinct_face", "reversed_face"])
def test_coincident_face_loops_retain_one_surface(representation, duplicate):
    f = ifcopenshell.file(schema="IFC4")
    coordinates = [
        (0.0, 0.0, 0.0),
        (1.0, 0.0, 0.0),
        (1.0, 1.0, 0.0),
        (0.0, 1.0, 0.0),
        (0.0, 0.0, 1.0),
        (1.0, 0.0, 1.0),
        (1.0, 1.0, 1.0),
        (0.0, 1.0, 1.0),
    ]
    indices = [(1, 4, 3, 2), (1, 2, 6, 5), (2, 3, 7, 6), (3, 4, 8, 7), (4, 1, 5, 8), (5, 6, 7, 8)]
    if representation == "IfcFacetedBrep":
        points = [f.createIfcCartesianPoint(p) for p in coordinates]

        def make_face(loop):
            return f.createIfcFace(
                [f.createIfcFaceOuterBound(f.createIfcPolyLoop([points[i - 1] for i in loop]), True)]
            )
    else:
        points = f.createIfcCartesianPointList3D(coordinates)

        def make_face(loop):
            return f.createIfcIndexedPolygonalFace(loop)

    faces = [make_face(loop) for loop in indices]
    if duplicate == "same_face":
        faces.append(faces[-1])
    elif duplicate == "distinct_face":
        faces.append(make_face(indices[-1]))
    elif duplicate == "reversed_face":
        faces.append(make_face(tuple(reversed(indices[-1]))))

    if representation == "IfcFacetedBrep":
        item = f.createIfcFacetedBrep(f.createIfcClosedShell(faces))
    else:
        item = f.createIfcPolygonalFaceSet(Coordinates=points, Closed=True, Faces=faces)

    geometry = ifcopenshell.geom.create_shape(ifcopenshell.geom.settings(), item, geometry_library="opencascade")
    vertices = ifcopenshell.util.shape.get_vertices(geometry)
    triangles = ifcopenshell.util.shape.get_faces(geometry)
    assert len(vertices) == 8
    assert len(triangles) == 12
    assert vertices.min(axis=0) == pytest.approx((0.0, 0.0, 0.0))
    assert vertices.max(axis=0) == pytest.approx((1.0, 1.0, 1.0))
    assert ifcopenshell.util.shape.get_volume(geometry) == pytest.approx(1.0)


if __name__ == "__main__":
    import pytest

    pytest.main(["-vvsx", __file__])
