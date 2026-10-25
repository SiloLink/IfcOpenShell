# This file was generated with the assistance of an AI coding tool.
"""The capability list comes from the loaded binary, before any file is opened."""

import json

import ifcopenshell


def test_capabilities_are_exported_by_the_loaded_binary():
    exported = json.loads(ifcopenshell.ifcopenshell_wrapper.silolink_capabilities())
    assert isinstance(exported, list) and all(isinstance(name, str) for name in exported)
    assert set(exported) == ifcopenshell.capabilities
    assert {
        "step_definition_coalescing",
        "open_error_gil_restored",
        "structured_diagnostics",
        "complete_opening_cutters",
        "partial_geometry_recovery",
        "mapped_occurrence_openings",
        "iterator_logger_retained",
    } <= ifcopenshell.capabilities
