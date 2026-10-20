# This file was generated with the assistance of an AI coding tool.
"""OpenCASCADE must cut surface openings without inventing an enclosed volume."""

import argparse
import json
import subprocess
import sys
from pathlib import Path


def run_case(case):
    import numpy as np

    import ifcopenshell
    import ifcopenshell.geom
    import ifcopenshell.util.shape

    model = ifcopenshell.file(schema="IFC4")

    def axis(location=(0.0, 0.0, 0.0), direction=(1.0, 0.0, 0.0)):
        return model.createIfcAxis2Placement3D(
            model.createIfcCartesianPoint(location),
            model.createIfcDirection((0.0, 0.0, 1.0)),
            model.createIfcDirection(direction),
        )

    def placement(location=(0.0, 0.0, 0.0), direction=(1.0, 0.0, 0.0), parent=None):
        return model.createIfcLocalPlacement(parent, axis(location, direction))

    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, axis(), None)
    units = model.createIfcUnitAssignment([model.createIfcSIUnit(None, "LENGTHUNIT", "MILLI", "METRE")])
    model.createIfcProject(ifcopenshell.guid.new(), None, None, None, None, None, None, [context], units)

    def face(points):
        loop = model.createIfcPolyLoop([model.createIfcCartesianPoint(tuple(float(v) for v in p)) for p in points])
        return model.createIfcFace([model.createIfcFaceOuterBound(loop, True)])

    def box_faces(lower, upper):
        x0, y0, z0 = lower
        x1, y1, z1 = upper
        points = (
            (x0, y0, z0),
            (x1, y0, z0),
            (x1, y1, z0),
            (x0, y1, z0),
            (x0, y0, z1),
            (x1, y0, z1),
            (x1, y1, z1),
            (x0, y1, z1),
        )
        return [
            face([points[i] for i in indices])
            for indices in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (3, 7, 6, 2), (0, 4, 7, 3), (1, 2, 6, 5))
        ]

    def product(ifc_class, item, rep_type, local_placement):
        representation = model.createIfcShapeRepresentation(context, "Body", rep_type, [item])
        return model.create_entity(
            ifc_class,
            GlobalId=ifcopenshell.guid.new(),
            ObjectPlacement=local_placement,
            Representation=model.createIfcProductDefinitionShape(None, None, [representation]),
        )

    grid = case == "grid"
    closed = case == "closed"
    styled = case == "styled"
    mapped = case == "mapped"
    if grid:
        n = 100
        faces = []
        for y in range(n):
            for x in range(n):
                a, b, c, d = (
                    (x * 100.0, y * 100.0, 0.0),
                    ((x + 1) * 100.0, y * 100.0, 0.0),
                    ((x + 1) * 100.0, (y + 1) * 100.0, 0.0),
                    (x * 100.0, (y + 1) * 100.0, 0.0),
                )
                faces.extend((face((a, b, c)), face((a, c, d))))
        expected_area = 100.0 - 4.0
    else:
        faces = box_faces((0.0, 0.0, 0.0), (10000.0, 6000.0, 2000.0))
        if not closed:
            del faces[1]  # The authored surface has no top face.
        expected_area = 192.0 if closed else 120.0

    if styled:
        colours = ((0.8, 0.2, 0.1), (0.1, 0.8, 0.2), (0.2, 0.1, 0.8), (0.8, 0.8, 0.1), (0.8, 0.1, 0.8))
        for source_face, rgb in zip(faces, colours):
            colour = model.createIfcColourRgb(None, *rgb)
            style = model.createIfcSurfaceStyle("Surface", "BOTH", [model.createIfcSurfaceStyleShading(colour)])
            model.createIfcStyledItem(source_face, [style], None)

    shell = model.createIfcClosedShell(faces) if closed else model.createIfcConnectedFaceSet(faces)
    item = model.createIfcFacetedBrep(shell) if closed else model.createIfcFaceBasedSurfaceModel([shell])
    rep_type = "Brep" if closed else "SurfaceModel"
    if mapped:
        source = model.createIfcShapeRepresentation(context, "Body", rep_type, [item])
        mapping = model.createIfcRepresentationMap(axis(), source)
        target = model.createIfcCartesianTransformationOperator3D(
            model.createIfcDirection((0.0, 1.0, 0.0)),
            model.createIfcDirection((-1.0, 0.0, 0.0)),
            model.createIfcCartesianPoint((1000.0, 2000.0, 0.0)),
            1.0,
            model.createIfcDirection((0.0, 0.0, 1.0)),
        )
        item = model.createIfcMappedItem(mapping, target)
        rep_type = "MappedRepresentation"
    host_placement = placement((20000.0, 30000.0, 4000.0), (0.0, 1.0, 0.0))
    host = product("IfcBuildingElementProxy", item, rep_type, host_placement)
    opening_placement = placement(parent=host_placement)
    if mapped:
        opening_placement = placement((1000.0, 2000.0, 0.0), (0.0, 1.0, 0.0), host_placement)
    opening = product(
        "IfcOpeningElement",
        model.createIfcFacetedBrep(
            model.createIfcClosedShell(box_faces((2000.0, 2000.0, -1000.0), (4000.0, 4000.0, 3000.0)))
        ),
        "Brep",
        opening_placement,
    )
    model.createIfcRelVoidsElement(ifcopenshell.guid.new(), None, None, None, host, opening)
    settings = ifcopenshell.geom.settings()
    settings.set("precision", 1e-5)
    settings.set("weld-vertices", True)
    logger = ifcopenshell.logger()
    logger.output_format(logger.FMT_INMEMORY)
    iterator = ifcopenshell.geom.iterator(
        settings, model, num_threads=1, include=[host], geometry_library="opencascade", logger=logger
    )
    assert iterator.initialize()
    shape = iterator.get()
    geometry = shape.geometry
    area = ifcopenshell.util.shape.get_area(geometry)
    assert abs(area - expected_area) < 1e-6, (case, area, expected_area)
    np.testing.assert_allclose(
        ifcopenshell.util.shape.get_shape_matrix(shape),
        ((0.0, -1.0, 0.0, 20.0), (1.0, 0.0, 0.0, 30.0), (0.0, 0.0, 1.0, 4.0), (0.0, 0.0, 0.0, 1.0)),
        atol=1e-9,
    )
    vertices = ifcopenshell.util.shape.get_vertices(geometry)
    if mapped:
        # Undo the mapped representation placement, keeping product-local coordinates.
        vertices = np.column_stack((vertices[:, 1] - 2.0, 1.0 - vertices[:, 0], vertices[:, 2]))
    triangles = vertices[ifcopenshell.util.shape.get_faces(geometry)]
    centroids = triangles.mean(axis=1)
    inside_hole = (centroids[:, 0] > 2.0) & (centroids[:, 0] < 4.0)
    inside_hole &= (centroids[:, 1] > 2.0) & (centroids[:, 1] < 4.0)
    assert not inside_hole.any(), centroids[inside_hole]
    if not closed:
        # No faces may be created on the inside walls of the cutting solid.
        assert np.all(
            np.all(np.isclose(triangles[:, :, 2], 0.0), axis=1)
            | np.all(np.isclose(triangles[:, :, 0], 0.0), axis=1)
            | np.all(np.isclose(triangles[:, :, 0], 10.0), axis=1)
            | np.all(np.isclose(triangles[:, :, 1], 0.0), axis=1)
            | np.all(np.isclose(triangles[:, :, 1], 6.0), axis=1)
        )
        assert "GEO191" not in [message.code for message in logger]
    else:
        assert abs(ifcopenshell.util.shape.get_volume(geometry) - 112.0) < 1e-6
    if styled:
        assert len(geometry.material_ids) == len(triangles)
        assert all(index >= 0 for index in geometry.material_ids)
        for triangle, material_id in zip(triangles, geometry.material_ids):
            source_faces = (
                np.all(np.isclose(triangle[:, 2], 0.0)),
                np.all(np.isclose(triangle[:, 1], 0.0)),
                np.all(np.isclose(triangle[:, 1], 6.0)),
                np.all(np.isclose(triangle[:, 0], 0.0)),
                np.all(np.isclose(triangle[:, 0], 10.0)),
            )
            assert sum(source_faces) == 1
            material = geometry.materials[material_id]
            np.testing.assert_allclose(material.diffuse.components, colours[source_faces.index(True)], atol=1e-9)
    assert not iterator.next()
    print(json.dumps({"case": case, "area": area, "triangles": len(triangles)}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=("surface", "mapped", "styled", "closed", "grid"))
    run_case(parser.parse_args().case)
else:
    import pytest

    @pytest.mark.parametrize("case", ("surface", "mapped", "styled", "closed", "grid"))
    def test_surface_openings(case):
        # Keep native sewing regressions bounded without changing geometry tolerances.
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), case],
            capture_output=True,
            text=True,
            timeout=45,
        )
        assert result.returncode == 0, result.stdout + result.stderr
