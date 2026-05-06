import subprocess
from pathlib import Path

import pytest

PROJECT_ROOT = Path("/home/charliechen/cinux")
SCRIPT = PROJECT_ROOT / ".venv/bin/python3"
TOOL = PROJECT_ROOT / "tools/read_sdm.py"
VOL1 = PROJECT_ROOT / "document/reference/intel/SDM-Vol1-Basic-Architecture.pdf"
VOL3A = PROJECT_ROOT / "document/reference/intel/SDM-Vol3A-System-Programming-Guide-Part1.pdf"


@pytest.fixture
def vol3a():
    return str(VOL3A)


@pytest.fixture
def vol1():
    return str(VOL1)


def run_tool(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(SCRIPT), str(TOOL), *args],
        capture_output=True, text=True, timeout=30,
    )
