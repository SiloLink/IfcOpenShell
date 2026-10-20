# This file was generated with the assistance of an AI coding tool.
"""Face-surface mapping regressions, isolated from pytest on native failure."""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run_native_case(case, kernel, threads):
    import ifcopenshell
    import ifcopenshell.geom

    model = ifcopenshell.file(schema="IFC4")
    origin = model.createIfcCartesianPoint((0.0, 0.0, 0.0))
    axis = model.createIfcDirection((0.0, 0.0, 1.0))
    placement = model.createIfcAxis2Placement3D(origin, axis)
    context = model.createIfcGeometricRepresentationContext(None, "Model", 3, 1e-5, placement, None)
    units = model.createIfcUnitAssignment([model.createIfcSIUnit(None, "LENGTHUNIT", None, "METRE")])
    model.createIfcProject(
        "2wKlmo9o1DdgfHw18cs4H0", None, "Face mapping regression", None, None, None, None, [context], units
    )
    plane = model.createIfcPlane(placement)

    def face(x, surface, same_sense):
        points = [
            model.createIfcCartesianPoint((x, 0.0, 0.0)),
            model.createIfcCartesianPoint((x + 1.0, 0.0, 0.0)),
            model.createIfcCartesianPoint((x, 1.0, 0.0)),
        ]
        # SameSense relates the surface normal to the topological face normal;
        # the boundary must carry the corresponding orientation as well.
        bound = model.createIfcFaceOuterBound(model.createIfcPolyLoop(points), same_sense)
        return model.createIfcFaceSurface([bound], surface, same_sense)

    same_sense = case.endswith("forward")
    if case.startswith("failed"):
        centre = model.createIfcCartesianPoint((0.5, 0.0, 0.0))
        circle = model.createIfcCircle(model.createIfcAxis2Placement3D(centre, axis), 0.0005)
        curve = model.createIfcTrimmedCurve(circle, [centre], [centre], True, "CARTESIAN")
        profile = model.createIfcArbitraryOpenProfileDef("CURVE", None, curve)
        surface = model.createIfcSurfaceOfRevolution(profile, None, model.createIfcAxis1Placement(origin, axis))
    else:
        surface = plane

    # The valid sibling shares the plane in the orientation control, exercising
    # the mapping cache without allowing one reversed face to mutate the other.
    faces = [face(2.0, surface, same_sense), face(0.0, plane, True)]
    shell = model.createIfcConnectedFaceSet(faces)
    item = model.createIfcFaceBasedSurfaceModel([shell])
    representation = model.createIfcShapeRepresentation(context, "Body", "SurfaceModel", [item])
    product = model.createIfcBuildingElementProxy(
        "0voq45cED6Q8VtZJRmocPp",
        None,
        "Healthy sibling survives",
        None,
        None,
        model.createIfcLocalPlacement(None, placement),
        model.createIfcProductDefinitionShape(None, None, [representation]),
    )
    logger = ifcopenshell.logger()
    logger.output_format(logger.FMT_INMEMORY)
    iterator = ifcopenshell.geom.iterator(
        ifcopenshell.geom.settings(),
        model,
        num_threads=threads,
        include=[product],
        geometry_library=kernel,
        logger=logger,
    )
    assert iterator.initialize()
    shape = iterator.get()
    assert shape.guid == product.GlobalId
    vertices = list(zip(*[iter(shape.geometry.verts)] * 3))
    triangles = list(zip(*[iter(shape.geometry.faces)] * 3))
    if case.startswith("failed"):
        assert len(vertices) == 3, (vertices, triangles)
        assert len(triangles) == 1, triangles
        assert all(0.0 <= point[0] <= 1.0 for point in vertices)
        assert "GEO295" in [message.code for message in logger]
    else:
        assert len(triangles) == 2
        for triangle in triangles:
            a, b, c = [vertices[index] for index in triangle]
            normal_z = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            expected = 1.0 if max(a[0], b[0], c[0]) <= 1.0 or same_sense else -1.0
            assert abs(normal_z - expected) < 1e-8, (case, triangle, normal_z, expected)
    assert not iterator.next()
    print(json.dumps({"case": case, "kernel": kernel, "threads": threads, "triangles": len(triangles)}), flush=True)


def run_isolated(case, kernel, threads):
    command = [sys.executable, str(Path(__file__).resolve()), case, "--kernel", kernel, "--threads", str(threads)]
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 30.0
        failure = None
        try:
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    failure = "native subprocess exceeded 30 seconds"
                    break
                if os.fstat(output.fileno()).st_size > 256 * 1024:
                    failure = "native subprocess exceeded 256 KiB output cap"
                    break
                time.sleep(0.02)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
        if os.fstat(output.fileno()).st_size > 256 * 1024:
            failure = failure or "native subprocess exceeded 256 KiB output cap"
        output.seek(0)
        diagnostic = output.read(256 * 1024).decode(errors="replace")
        assert not failure, f"{failure}\n{diagnostic}"
        assert process.returncode == 0, f"native subprocess exited {process.returncode}\n{diagnostic}"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=("failed-forward", "failed-reversed", "valid-forward", "valid-reversed"))
    parser.add_argument("--kernel", default="opencascade")
    parser.add_argument("--threads", type=int, default=1)
    arguments = parser.parse_args()
    run_native_case(arguments.case, arguments.kernel, arguments.threads)
else:
    import pytest

    @pytest.mark.parametrize("threads", [1, 4])
    @pytest.mark.parametrize("kernel", ["opencascade", "hybrid-cgal-simple-opencascade-cgal"])
    @pytest.mark.parametrize("same_sense", ["forward", "reversed"])
    def test_failed_face_surface_preserves_healthy_sibling(same_sense, kernel, threads):
        run_isolated(f"failed-{same_sense}", kernel, threads)

    @pytest.mark.parametrize("threads", [1, 4])
    @pytest.mark.parametrize("same_sense", ["forward", "reversed"])
    def test_valid_face_surface_preserves_orientation(same_sense, threads):
        run_isolated(f"valid-{same_sense}", "opencascade", threads)
