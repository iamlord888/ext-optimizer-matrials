# 超时反馈实施记录 / Timeout Feedback Implementation

当前状态：Partial 内部协议已精简为 execution ID、连续 DATA/END、总 payload 上限、PRETTY 边界和 row5 observation。客户端、`extopt.so` 与目标 GaussDB SDK 采用成套构建和部署策略；不再交换或校验 HAPF 版本、bundle、Build ID、架构、指针宽度或私有 ABI。下文旧验证日志若使用 V3/row8/CRC，只作为历史实现记录。

## 基线 / Baseline

2026-09-05 在现有 `impl-refactor@559add378e472ed8725cf2c3a9e5cf67dc0ad11f` 开始。
保留磁盘最新规格与 Ticket 08。实现契约为 `超时EXPLAIN_ANALYZE部分反馈需求串讲.md`。
本地 tracker 位于 `.scratch/timeout-explain-analyze-integration/`。

## PG 阶段对照 / PG Stage Alignment

| 阶段 / Stage | PG 位置与调用链 | 外置路径 | 依赖、边界与可观察行为 |
| --- | --- | --- | --- |
| 规划 | `commands/explain.c:ExplainOneQuery -> planner -> ExplainOnePlan` | `load_query_info -> ExtOptInitializeQuery -> sealed snapshot -> optimize_sql_with_rows_hint` | 使用既有 metadata/shim/fmgr/statistic/collation/encoding 闭包；本特性不改公式或规划阶段；原生/外置规划都交给 GaussDB 执行 |
| 执行 | `commands/explain.c:ExplainOnePlan -> ExecutorStart -> ExecutorRun -> ExecutorFinish -> ExecutorEnd` | `feedback_round_execute -> execute_explain_analyze -> GaussDB EXPLAIN ANALYZE` | 完整路径沿用 PRETTY A-rows/Total runtime；部分路径只在 ExecutorRun 对象仍有效时捕获，后续取消清理不能再读取 PlanState |
| 行级计数 | `executor/execMain.c:ExecutePlan -> ExecProcNode -> ExecProcNodeInstr` | 匹配 SDK 的 GaussDB 普通 row executor Instrumentation adapter | 服务端只发送已执行节点，并统一为 INCOMPLETE/ACTIVE/TERMINAL、rescan、A/E；MultiExec Hash 排除但子节点继续；不依据本地 EOF 推断将来不再 rescan |
| 结构与反馈 | `ExplainPrintPlan` 与节点树 | PRETTY/JSON -> PlanNode -> Rows target -> Rows state | Partial 必须使用准确 candidate SQL replan；映射矛盾整轮拒绝；Rows 生成保留 master 的 inner Scan 抑制和 inner Join 反馈 |
| 生命周期 | PG 查询取消错误展开、事务清理 | 单 PGconn -> PQcancel -> 终止 SQLSTATE -> 有限 drain -> idle/重连 | 正常连接才复用；Context 仅保存 sealed metadata，不绑定执行 PGconn；以真实连接与 SQL 日志验收 |

## 状态 / Status

`final-all-*.log` 是 rebase 前的历史验收，不能证明当前基于 `6bc9013d5f` 的工作区通过。本轮针对评审指出的三处缺陷重新构建并验证，记录见下文及 Ticket 05/06/08/09/10。

- 生产映射只比较结构身份：节点数量、父子拓扑、node id、query block 冲突、anchor 和 E-rows。evidence 只依赖统一 execution state、rescan 和 A/E。
- 全局预算耗尽时仍执行结果及 snapshot 的无 SQL 校验；序号错误、缺 END 和 handler 错误为 `ERROR`，合法取消才为 `TIMEOUT_NO_TRUSTED_NODES`。跳过清理 SQL 的脏连接仍关闭。
- 有限恢复复用 `PQconninfo`、已解析端点及实际密码，使用 `PQconnectStartParams -> PQconnectPoll -> wait_for_socket`；TCP 启动、认证和恢复 SQL 共用绝对期限。恢复阶段不重新解析 DNS、不提示密码，也不切换多主机候选；无已解析端点则失败。
- Make 依赖恢复为 master 的 `libexternal_optimizer.so` 与原生 `libpq`，保留 Partial 模块及测试。容器旧 libpq 对象曾导致握手超时，清理该库并重新构建后解决。

## 环境 / Environment

`gaussdb-local` 使用 GaussDB 507.0.0 build d414f3bb 的 SDK 和运行实例。当前验收要求从该 SDK 构建 `extopt.so`，与同轮客户端 package 一起安装；不再以运行时 Build ID 或私有 ABI identity 作为协议条件。

## 精简协议验证 / Simplified Protocol Validation

2026-09-16 在 `gaussdb-local` 的 `hapf_integration` 中安装本轮 package、重启数据库进程以卸载旧 SO，并运行 `contrib/hint_adviser/t/validate_feedback.sh`。完整结果为 `validation_rc=0`，容器日志 `/tmp/validate-feedback-row5.log`。

- `partial_snapshot_test`：DATA/END、execution ID、连续序号、payload 上限、PRETTY 边界和 row5 通过。
- `connection_test`：statement timeout 与 PQcancel 均保留 57014、drain idle 和 4 个 observation。
- `feedback_round_test`：COMPLETE、PARTIAL、TIMEOUT_NO_TRUSTED_NODES、ERROR 四结果通过。
- `feedback_integration.py`、`sql_input_integration.py`、`topology_test.py` 和精简后的 `fault_test.py` 全部通过。
- 正确 package `package/hint_adviser/bin/tuning_agent` 在 `tpcds_sf01` 的真实 smoke 返回 `rc=0`、summary v14、`finish_reason=Converged`、`plantrace_status=ok`。

本地 validation skill 的最后 smoke 仍硬编码历史 `contrib/hint_adviser/package/hint_adviser/client` 路径和旧 summary 大小写，因此该脚本的最后一步失败不代表当前根 package；上述正确 package smoke 单独执行并通过。

## 命名对齐 / Naming Alignment

2026-09-17 将本轮新增的 `hint_adviser` 和 `extopt-gaussdb` 文件、函数及测试入口统一为各自已有模块使用的小写下划线风格。客户端五个生产模块现在使用 `hint_adviser_feedback_*`、`hint_adviser_partial_*` 和 `hint_adviser_pretty_parser.*` 文件名；服务端新增模块使用 `extopt_partial_*` 文件名。函数、文件内辅助函数、测试文件及测试可执行文件同步改名；`FeedbackResult`、`PartialSnapshot` 等类型名和 `FEEDBACK_PARTIAL` 等枚举常量保持原约定。

SQL 接口 `extopt.partial_capability(bigint)` 的名称和返回值不变，其底层 C 符号由 `PartialCapability` 改为 `extopt_partial_capability`，与既有 `extopt_query_source`、`extopt_btree_tree_height` 保持一致。安装新 SO 和 SQL 文件后，已存在的 extension 登记必须重建，不能让数据库目录继续引用旧 C 符号。

该修改不改变控制流或协议。当前根 package 重新构建成功，`validate_feedback.sh` 完整通过 Rows、DATA/END row5、两种取消、四种单轮结果、16 组 SQL 输入、七类拓扑和故障矩阵；容器日志为 `/tmp/validate-feedback-naming.log`。

On 2026-09-17, new hint_adviser and extopt-gaussdb files and functions were aligned with each module's established lower-snake-case convention. Type names, enum constants, SQL API behavior, and the HAPF protocol are unchanged. The partial-capability C symbol is now `extopt_partial_capability`, so an existing extension registration must be recreated after installing the new package. The current package and full feedback validation matrix pass; see `/tmp/validate-feedback-naming.log`.

## 历史验证 / Historical Validation

以下是 rebase 前源码的历史结果，本轮未将其当作当前验收：

- `final-all-focused.log`: `hint_adviser_hint_test`、`feedback_round_test`、`connection_test` 全部通过；包含 `COMPLETE`、`PARTIAL`、`TIMEOUT_NO_TRUSTED_NODES`、`ERROR`、两种取消和 expired recovery deadline。
- `final-all-real.log`: `feedback_integration.py`、`topology_test.py` 和旧格式 `fault_test.py` 返回 0；已有完整 best 在 global `PQcancel` 后仍交付，final plantrace 使用干净重建会话。该日志早于协议精简。
- `final-all-meson.log`: 旧格式 `PartialSnapshot` 和 `PartialProofAndBudget` 2/2 通过。
- `final-all-tap.log`: `contrib/hint_adviser/t/001_basic.pl` 118/118 通过。

此前 skill 默认 smoke 路径仍指向旧的 `client/` 包布局；本轮使用当前 Make 产物 `/package/hint_adviser/bin/tuning_agent` 直接验证，避免把旧二进制结果混入结论。

## 评审修复验证 / Review Fix Validation

该轮历史评审只修改客户端及测试，使用当时已安装的旧协议扩展与隔离数据库 `hapf_integration`，未重新安装或重启服务端。日志位于 `.scratch/timeout-explain-analyze-integration/review-fixes/`，不作为精简后协议验收。

- 复用审计：PG `ExplainNode` 从同一 Plan 生成名称和类型；客户端通过两次 replan 获取 PRETTY/JSON，必须在 adapter 独立核验，不能从复制后的字段证明身份。没有引入新的 planner、metadata、fmgr 或估算阶段。
- 原生 libpq `PQconnectdbParams -> PQconnectStartParams -> pqConnectDBComplete` 的阻塞完成阶段，被客户端已有 `wait_for_socket` 驱动的 `PQconnectPoll` 替换；连接参数、认证和报错解析仍由 libpq 提供。
- Make/package、Rows evidence、`hint_adviser_feedback_policy`、旧 HAPF framing、映射与预算测试通过。精简后的 DATA/END/row5 协议需要本轮重新验证；真实四 outcome、两种取消、backend 复用、重连会话设置与 notice receiver 仍是当前验收范围。
- TCP 服务接受后不回应的用例耗时约 205ms；发出密码请求后不完成认证的用例约 211ms；均受 200ms 恢复预算约束并观察到 socket 关闭，未等到 5s connect_timeout。
- `feedback_integration.py` 和 `topology_test.py` 覆盖 native/runner、summary v14、完整 best、全局取消后的干净 plantrace、`EXACT` 对已发生 rescan 的拒绝、active/rescan 下界与完整 ANALYZE 对照。2026-09-11 后 Nested Loop 期望恢复为 master：inner Scan 抑制、inner Join 保留。
- `hapf-review-faults.log`：17 个故障与对照场景通过；新增四类错误在预算耗尽时仍禁止旧 best 交付，合法 snapshot 与无 HAPF 的两个对照保留交付，计数仍为 iterations=1、partial_iterations=0、best_iteration=1。
- `hapf-review-smoke.log`：当前包使用本次构建的原生 libpq，无缺失动态库；真实表查询返回 0，finish_reason=Converged、delivery_status=delivered、plantrace_status=ok。
- 当前 Meson/TAP/全量 Rows benchmark 未在本轮重新验收；旧 Meson 文件仍依赖 master 已移除的外置优化器目标，保留为独立构建整合缺口。

## 扫描身份修复 / Scan Identity Fix

防止同型 Scan 换位错绑依靠结构映射：node id、query block 冲突、anchor 和 E-rows 一致后才会绑定元数据。映射不比较 JSON `Node Type` 与 PRETTY 算子拼写，也不比较扫描对象、alias、schema、索引名和扫描方向。

anchor/preorder 遍历只建立候选配对，全部验证后统一发布元数据；失败不改变先前节点、PRETTY 消费状态或持久 hints。Partial proof 复用同一结构检查。Make/package、Rows/proof 测试、真实四 outcome、拓扑及 runner 交付已通过。详情及既有 SQL 引号处理、Meson 整合边界见 Ticket 11。

Structural mapping is what keeps same-shape scans apart: node id, query-block conflicts, anchors and E-rows must agree before metadata is published. The mapping does not compare JSON Node Type with the PRETTY operator spelling, nor scan object, alias, schema, index, or scan direction.

## 评审简化 / Review Cleanup

2026-09-08 仅实施评审提出的行为保持型简化，不修改本轮确认的解析、Nested Loop 或 Meson bug。

- 有限恢复为唯一重连入口；删除无界 ConnectionRecover 和生产不可达的负 deadline 分支，测试改走 connection_recover_until。初始连接仍使用原入口。
- 抽取 candidate_plan_complete，供 candidate_plan_build 和 Partial proof 之后共用别名/Rows target 补全；FeedbackPublishComplete 统一完整结果发布。关闭超时时仍由生成的 Outline SQL replan，开启时仍使用准确 candidate SQL；会话恢复流程因配置不同而保留。
- 服务端 execution ID 使用与 active_capture 相同生命周期的 THR_LOCAL 整数；移除 pthread key、分配/析构和无入口 shutdown 链。partial_capture_prepare 直接验证并安装 hook，失败抛 ERROR，成功仍输出 CAPTURE=1；没有修改 HAPF 版本、ABI 或布局偏移。
- 删除仅写不读的 observation tag、pretty_consumed 及无调用计数接口；移除新建 capture 上的恒真 guard。2026-09-11 为恢复 master 同轮 first-wins 与跨轮覆盖重新保留 `last_updated_round`。hook 回调由初始化保证非空，改用 Assert 表达该不变量。
- PG/外置阶段对照沿用上表：没有修改 planner、估算、metadata 或 PlanState 读取语义；复用原 libpq 恢复和原 PlanTree 补全逻辑，不引入平行实现。

当前 Make/package、Rows、proof、HAPF、真实四 outcome、关闭超时的完整结果、连接恢复、runner、拓扑 oracle、故障矩阵与收敛 smoke 通过。日志：`/tmp/timeout-cleanup-build.log`、`/tmp/timeout-cleanup-validation.log`；详细对照见 `.scratch/timeout-explain-analyze-integration/review-comparison/README.md`。

Cleanup preserves replan SQL selection, evidence rules and delivery behavior. Recovery now has one bounded implementation; plan completion and complete-result publication are shared. Thread-local capture identity and dead-state removal passed the current Make/package, focused, real-round, connection, topology, fault and convergence tests. Known parser, Nested Loop and Meson bugs remain unchanged pending a separate decision.

## 标点与子计划修复 / Punctuation And Child Role Fixes

2026-09-08 后续按用户决定修复上述三项客户端 bug，历史分支不修改，Meson 整合仍单独保留。

| 阶段 / Stage | PG 原生链路 | 本次客户端修改 | 边界与验证 |
| --- | --- | --- | --- |
| 名称格式化 | `ExplainNode -> quote_identifier`；同一 Plan 输出名称与结构 | `pretty_parse_snapshot/pretty_parse_row -> PrettyFindUnquoted`，共用引号外分隔符；anchor 识别 `[id, name]` | PRETTY 文本 adapter 专用，不复用会终止进程的 SQL 输入扫描；不修改 planner、metadata、估算或 HAPF |
| 子节点角色 | `ExplainNode` 在 `explain.c:2405` 先输出 InitPlan，再分别以 Outer/Inner 输出左右子树 | JSON 保留 Parent Relationship；Rows 按角色抑制 inner Scan，alias wrapper 转移原物理边角色 | 不再由 child 下标推测；inner Join 沿用 master 可生成 Rows；active/rescan 只拒绝 `EXACT`，A>E 仍保留 `LOWER_BOUND` |

已对齐 PG 的引号边界与子计划角色输出消费阶段，未修改 PG 原函数。`pretty_boundary_test.c` 使用完整映射、Partial proof、Rows 输出以及真实 feedback_round_execute 入口；两种特殊名称在真实完整/Partial 轮次输出 EXACT 100；前置 InitPlan 的 Nested Loop 保留正确 Outer，inner Scan 抑制而可寻址 inner Join 沿用 master 输出。

Make/package、全部聚焦测试、连接与四 outcome、runner、拓扑 oracle、故障矩阵均通过，日志 `/tmp/pretty-boundary-build.log`、`/tmp/pretty-boundary-validation.log`。skill 旧包检查不作当前验收，实际使用新 `package/hint_adviser/bin/tuning_agent`。真实动态采样 smoke 为 Converged/delivered/plantrace=ok，容器 `/tmp/pretty-boundary-smoke/summary.tsv`；本轮未替换或重启服务端。

仍有 gap：Meson 上游整合、全量 Rows benchmark 未验收；CLI query-tree 输入对某些 quoted alias 的前置解析限制不属于 PRETTY 错误，本轮没有修改。此前分支对照保留为历史事实，当前三项回放已经全部 PASS。

The three client bugs are fixed and regression-tested. PRETTY parsing respects quoted delimiters and genuine anchor syntax; Nested Loop feedback follows semantic child roles through alias wrappers. Real complete/Partial rounds, full feature tests and convergence smoke pass. Earlier query-source parsing limitations, Meson integration and full benchmark validation remain separate gaps.

## SQL 输入修复 / SQL Input Fixes

已对齐 PG `scan.l` 对双写引号的消费语义，以及 `outToken/pg_strtok` 对名称内部标点、反斜杠空白的 token 边界。保留现有 Flex/Bison parser 与单 SELECT 检查；未修改 planner、metadata、估算或服务端 ABI。复用审计、边界、红绿反例和验证结果见 `.scratch/timeout-explain-analyze-integration/issues/12-sql-input-quoting.md`。

`sql_input_test` 接入 Make check/installcheck 与 `validate_feedback.sh`，验证正确名称提取以及损坏 SQL/Query-source 拒绝。`sql_input_integration.py` 从完整 CLI 覆盖 16 种组合，两个原始失败 SQL 均可正常交付；外置 normal、完整 best 和 Partial-only 不交付的契约不变。现有全部特性矩阵与保留动态采样的收敛 smoke 通过，日志 `/tmp/sql-input-build.log`、`/tmp/sql-input-validation.log`。仍未运行全量 Rows benchmark；Meson 不再要求修复或验收。

Both earlier SQL/query-source input failures are fixed and validated through the actual CLI. Make/package, focused parser tests, 16 CLI combinations and the existing feature matrix pass. Meson is outside acceptance by user decision; full Rows benchmark validation remains outstanding.

## 双取消与空 Evidence 修复 / Dual Cancel And Empty Evidence Fix

2026-09-15 的 JOB 复测发现两项超时路径缺陷。`24b/27b/33c` 在短 adaptive timeout 下由服务端 `statement_timeout` 先返回 57014，使用相同 deadline 的客户端 watchdog 随后发送 PQcancel，迟到信号命中等待态 backend 并使 cleanup 失败。watchdog 现使用 effective timeout 加 2500ms grace，并受全局 search deadline 限制；2500ms 覆盖服务端 2000ms snapshot 上限。若仍实际发送 PQcancel，feedback round 会关闭并重建执行连接，不复用可能收到迟到信号的旧 backend。

另外 15 个首轮无 HAPF 的 JOB 样本均在根 Agg 上出现空 Instrumentation。此状态现在表示该节点无本地 evidence；adapter 跳过该节点并继续遍历。旧格式测试中这些样本能够发送完整快照，`6b` 也产生零 observation snapshot；精简后的 DATA/END 格式以本轮重新验证为准。

Make package、Rows/HAPF framing、真实 Connection/FeedbackRound、完整 topology 与 fault matrix 通过。带全局 deadline 的 1ms 服务端超时连续五次保留 `server_cancel` 和原 idle backend；watchdog fault 保留 `client_cancel`，并验证成功重连。当前 Make 产物的 JOB fixture smoke 为 `Converged`、`delivered`、summary v14。技能脚本的旧 `client/` package 路径仍是独立工具缺口，不作为本轮结果。

The server timeout has a 2500 ms head start over the PQcancel fallback. A feedback round reconnects after every sent PQcancel. A PlanState with no local Instrumentation contributes no observation and does not invalidate sibling evidence. The simplified internal protocol uses execution-scoped DATA/END messages and row5 observations; matched SDK packaging replaces runtime Build-ID/ABI negotiation.
