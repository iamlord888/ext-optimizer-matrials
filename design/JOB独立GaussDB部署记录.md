# JOB 独立 GaussDB 部署记录 / Isolated JOB GaussDB Deployment

日期：2026-09-09。状态：该远端部署已停止；后续实验改用本机 Docker `gaussdb-local`，本记录仅保留历史部署证据。

## 历史实例 / Historical Instance

| 项目 | 值 |
| --- | --- |
| SSH | `pg@1.92.113.201` |
| OS/数据库用户 | `pg` / `pg`（不是原共享实例的 omm） |
| 根目录 | `/data/pg/gaussdb-job-timeout-20260908` |
| 数据目录 | `/data/pg/gaussdb-job-timeout-20260908/data` |
| GaussDB 安装 | `/data/pg/gaussdb-job-timeout-20260908/gaussdb` |
| 数据库 | `job` |
| 主端口 | `25432`，仅监听 `127.0.0.1`；内部端口 `25433` |
| 推荐连接 | Unix socket `/data/pg/gaussdb-job-timeout-20260908/run` |
| 版本 | GaussDB Kernel 507.0.0 build d414f3bb |
| GaussDB 构建 | `507.0.0 build d414f3bb` 及其匹配 SDK |
| 插件 | 该远端实例使用协议精简前的历史 extopt；不能代表当前 DATA/END、row5 package |
| 当前客户端 | `source/current/package/hint_adviser/bin/tuning_agent`，远端原生 Make 构建 |

远端无 Docker/Podman，因此采用镜像中安装文件的原生非 root 部署，不安装系统级容器守护进程、不修改其他人的 GaussDB。实例根目录、数据目录和 socket 目录均为 700，socket 为 700，凭据文件为 600。TCP 保持 loopback + sha256；当前原生 PostgreSQL libpq 不支持该内核认证交换，实验统一走私有 Unix socket。

## 连接与管理 / Access And Lifecycle

在远程 `pg` 用户 shell 中：

```bash
/data/pg/gaussdb-job-timeout-20260908/db.sh sql
/data/pg/gaussdb-job-timeout-20260908/db.sh status
/data/pg/gaussdb-job-timeout-20260908/db.sh stop
/data/pg/gaussdb-job-timeout-20260908/db.sh start
```

运行当前 adviser：

```bash
/data/pg/gaussdb-job-timeout-20260908/adviser.sh \
  --verify-initial-plan=true --search-timeout-ms=300000 \
  --enable-extoptimizer=false --explain-timeout-ms=10000 \
  -f /data/pg/gaussdb-job-timeout-20260908/incoming/sql/1a.sql
```

wrapper 已固定本实例的 socket、端口、用户和 job 库，并设置本次构建的 libpq/SO 搜索路径，不在 argv 中传密码。密码存于远端 `credentials.json` 与 `.pgpass`，不复制到本地报告。实例由 gs_ctl 后台运行，未创建开机自启服务；服务器重启后需执行 start。

## 镜像与源码 / Provenance

- 仓库：`https://gitcode.com/ext-optimizer-group/gaussdb-docker-extension`，master `9f0f5bfcd5151b6443e6fe2fa3a3aeda94a0b923`。
- 镜像归档：`cent/gaussdb-local-507.0.0-centralized-initialized-linux-amd64.tar.gz`，655885563 bytes。
- 归档 SHA256：`663630a2674038a375875b440d217f7806ceb5f9230396d060d56b0a42358a8a`，与仓库 checksum 及 LFS 对象一致。
- 镜像 config SHA256：`9b716de550793bc25b9ab62dab8eb13a935441c1ff55f359b907ed01c4972af6`。远端 GitCode 认证不可用，经本机 LFS 拉取校验后加载并从未启动容器提取 `/opt/gaussdb`，只传安装文件；没有复制已运行实例的数据目录。
- 本机两个临时提取容器已删除，本机原 gaussdb-local 镜像标签已恢复，运行实例未停止。
- adviser 基础 HEAD `6bc9013d5f4dbe03e2987ef6c7bbd1439725f352`，包含当前工作区实现与全部修复；对 master 的 tracked binary diff SHA256 为 `9fbd9e8cd242f0dee52fef63b53d28bf8670dd9fbeccf8811eb72aeb4b7d02f6`。具体文件和产物 hash 见 manifest。
- 插件先由独立 `source/extopt-gaussdb` Make 编译安装，再构建完整当前源码的 package。当前源码位于 `source/current`，不覆盖此前 job29c 包。

## 数据 / Data

从本机 gaussdb-local 的 `/tmp/imdb` 读取已验证 CSV，不连接或导出他人的远程 job 库。源 CSV 共约 4.1GB，21 文件 SHA256 在传输前后全部匹配。使用这份数据实际对应的 USTORE schema：通过本地 gs_dump 分别导出 pre-data/post-data，避免标准 JOB schema 的个别 varchar 宽度与 CSV 不匹配。

串行导入 21 张表；每张表 COPY 后执行精确 count，与历史已验证行数逐一比较；随后建立 21 主键与 23 其他索引，共 44 索引，并执行 ANALYZE，得到 108 条 public 列统计。job 逻辑大小约 13GB，数据目录含 WAL/undo 约 17GB。

代表行数：cast_info=36,244,344，movie_info=14,835,711，title=2,528,312，person_info=2,963,632。全部行数及每表耗时见 `run/import-report.json`。113 条 JOB SQL 已复制并逐文件 hash；传输产生的两个 macOS metadata 文件移到独立目录，不计入 SQL 集。

为避免 /data 空间不足，导入完成的 CSV 已移动到 `/home/pg/gaussdb-job-input-20260908/imdb`，原 incoming/imdb 为指向它的符号链接；第一次管理员不匹配的空数据目录保留在该家目录下作诊断备份，没有删除。最新检查 /data 可用约 15GB，根盘约 26GB。

## 配置与隔离 / Configuration And Isolation

保持 query_dop=1、row executor、work_mem=64MB、shared_buffers=32MB、max_process_memory=12GB、max_connections=128、enable_thread_pool=off、enable_adaptive_cost=off、enable_feedback_cardest=off。自有实例持久配置已设置 `enable_nestloop=off`、`enable_mergejoin=off`、`enable_index_nestloop=on`、`enable_hashjoin=on`：关闭普通 Nested Loop 和 Merge Join，保留索引 Nested Loop 与 Hash Join。客户端不再重复传递这些 `-S` 设置；每批测试前通过 `SHOW` 校验持久配置实际生效。客户端连接时设置 cost_param=2，数据库登录默认仍为 0。

导入单进程 nice/ionice 低优先级，编译 -j2。只在本实例做初始化、角色/认证处理、扩展安装和数据导入，没有修改全机配置或原 29000 实例。不同数据库仍共享物理 CPU、内存和磁盘，不能声称资源完全隔离；后续 benchmark 单 worker，所有结果、日志和 SQL 副本写 `/home/pg/data/job-timeout-benchmark-*`，不写公共 `/data`。

初次初始化的管理员名与 OS 用户不同导致登录限制，保留空实例后改为 pg/pg；密码限制为最多 32 字符，使用独立随机密码。gsql 重复 -c 只执行最后一条，因此部署脚本已改为逐次命令；新实例需显式 enable_extension 才能创建扩展。这些只影响本次部署脚本，不修改产品代码。

## 验证 / Verification

- Make/package：Rows evidence/policy、HAPF framing、SQL/query-source、PRETTY/plan identity、Nested Loop 角色测试全部通过。初次并行测试缺 generated headers、运行期缺库路径的问题分别通过前置 generated-headers 和明确 LD_LIBRARY_PATH 解决，未绕过测试。
- 该实例上的 capability 和 HAPF 结果属于协议精简前历史验证，不用于当前验收。
- 当前协议必须在本地 `gaussdb-local` 中以布尔 capability、DATA/END 和 row5 observation 重新验证。
- adviser 实际 JOB 表 Partial smoke：title 与 kind_type Join，150ms 超时，`PARTIAL`、proof valid=true、trusted_nodes=1、hint_added=true。
- 正常 adviser smoke：title id=1，保留动态采样及 verify-initial-plan，Converged、delivered、plantrace=ok，rc=0。
- 本轮只验证部署可用，不开始 113 条 JOB 调优或宣称 A/B 等价。

远端证据：`logs/import.log`、`logs/client-build-final.log`、`logs/hapf-smoke.*`、`logs/adviser-smoke/summary.tsv`、`logs/adviser-partial-smoke/`、`run/manifest.json`、`run/import-report.json`、`run/verification.json`。

本地副本位于 `.scratch/timeout-explain-analyze-integration/remote-deployment/`；部署、导入、验证和管理脚本也保存在此。旧认证排障脚本仅作为历史诊断，不应对当前已导入实例重跑。

## English Summary

This file records a stopped and removed remote instance that used the pre-simplification protocol. Its dataset and cancellation results remain historical evidence only. Current acceptance uses the local `gaussdb-local` instance, a package built from the matching SDK, boolean capability, DATA/END framing, and row5 observations.
