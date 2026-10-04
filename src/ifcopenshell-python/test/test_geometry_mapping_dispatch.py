import numpy as np
import pytest

import ifcopenshell
import ifcopenshell.geom

SCHEMAS = ("IFC2X3", "IFC4", "IFC4X1", "IFC4X2", "IFC4X3", "IFC4X3_TC1", "IFC4X3_ADD1", "IFC4X3_ADD2")


@pytest.fixture(params=SCHEMAS)
def model(request):
    wrapper = ifcopenshell.ifcopenshell_wrapper
    if request.param not in wrapper.schema_names():
        pytest.skip(f"Schema {request.param} is not compiled in this build")
    # The public schema="IFC4X3" constructor aliases ADD2; exercise the original schema as well.
    return ifcopenshell.file(schema_identifier=request.param)


def _map(instance):
    return ifcopenshell.geom.map_shape(ifcopenshell.geom.settings(), instance)


def _position_2d(model):
    return model.createIfcAxis2Placement2D(model.createIfcCartesianPoint((0.0, 0.0)), None)


def _position_3d(model):
    return model.createIfcAxis2Placement3D(model.createIfcCartesianPoint((0.0, 0.0, 0.0)), None, None)


def test_exact_point_handler_preserves_coordinates(model):
    point = model.createIfcCartesianPoint((1.25, -2.5, 3.75))
    assert _map(point).components == (1.25, -2.5, 3.75)


def test_unregistered_subtype_uses_compatible_transformation_handler(model):
    transform = model.createIfcCartesianTransformationOperator3DnonUniform(
        None, None, model.createIfcCartesianPoint((1.0, 2.0, 3.0)), 2.0, None, 3.0, 4.0
    )
    np.testing.assert_array_equal(
        _map(transform).components,
        ((2.0, 0.0, 0.0, 1.0), (0.0, 3.0, 0.0, 2.0), (0.0, 0.0, 4.0, 3.0), (0.0, 0.0, 0.0, 1.0)),
    )


def test_rounded_profile_handler_precedes_rectangle_handler(model):
    profile = model.createIfcRoundedRectangleProfileDef("AREA", None, _position_2d(model), 4.0, 6.0, 0.25)
    loop = _map(profile)
    assert len(loop.children) == 8
    assert sum(isinstance(edge.basis, ifcopenshell.ifcopenshell_wrapper.circle) for edge in loop.children) == 4


def test_hollow_profile_handler_preserves_inner_boundary(model):
    profile = model.createIfcRectangleHollowProfileDef("AREA", None, _position_2d(model), 4.0, 6.0, 0.25, None, None)
    face = _map(profile)
    assert len(face.children) == 2
    assert [len(loop.children) for loop in face.children] == [4, 4]
    np.testing.assert_array_equal(face.children[1].children[0].start.components, (-1.75, -2.75, 0.0))


def test_null_mapping_still_attempts_base_handler_and_preserves_failure_logs(model):
    profile = model.createIfcRoundedRectangleProfileDef("AREA", None, _position_2d(model), 0.0, 6.0, 0.25)
    ifcopenshell.get_log()
    assert _map(profile) is None
    log = ifcopenshell.get_log()
    assert log.count("Skipping zero sized profile:") == 2
    assert log.count("Failed to convert:") == 2
    assert "No operation defined for:" not in log


def test_unsupported_entity_keeps_no_operation_diagnostic(model):
    colour = model.createIfcColourRgb(None, 0.1, 0.2, 0.3)
    ifcopenshell.get_log()
    assert _map(colour) is None
    log = ifcopenshell.get_log()
    assert log.count("No operation defined for:") == 1
    assert "Failed to convert:" not in log


def test_tapered_extrusion_uses_loft_before_base_extrusion(model):
    if model.schema_identifier == "IFC2X3":
        pytest.skip("IFC2X3 has no tapered extrusion entity")
    start = model.createIfcRectangleProfileDef("AREA", None, _position_2d(model), 4.0, 6.0)
    end = model.createIfcRectangleProfileDef("AREA", None, _position_2d(model), 2.0, 3.0)
    solid = model.createIfcExtrudedAreaSolidTapered(
        start, _position_3d(model), model.createIfcDirection((0.0, 0.0, 1.0)), 5.0, end
    )
    loft = _map(solid)
    assert isinstance(loft, ifcopenshell.ifcopenshell_wrapper.loft)
    assert len(loft.children) == 2
    np.testing.assert_array_equal(loft.children[1].matrix.components[2], (0.0, 0.0, 1.0, 5.0))
