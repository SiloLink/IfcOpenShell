# This file was generated with the assistance of an AI coding tool.
"""File construction failures must restore Python's thread state before raising."""

import subprocess
import sys
from pathlib import Path


def run_case(method, case, path):
    import ifcopenshell

    header = "ISO-10303-21;HEADER;FILE_DESCRIPTION((''),'2;1');FILE_NAME('','',(),(),'','','');FILE_SCHEMA(('IFC4'));ENDSEC;DATA;"
    first = "#1=IFCQUANTITYLENGTH('a',$,$,1.,$);"
    normal = header + first + "#2=IFCQUANTITYLENGTH('b',$,$,2.,$);ENDSEC;END-ISO-10303-21;"
    repeated = {
        "identical": first,
        "layout": "#1 = IFCQUANTITYLENGTH ( 'a', /* comment */ $, $, 1. , $ ) ;",
        "value_conflict": first.replace("1.,", "2.,"),
        "string_conflict": first.replace("'a'", "'a '"),
        "precision_conflict": first.replace("1.,", "1.00000000000000001,"),
        "suffix_conflict": first.replace(");", ")EXTRA;"),
    }.get(case, "")
    data = normal.replace("ENDSEC;END-ISO", repeated + "ENDSEC;END-ISO")
    duplicate = case.endswith("conflict")
    path.write_text(data)
    for _ in range(3):
        try:
            if method in ("lazy", "paged"):
                native = ifcopenshell.ifcopenshell_wrapper.file.create_uninitialized()
                if method == "lazy":
                    native.lazy_loading(True)
                else:
                    native.paged_reading(True)
                assert native.initialize(str(path))
                model = native
            else:
                model = ifcopenshell.open(path) if method == "open" else ifcopenshell.file.from_string(data)
        except RuntimeError as exc:
            assert duplicate
            assert "Duplicate STEP instance id #1" in str(exc), str(exc)
        else:
            assert not duplicate, "Ambiguous STEP ids must not silently select an instance"
            assert [(p.id(), p.Name, p.LengthValue) for p in model.by_type("IfcQuantityLength")] == [
                (1, "a", 1.0),
                (2, "b", 2.0),
            ]
        # An exception must not corrupt subsequent Python or native calls.
        healthy = ifcopenshell.file.from_string(normal)
        assert len(healthy.by_type("IfcQuantityLength")) == 2


if __name__ == "__main__":
    run_case(sys.argv[1], sys.argv[2], Path(sys.argv[3]))
else:
    import pytest

    @pytest.mark.parametrize("method", ("open", "read", "lazy", "paged"))
    @pytest.mark.parametrize(
        "case",
        ("unique", "identical", "layout", "value_conflict", "string_conflict", "precision_conflict", "suffix_conflict"),
    )
    def test_open_errors(method, case, tmp_path):
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), method, case, str(tmp_path / "input.ifc")],
            capture_output=True,
            text=True,
            timeout=20,
        )
        assert result.returncode == 0, result.stdout + result.stderr

    @pytest.mark.parametrize("method", ("open", "read", "lazy", "paged"))
    def test_coalesced_entities_keep_indices_and_inverse_references_once(method, tmp_path):
        import ifcopenshell

        ifcopenshell.get_log()
        model = ifcopenshell.file(schema="IFC4")
        wall = model.create_entity("IfcWall", GlobalId="0000000000000000000001")
        group = model.create_entity("IfcGroup", GlobalId="0000000000000000000002")
        rel = model.create_entity("IfcRelAssignsToGroup", RelatedObjects=[wall], RelatingGroup=group)
        text = model.to_string().replace("ENDSEC;\nEND-ISO", str(wall) + ";\n" + str(rel) + ";\nENDSEC;\nEND-ISO")
        path = tmp_path / "duplicates.ifc"
        path.write_text(text)
        if method in ("lazy", "paged"):
            native = ifcopenshell.ifcopenshell_wrapper.file.create_uninitialized()
            getattr(native, "lazy_loading" if method == "lazy" else "paged_reading")(True)
            assert native.initialize(str(path))
            loaded = native
        else:
            loaded = ifcopenshell.open(path) if method == "open" else ifcopenshell.file.from_string(text)
        assert len(loaded.by_type("IfcWall")) == 1
        assert len(loaded.by_type("IfcRelAssignsToGroup")) == 1
        kept = loaded.by_guid(wall.GlobalId)
        assert kept.id() == wall.id()
        assert len(loaded.get_inverse(kept, allow_duplicate=True)) == 1
        assert loaded.by_id(rel.id()).RelatedObjects == (kept,)
        diagnostics = ifcopenshell.get_log()
        assert f"STEP duplicate #{wall.id()}: identical definition coalesced" in diagnostics
        assert f"STEP duplicate #{rel.id()}: identical definition coalesced" in diagnostics
        assert "count=2" in diagnostics
        import json

        events = [
            json.loads(line.split("PARSER_EVENT_V1 ", 1)[1])
            for line in diagnostics.splitlines()
            if "PARSER_EVENT_V1 " in line
        ]
        assert len(events) == 2
        assert {int(event["entity_id"]) for event in events} == {wall.id(), rel.id()}
        assert all(event["category"] == "step_deduplicated" and event["outcome"] == "recovered" for event in events)
        assert len({event["detail_id"] for event in events}) == 2
