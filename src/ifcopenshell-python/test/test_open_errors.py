# This file was generated with the assistance of an AI coding tool.
"""File construction failures must restore Python's thread state before raising."""

import subprocess
import sys
from pathlib import Path


def run_case(method, duplicate, path):
    import ifcopenshell

    header = "ISO-10303-21;HEADER;FILE_DESCRIPTION((''),'2;1');FILE_NAME('','',(),(),'','','');FILE_SCHEMA(('IFC4'));ENDSEC;DATA;"
    data = header + "#1=IFCQUANTITYLENGTH('a',$,$,1.,$);#2=IFCQUANTITYLENGTH('b',$,$,2.,$);ENDSEC;END-ISO-10303-21;"
    if duplicate:
        data = data.replace("#2=", "#1=")
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
        normal = ifcopenshell.file.from_string(data.replace("#1=IFCQUANTITYLENGTH('b'", "#2=IFCQUANTITYLENGTH('b'"))
        assert len(normal.by_type("IfcQuantityLength")) == 2


if __name__ == "__main__":
    run_case(sys.argv[1], sys.argv[2] == "duplicate", Path(sys.argv[3]))
else:
    import pytest

    @pytest.mark.parametrize("method", ("open", "read", "lazy", "paged"))
    @pytest.mark.parametrize("case", ("unique", "duplicate"))
    def test_open_errors(method, case, tmp_path):
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), method, case, str(tmp_path / "input.ifc")],
            capture_output=True,
            text=True,
            timeout=20,
        )
        assert result.returncode == 0, result.stdout + result.stderr
