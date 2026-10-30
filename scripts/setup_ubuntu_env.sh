#!/usr/bin/env bash
# OrderSystem / NebulaRPC 一键 Ubuntu 环境安装脚本
# 虽然文件扩展名是 .txt，但内容是 Bash 命令；使用 bash environment.txt 执行。

set -Eeuo pipefail

log() {
    printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
    printf '\n错误：%s\n' "$*" >&2
    exit 1
}

if [[ ! -r /etc/os-release ]]; then
    fail "无法识别当前 Linux 系统。此脚本只支持 Ubuntu 22.04 或更新版本。"
fi

. /etc/os-release

if [[ "${ID:-}" != "ubuntu" ]]; then
    fail "当前系统不是 Ubuntu（检测到：${PRETTY_NAME:-unknown}）。"
fi

if ! dpkg --compare-versions "${VERSION_ID:-0}" ge "22.04"; then
    fail "当前 Ubuntu 版本为 ${VERSION_ID:-unknown}，项目环境要求 Ubuntu 22.04 或更新版本。"
fi

command -v sudo >/dev/null 2>&1 || fail "没有找到 sudo。请先安装 sudo 或使用有管理员权限的账户。"

log "检查管理员权限"
sudo -v

log "更新 Ubuntu 软件包列表"
sudo apt-get update

log "安装 C++、CMake、Protobuf、日志库、MySQL 服务端和客户端开发依赖"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    protobuf-compiler \
    libprotobuf-dev \
    libspdlog-dev \
    libmariadb-dev \
    mysql-server \
    mysql-client

log "检查 CMake 版本"
cmake_version="$(cmake --version | awk 'NR==1 {print $3}')"
if ! dpkg --compare-versions "$cmake_version" ge "3.22"; then
    fail "安装后的 CMake 版本是 $cmake_version，低于项目要求的 3.22。请升级 CMake 后再编译。"
fi

log "检查关键工具和开发库"
g++ --version | head -n 1
printf 'CMake: '
cmake --version | head -n 1
printf 'Ninja: '
ninja --version
printf 'Protobuf: '
protoc --version
printf 'MariaDB client library: '
pkg-config --modversion libmariadb

dpkg-query -W -f='spdlog: ${Version}\n' libspdlog-dev
dpkg-query -W -f='Protobuf development files: ${Version}\n' libprotobuf-dev

pkg-config --exists libmariadb || fail "pkg-config 找不到 libmariadb，MariaDB 开发库检查失败。"
test -f /usr/include/google/protobuf/message.h || fail "没有找到 Protobuf C++ 头文件。"
test -f /usr/include/spdlog/spdlog.h || fail "没有找到 spdlog 头文件。"

log "检查 MySQL 服务"
if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    if systemctl list-unit-files mysql.service >/dev/null 2>&1; then
        sudo systemctl enable --now mysql
        systemctl --no-pager --full status mysql || true
    else
        printf '没有发现 mysql.service，请检查 MySQL 安装情况。\n'
    fi
else
    printf '当前环境没有可用的 systemd；MySQL 服务请按当前系统方式启动。\n'
fi

printf '\n============================================================\n'
printf 'OrderSystem 编译环境安装完成。\n'
printf 'Ubuntu: %s\n' "${PRETTY_NAME:-unknown}"
printf 'CMake: %s\n' "$cmake_version"
printf '============================================================\n'

cat <<'NEXT'

下一步：
1. 确认 NebulaRPC 源码位于：$HOME/.vs/NebulaRPC
2. 进入 OrderSystem 目录。
3. 配置并编译：

   cmake --preset linux-release -DNEBULARPC_ROOT="$HOME/.vs/NebulaRPC"
   cmake --build --preset build-linux-release \
       --target order_db_benchmark_server order_db_benchmark_client \
       -j"$(nproc)"

注意：
- 此脚本会安装并启动 MySQL 服务端，但不会创建数据库、账号或测试数据。
- 数据库初始化仍需按项目 docs/database-operation-benchmark.md 执行。
- 不会修改 NebulaRPC 或 OrderSystem 的源码，也不会执行 Git 操作。
NEXT
