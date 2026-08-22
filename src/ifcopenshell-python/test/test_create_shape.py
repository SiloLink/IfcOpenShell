import functools
import itertools
import multiprocessing
import operator
import os
from typing import get_args

import pytest

import ifcopenshell
import ifcopenshell.api.context
import ifcopenshell.api.geometry
import ifcopenshell.api.material
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
        assert set(get_args(ifcopenshell.geom.SETTING)) == set(
            settings.setting_names()
        ), "Also need to update IfcPython.i, if new settings were added/removed."

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

    def test_serializer_settings(self):
        settings = ifcopenshell.geom.serializer_settings()
        assert set(get_args(ifcopenshell.geom.SERIALIZER_SETTING)) == set(
            settings.setting_names()
        ), "Also need to update IfcPython.i, if new settings were added/removed."

        # Only for settings.
        assert "use-python-opencascade" not in settings.setting_names()
        with pytest.raises(AttributeError):
            settings.get(settings.USE_PYTHON_OPENCASCADE)
        with pytest.raises(RuntimeError):
            settings.get("use-python-opencascade")
        with pytest.raises(RuntimeError):
            settings.set("use-python-opencascade", True)
        assert "USE_PYTHON_OPENCASCADE" not in repr(settings)


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
        shape = ifcopenshell.geom.create_shape(
            settings, representation, geometry_library=geometry_library
        )

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
        context = ifc_file.createIfcGeometricRepresentationContext(
            None, "Model", 3, 1e-5, axis, None
        )
        ifc_file.createIfcProject(
            ifcopenshell.guid.new(), None, "Test", None, None, None, None, (context,), None
        )
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
        host_representation = ifc_file.createIfcShapeRepresentation(
            context, "Body", "Tessellation", (host,)
        )
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
        void_representation = ifc_file.createIfcShapeRepresentation(
            context, "Body", "Tessellation", (void,)
        )
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
        ifc_file.createIfcRelVoidsElement(
            ifcopenshell.guid.new(), None, None, None, wall, opening
        )

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
            tuple(round(float(channel), 6) for channel in materials[index])
            for index in material_ids
            if index >= 0
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

    def test_indexed_colours_follow_triangulated_faces(self):
        ifc_file = ifcopenshell.file(schema="IFC4")
        project = ifc_file.createIfcProject(
            ifcopenshell.guid.new(), None, "Test", None, None, None, None, None, None
        )
        origin = ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0))
        axis = ifc_file.createIfcAxis2Placement3D(origin, None, None)
        context = ifc_file.createIfcGeometricRepresentationContext(
            None, "Model", 3, 1e-5, axis, None
        )
        project.RepresentationContexts = [context]

        points = ifc_file.createIfcCartesianPointList3D(
            (
                (0.0, 0.0, 0.0),
                (1.0, 0.0, 0.0),
                (0.0, 1.0, 0.0),
                (1.0, 1.0, 0.0),
            )
        )
        face_set = ifc_file.createIfcTriangulatedFaceSet(
            points, None, False, ((1, 2, 3), (2, 4, 3)), None
        )
        colours = ifc_file.createIfcColourRgbList(
            ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0))
        )
        ifc_file.createIfcIndexedColourMap(face_set, 0.6, colours, (1, 2))
        representation = ifc_file.createIfcShapeRepresentation(
            context, "Body", "Tessellation", (face_set,)
        )

        settings = ifcopenshell.geom.settings()
        settings.set("apply-default-materials", False)
        shape = ifcopenshell.geom.create_shape(settings, representation)

        material_ids = ifcopenshell.util.shape.get_faces_material_style_ids(shape)
        materials = ifcopenshell.util.shape.get_material_colors(shape)
        face_colours = [
            tuple(round(float(channel), 6) for channel in materials[index])
            for index in material_ids
        ]

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
            points = ifc_file.createIfcCartesianPointList3D(
                tuple((x, y, z) for y in (0.2, 0.0) for x, z in profile)
            )
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
            representation = ifc_file.createIfcShapeRepresentation(
                context, "Body", "Tessellation", (face_set,)
            )
            settings = ifcopenshell.geom.settings()
            settings.set("apply-default-materials", False)
            return ifcopenshell.geom.create_shape(settings, representation)

        def triangle_coordinates(shape):
            vertices = ifcopenshell.util.shape.get_vertices(shape)
            return sorted(
                tuple(
                    sorted(
                        tuple(round(float(channel), 9) for channel in vertices[vertex_index])
                        for vertex_index in face
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


def test_iterator_include_filter_does_not_duplicate_identity_mapped_representation():
    ifc_file = ifcopenshell.api.project.create_file()
    ifcopenshell.api.root.create_entity(ifc_file, ifc_class="IfcProject", name="Test")
    unit = ifcopenshell.api.unit.add_si_unit(ifc_file, unit_type="LENGTHUNIT")
    ifcopenshell.api.unit.assign_unit(ifc_file, units=[unit])
    context = ifcopenshell.api.context.add_context(ifc_file, context_type="Model")
    body = ifcopenshell.api.context.add_context(
        ifc_file,
        context_type="Model",
        context_identifier="Body",
        target_view="MODEL_VIEW",
        parent=context,
    )

    builder = ShapeBuilder(ifc_file)
    source_representation = builder.get_representation(body, builder.extrude(builder.rectangle(), magnitude=1.0))
    representation_map = ifc_file.createIfcRepresentationMap(
        MappingOrigin=ifc_file.createIfcAxis2Placement3D(ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0))),
        MappedRepresentation=source_representation,
    )

    def create_identity_mapped_representation():
        target = ifc_file.createIfcCartesianTransformationOperator3D(
            ifc_file.createIfcDirection((1.0, 0.0, 0.0)),
            ifc_file.createIfcDirection((0.0, 1.0, 0.0)),
            ifc_file.createIfcCartesianPoint((0.0, 0.0, 0.0)),
            1.0,
            ifc_file.createIfcDirection((0.0, 0.0, 1.0)),
        )
        return ifc_file.createIfcShapeRepresentation(
            body,
            "Body",
            "MappedRepresentation",
            [ifc_file.createIfcMappedItem(MappingSource=representation_map, MappingTarget=target)],
        )

    products = []
    for material_name in ("A", "B"):
        product = ifcopenshell.api.root.create_entity(ifc_file, ifc_class="IfcWall", name=f"Wall {material_name}")
        ifcopenshell.api.geometry.edit_object_placement(ifc_file, product=product)
        product.Representation = ifc_file.createIfcProductDefinitionShape(
            Representations=[create_identity_mapped_representation()]
        )
        material = ifcopenshell.api.material.add_material(ifc_file, name=material_name)
        ifcopenshell.api.material.assign_material(ifc_file, products=[product], material=material)
        products.append(product)

    settings = ifcopenshell.geom.settings()
    settings.set(settings.USE_WORLD_COORDS, False)
    iterator = ifcopenshell.geom.iterator(settings, ifc_file, include=[products[0]], num_threads=1)
    assert iterator.initialize()

    ids = []
    while True:
        ids.append(iterator.get().id)
        if not iterator.next():
            break

    assert ids == [products[0].id()]
    assert [[product.id() for product in group] for group in iterator.get_task_products()] == [[products[0].id()]]


if __name__ == "__main__":
    import pytest

    pytest.main(["-vvsx", __file__])
