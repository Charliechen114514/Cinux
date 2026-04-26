#!/bin/bash
#
# scripts/compile/create_env.sh
# Create a Python virtual environment and install dependencies for Cinux build scripts.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
REQ_FILE="$SCRIPT_DIR/requirements.txt"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }

python_cmd=""
for cmd in python3 python; do
    if command -v "$cmd" &>/dev/null; then
        python_cmd="$cmd"
        break
    fi
done

if [[ -z "$python_cmd" ]]; then
    error "Python not found. Please install Python 3.8+ first."
    exit 1
fi

py_version=$("$python_cmd" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
info "Using Python $py_version ($python_cmd)"

if [[ -d "$VENV_DIR" ]]; then
    warn "Virtual environment already exists at $VENV_DIR"
    read -rp "Recreate? [y/N] " answer
    if [[ "$answer" =~ ^[Yy]$ ]]; then
        rm -rf "$VENV_DIR"
    else
        info "Updating packages in existing environment..."
        source "$VENV_DIR/bin/activate"
        pip install -q --upgrade pip
        pip install -r "$REQ_FILE"
        info "Done. Activate with: source $VENV_DIR/bin/activate"
        exit 0
    fi
fi

info "Creating virtual environment..."
"$python_cmd" -m venv "$VENV_DIR"

source "$VENV_DIR/bin/activate"

info "Upgrading pip..."
pip install -q --upgrade pip

info "Installing dependencies from requirements.txt..."
pip install -r "$REQ_FILE"

info ""
info "Virtual environment created successfully!"
info ""
info "Activate with:"
info "  source $VENV_DIR/bin/activate"
info ""
info "Then run:"
info "  python scripts/compile/cinux-cli.py --help"
info "  python scripts/compile/cinux-interactive.py"
