# This file was generated with the assistance of an AI coding tool.
"""Hybrid children must use the same resolved model precision as mapping."""

import argparse
import json
import math
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
    units = model.createIfcUnitAssignment([model.createIfcSIUnit(None, "LENGTHUNIT", "MILLI", "METRE")])
    model.createIfcProject(
        "2wKlmo9o1DdgfHw18cs4H0", None, "Model precision regression", None, None, None, None, [context], units
    )
    profile = model.createIfcCircleProfileDef("AREA", None, None, 50.0)

    def product(guid, name, depth):
        extrusion = model.createIfcExtrudedAreaSolid(profile, placement, axis, depth)
        representation = model.createIfcShapeRepresentation(context, "Body", "SweptSolid", [extrusion])
        return model.createIfcBuildingElementProxy(
            guid,
            None,
            name,
            None,
            None,
            model.createIfcLocalPlacement(None, placement),
            model.createIfcProductDefinitionShape(None, None, [representation]),
        )

    # The source context requests 1e-8 m. Native mapping applies its existing
    # 1e-7 m minimum; the valid 5e-6 m extrusion must survive unchanged.
    depth = 1e-6 if case == "below-floor" else 0.005
    thin = product("0voq45cED6Q8VtZJRmocPp", "Thin cylinder", depth)
    healthy = product("1w2o0Six6Kf4C9rBQfAg7l", "Healthy cylinder", 100.0)
    settings = ifcopenshell.geom.settings()
    settings.set("circle-segments", 32)
    if case == "coarser-factor":
        settings.set("precision-factor", 1000.0)
    iterator = ifcopenshell.geom.iterator(
        settings, model, num_threads=threads, include=[thin, healthy], geometry_library=kernel
    )
    assert iterator.initialize()
    observed = {}
    while True:
        shape = iterator.get()
        vertices = list(zip(*[iter(shape.geometry.verts)] * 3))
        faces = list(zip(*[iter(shape.geometry.faces)] * 3))
        assert vertices and faces
        assert all(math.isfinite(component) for vertex in vertices for component in vertex)
        assert all(0 <= index < len(vertices) for face in faces for index in face)
        dimensions = [max(v[i] for v in vertices) - min(v[i] for v in vertices) for i in range(3)]
        assert 0.09 <= dimensions[0] <= 0.100000001, dimensions
        assert 0.09 <= dimensions[1] <= 0.100000001, dimensions
        expected_height = 0.1 if shape.guid == healthy.GlobalId else depth * 0.001
        assert math.isclose(dimensions[2], expected_height, rel_tol=1e-9, abs_tol=1e-12), dimensions
        for face in faces:
            a, b, c = [vertices[i] for i in face]
            ab = [b[i] - a[i] for i in range(3)]
            ac = [c[i] - a[i] for i in range(3)]
            cross = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]]
            assert sum(component * component for component in cross) > 0.0
        observed[shape.guid] = {"dimensions": dimensions, "triangles": len(faces)}
        if not iterator.next():
            break
    expected = {healthy.GlobalId, thin.GlobalId} if case == "model-default" else {healthy.GlobalId}
    assert set(observed) == expected, observed
    print(json.dumps({"case": case, "kernel": kernel, "threads": threads, "observed": observed}), flush=True)


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
    parser.add_argument("case", choices=("model-default", "below-floor", "coarser-factor"))
    parser.add_argument("--kernel", default="opencascade")
    parser.add_argument("--threads", type=int, default=1)
    arguments = parser.parse_args()
    run_native_case(arguments.case, arguments.kernel, arguments.threads)
else:
    import pytest

    @pytest.mark.parametrize("threads", [1, 4])
    @pytest.mark.parametrize("kernel", ["opencascade", "hybrid-cgal-simple-opencascade-cgal"])
    @pytest.mark.parametrize("case", ["model-default", "below-floor", "coarser-factor"])
    def test_hybrid_respects_model_precision_and_coarser_factor(case, kernel, threads):
        run_isolated(case, kernel, threads)
