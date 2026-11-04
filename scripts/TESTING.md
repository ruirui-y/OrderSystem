# OrderSystem 测试环境与运行说明

本文档是 OrderSystem 各阶段共用的测试环境说明。它只介绍：如何连接 Ubuntu、如何找到项目里的 Linux 测试脚本、如何运行指定阶段的测试，以及如何把结果同步回 Windows。

**各阶段具体运行哪个脚本、使用哪些参数、结果保存到哪个目录，以对应阶段目录中的脚本为准。** 第三阶段、第四阶段或后续阶段不需要各写一份环境连接文档；只需在本文件基础上，维护各自的测试脚本和结果同步脚本。

本文件不保存 SSH 密码、PostgreSQL 密码或私钥。

---

## 1. 测试环境

当前已配置的 Ubuntu 测试机：

| 项目 | 当前配置 |
|---|---|
| SSH 用户 | `yjj` |
| SSH 地址 | `192.168.184.128` |
| SSH 端口 | `22` |
| OrderSystem Linux 目录 | `/home/yjj/.vs/OrderSystem` |
| NebulaRPC Linux 目录 | `/home/yjj/.vs/NebulaRPC` |
| PostgreSQL 地址 | `127.0.0.1:5432` |
| 测试数据库 | `ordersystem_bench` |
| 数据库用户 | `ordersystem_bench` |

如果虚拟机 IP 或目录发生变化，以用户最新提供的环境信息为准，不要自行猜测新地址。

## 2. 从 Windows 连接 Ubuntu

在 Windows PowerShell 中执行：

```powershell
ssh -p 22 yjj@192.168.184.128
```

首次连接时，SSH 可能要求确认主机指纹；确认后按提示输入 SSH 登录密码，或者使用已经配置好的 SSH 私钥。

SSH 登录密码与 PostgreSQL 数据库密码是两套不同的凭据。不要把密码写进命令、脚本、日志或本文档。

登录后进入 OrderSystem 项目目录：

```bash
cd /home/yjj/.vs/OrderSystem
pwd
```

确认输出为：

```text
/home/yjj/.vs/OrderSystem
```

如果 SSH 连接失败，先报告连接错误和目标地址，不要反复尝试猜测密码，也不要修改 SSH 配置或密钥。

## 3. 确认 Linux 上的源码是最新版本

OrderSystem 的 Windows 工作区通过 Visual Studio 的 Linux 远程项目同步机制同步到 Ubuntu。

开始测试前：

1. 确认用户已经把本次要测的代码同步到 Linux。
2. SSH 登录后进入 `/home/yjj/.vs/OrderSystem`。
3. 用 `pwd` 确认当前目录。
4. 用 `find` 查看脚本和源码文件是否存在。

如果用户已经通过 Visual Studio 完成同步，不要重复同步。

如果确实需要手动同步，先确认 Windows 项目根目录和远端目标目录；不要未经确认使用带 `--delete` 的同步命令，以免删除 Linux 端文件。

## 4. PostgreSQL 密码与连接验证

### 4.1 密码保存位置

PostgreSQL 密码保存在 Ubuntu 用户自己的：

```text
/home/yjj/.pgpass
```

也就是：

```bash
~/.pgpass
```

不要执行 `cat ~/.pgpass`，不要读取、打印、复制或回传文件内容。Agent 只需要确认文件存在和权限正确。

### 4.2 `.pgpass` 权限

PostgreSQL 要求该文件不能被其他用户读取。检查并设置权限：

```bash
chmod 600 ~/.pgpass
test -f ~/.pgpass && stat -c '%a %n' ~/.pgpass
```

预期权限为 `600`。

`.pgpass` 每行格式如下，最后一列由用户在 Ubuntu 本机填写真实密码：

```text
主机:端口:数据库名:用户名:密码
```

当前测试连接对应的匹配项为：

```text
127.0.0.1:5432:ordersystem_bench:ordersystem_bench:用户本机配置的密码
```

### 4.3 验证 PostgreSQL

先避免空的 `PGPASSWORD` 覆盖 `.pgpass`：

```bash
unset PGPASSWORD DB_PASSWORD
```

再执行只读连接检查：

```bash
psql --no-psqlrc \
  --host=127.0.0.1 \
  --port=5432 \
  --username=ordersystem_bench \
  --dbname=ordersystem_bench \
  --command="SELECT 1;"
```

连接成功应返回 `1`。如果认证失败，报告错误即可；不要读取 `.pgpass`，不要把数据库密码写入命令行或环境变量后再打印出来。

**安全提醒：** 各阶段测试脚本可能会清空并重建压测表。只能使用专用的 `ordersystem_bench` 测试数据库，绝不能对真实业务数据库运行这些脚本。不要为了绕过脚本保护而擅自修改数据库名。

## 5. 在 Linux 项目中查找测试脚本

进入项目根目录后，先列出所有阶段的 Linux 测试脚本：

```bash
cd /home/yjj/.vs/OrderSystem
find scripts -maxdepth 3 -type f -name '*.sh' -print | sort
```

同时查看 Windows 结果同步脚本：

```bash
find scripts/windows -maxdepth 1 -type f \( -name '*.ps1' -o -name '*.bat' \) -print | sort
```

当前项目按阶段存放 Linux 测试脚本：

```text
scripts/
├── common/              # 各阶段共用的数据库准备脚本
├── env/                 # Ubuntu 环境安装脚本
├── phase1_sync_worker/  # 第一阶段：DB Worker
├── phase2_async/        # 第二阶段：EventLoop 非阻塞数据库访问
└── phase3_pipeline/     # 第三阶段：PostgreSQL Pipeline
```

以后增加新阶段时，继续使用 `scripts/phaseN_.../` 目录，不要把不同阶段的测试入口混在一起。

## 6. 选择并运行测试

不要默认运行 `scripts/` 下的所有脚本。应根据用户当前要测试的阶段，只运行该阶段目录里的测试入口。

运行前先查看目标脚本开头的用途说明和参数说明：

```bash
sed -n '1,180p' scripts/对应阶段/目标测试脚本.sh
```

确认测试目标、请求数量、并发参数、数据库名和结果输出目录后，再执行：

```bash
bash scripts/对应阶段/目标测试脚本.sh
```

如果脚本支持环境变量参数，应以脚本中的实际默认值和参数校验为准，不要猜测变量名。

各阶段的测试入口和测试顺序由各自目录中的脚本维护。例如：

- `scripts/phase1_sync_worker/`：第一阶段 DB Worker 测试。
- `scripts/phase2_async/`：第二阶段非阻塞数据库测试。
- `scripts/phase3_pipeline/`：第三阶段 Pipeline 测试。

不要运行已经被后续实现替代或明确废弃的旧脚本。若脚本与当前源码配置不一致，停止并报告，不要自行修改源码或脚本。

### 编译约定

Linux Release 构建目录通常为：

```text
build/linux-release/
```

项目使用 CMake Presets。需要编译时先查看当前 `CMakePresets.json` 和已有构建产物，再按用户要求执行对应 preset。

如果用户已经确认当前源码编译通过，且源码没有变化，不要为了测试重复完整编译。不要擅自删除 `build/` 或清理构建目录。

## 7. Linux 测试结果保存

每个阶段的公共测试脚本会设置自己的结果输出根目录，通常位于 Linux 用户目录下的：

```text
~/rpc-bench/
```

不要猜测具体阶段目录。运行前从该阶段公共脚本中确认输出变量和默认目录：

```bash
grep -nE 'OUTPUT_ROOT|RESULTS_ROOT|RUN_DIR|REMOTE_OUTPUT' scripts/对应阶段/*common*.sh
```

每次测试通常会创建独立的 `run_id` 子目录。保留所有原始结果，不要覆盖或删除之前的测试记录。

测试完成后，至少记录：

- 测试阶段和脚本名称。
- `run_id`。
- 实际测试参数。
- 进程退出码。
- PASS / FAIL 结果。
- QPS、平均延迟、P50、P95、P99 等脚本实际提供的指标。
- 客户端、服务端和数据库重置日志的位置。

没有实际统计的指标不要编造；脚本没有记录 Pipeline Sync 数量时，不要声称已经测得批次聚合情况。

## 8. 从 Linux 把结果同步回 Windows

结果同步在 **Windows PowerShell** 中执行，不是在 Ubuntu SSH 终端里执行。

先回到 Windows 的 OrderSystem 项目根目录，查看当前阶段对应的同步脚本：

```powershell
Get-ChildItem .\scripts\windows\sync_*_results.ps1
```

同步脚本通常会自动查找 Linux 上最新的一轮结果，并复制到 Windows 项目下的 `perf-results/` 对应阶段目录。

例如第三阶段：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\scripts\windows\sync_phase3_results.ps1"
```

也可以使用对应的 `.bat` 包装脚本：

```powershell
.\scripts\windows\sync_phase3_results.bat
```

如果需要下载指定的 `run_id`，先查看该阶段 `.ps1` 的参数定义，再传入 `-RunId`。如果虚拟机 IP 变化，使用脚本提供的 `-SshTarget` 参数；如果使用私钥，使用脚本提供的 `-IdentityFile` 参数。

Windows 需要安装 OpenSSH Client，能够找到 `ssh.exe` 和 `scp.exe`。首次连接可能需要确认 SSH 主机指纹并输入 SSH 登录凭据。

SSH 登录凭据只用于复制文件；PostgreSQL 密码仍由 Ubuntu 的 `~/.pgpass` 提供，两者不要混淆。

同步完成后，确认结果已落到项目根目录的：

```text
perf-results/对应阶段/
```

不要把性能结果复制到 `scripts/` 源码目录，也不要将 `.perf.data`、日志、CSV 或压缩包擅自加入 Git。

## 9. Agent 执行边界

Agent 接手测试任务时，只负责环境连接、运行用户指定阶段的测试、收集结果和同步文件。

允许：

- 使用 SSH 连接已配置的 Ubuntu 测试机。
- 检查 `.pgpass` 是否存在及权限是否为 `600`，但不读取内容。
- 检查 PostgreSQL 是否可连接。
- 查找并运行用户指定阶段的 Linux 测试脚本。
- 按需编译（仅在源码变化、构建产物缺失或用户要求时）。
- 使用 Windows PowerShell 中对应的同步脚本下载结果。
- 汇报测试结果和日志路径。

禁止：

- 修改 C++ 源码、SQL、测试脚本或配置来“修复”测试。
- 修改 SSH 密码、数据库密码、私钥或 `.pgpass` 内容。
- 输出或上传任何密码、私钥。
- 对非 `ordersystem_bench` 数据库运行会重置数据的测试。
- 未经确认删除测试结果、构建目录或其他文件。
- 执行 `git add`、`git commit`、`git push`。
- 将测试失败改写成成功，或编造没有实际采集的性能指标。

测试失败时，停止后续测试，保留日志，报告失败命令、退出码和错误信息，等待开发者处理。

---

**记住：本文件负责通用环境和工作流程；每个阶段目录中的脚本负责该阶段具体怎么测。**
