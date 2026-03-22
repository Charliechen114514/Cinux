#!/bin/bash
#
# Shared logging utilities for IMX-Forge scripts
#
# Usage:
#   source "${SCRIPT_DIR}/../lib/logging.sh"
#   log_info "Some message"
#

# Color definitions (exported for use in other scripts)
# Both LOG_* and unprefixed versions are exported for compatibility
export LOG_RED='\033[0;31m'
export LOG_GREEN='\033[0;32m'
export LOG_YELLOW='\033[1;33m'
export LOG_BLUE='\033[0;34m'
export LOG_NC='\033[0m' # No Color

# Backwards compatibility - also export unprefixed versions
export RED="${LOG_RED}"
export GREEN="${LOG_GREEN}"
export YELLOW="${LOG_YELLOW}"
export BLUE="${LOG_BLUE}"
export NC="${LOG_NC}"

# Logging functions
log_info() {
    echo -e "${LOG_GREEN}[INFO]${LOG_NC} $1"
}

log_error() {
    echo -e "${LOG_RED}[ERROR]${LOG_NC} $1" >&2
}

log_warn() {
    echo -e "${LOG_YELLOW}[WARN]${LOG_NC} $1"
}

log_debug() {
    if [[ "${DEBUG:-0}" == "1" ]]; then
        echo -e "${LOG_BLUE}[DEBUG]${LOG_NC} $1"
    fi
}

log_cmd() {
    echo -e "${LOG_YELLOW}[CMD]${LOG_NC} $1"
}

log_success() {
    echo -e "${LOG_GREEN}[SUCCESS]${LOG_NC} $1"
}
