#!/usr/bin/env bash
# Install the Ubuntu build and PostgreSQL development environment for OrderSystem.

set -Eeuo pipefail

log() {
    printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
    printf '\nError: %s\n' "$*" >&2
    exit 1
}

if [[ ! -r /etc/os-release ]]; then
    fail "Cannot identify this Linux system. Ubuntu 22.04 or newer is required."
fi

. /etc/os-release

if [[ "${ID:-}" != "ubuntu" ]]; then
    fail "This script supports Ubuntu only (detected: ${PRETTY_NAME:-unknown})."
fi

if ! dpkg --compare-versions "${VERSION_ID:-0}" ge "22.04"; then
    fail "Ubuntu ${VERSION_ID:-unknown} is too old. Ubuntu 22.04 or newer is required."
fi

command -v sudo >/dev/null 2>&1 || fail "sudo was not found. Install sudo or use an administrator account."

log "Checking administrator access"
sudo -v

log "Updating Ubuntu package lists"
sudo apt-get update

log "Installing C++, CMake, Protobuf, PostgreSQL, and libpq development files"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    protobuf-compiler \
    libprotobuf-dev \
    libspdlog-dev \
    libpq-dev \
    postgresql \
    postgresql-client

log "Checking CMake version"
cmake_version="$(cmake --version | awk 'NR==1 {print $3}')"
if ! dpkg --compare-versions "$cmake_version" ge "3.22"; then
    fail "Installed CMake is $cmake_version, below the required 3.22. Upgrade CMake first."
fi

log "Checking tools and development libraries"
g++ --version | head -n 1
printf 'CMake: '
cmake --version | head -n 1
printf 'Ninja: '
ninja --version
printf 'Protobuf: '
protoc --version
printf 'libpq: '
pkg-config --modversion libpq

dpkg-query -W -f='spdlog: ${Version}\n' libspdlog-dev
dpkg-query -W -f='Protobuf development files: ${Version}\n' libprotobuf-dev

pkg-config --exists libpq || fail "pkg-config cannot find libpq. PostgreSQL development files are missing."
test -f /usr/include/postgresql/libpq-fe.h || fail "PostgreSQL client header libpq-fe.h was not found."
test -f /usr/include/google/protobuf/message.h || fail "Protobuf C++ headers were not found."
test -f /usr/include/spdlog/spdlog.h || fail "spdlog headers were not found."

log "Checking PostgreSQL service"
if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    if systemctl list-unit-files postgresql.service >/dev/null 2>&1; then
        sudo systemctl enable --now postgresql
        systemctl --no-pager --full status postgresql || true
    else
        printf 'postgresql.service was not found. Check the PostgreSQL installation.\n'
    fi
else
    printf 'systemd is unavailable; start PostgreSQL using the service manager for this environment.\n'
fi

printf '\n============================================================\n'
printf 'OrderSystem build environment installation completed.\n'
printf 'Ubuntu: %s\n' "${PRETTY_NAME:-unknown}"
printf 'CMake: %s\n' "$cmake_version"
printf 'libpq: %s\n' "$(pkg-config --modversion libpq)"
printf '============================================================\n'

cat <<'NEXT'

Next steps:
1. Confirm that NebulaRPC source is available at $HOME/.vs/NebulaRPC.
2. Create the PostgreSQL role and database (both named ordersystem_bench), then load sql/schema.sql and sql/seed.sql.
3. Configure and build:

   cmake --preset linux-release -DNEBULARPC_ROOT="$HOME/.vs/NebulaRPC"
   cmake --build --preset build-linux-release \
       --target order_db_benchmark_server order_db_benchmark_client \
       -j"$(nproc)"

This script installs and starts PostgreSQL. It does not create the application role,
database, schema, or benchmark records.
NEXT
