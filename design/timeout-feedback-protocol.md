# 超时反馈内部协议 / Timeout Feedback Internal Protocol

HAPF 是 `hint_adviser` 与同一交付包中的 `extopt.so` 之间的内部 notice 协议。
两侧不提供跨版本兼容，也不交换 Build ID、架构、指针宽度或私有 ABI 标识。
`tuning_agent`、`libexternal_optimizer.so`、`extopt.so` 和目标 GaussDB SDK 必须成套构建和部署。

## SQL 捕获开关 / Capture Switch

```sql
SELECT extopt.partial_capability(<execution_id>);
```

函数返回布尔成功值。正 execution ID 开启捕获，作用于当前会话的下一次 `ExecutorRun`；`0` 关闭捕获；
负数拒绝。execution ID 是 round 隔离标识，不是版本或 ABI 标识。

## Notice 分帧 / Notice Framing

```text
HAPF <execution_id> <sequence> DATA <base64_payload>
HAPF <execution_id> <sequence> END
```

- 客户端只把以 `HAPF ` 开头的 primary notice 交给当前 assembler。
- execution ID 必须等于当前 round。
- sequence 从 `0` 开始连续递增。
- 一个或多个 `DATA` 后必须有一个 `END`；`END` 后不接受更多消息。
- base64 只需能由标准 decoder 解码，不要求重新编码后的规范形式完全相同。
- 解码后的总 payload 不得超过 8 MiB；单条 notice 使用固定缓冲上限。

协议不发送 `BEGIN`、版本、bundle、Build ID、架构、指针宽度、ABI、总字节数、chunk 数、
observation 声明数或 CRC。libpq/TCP 提供可靠有序传输，sequence 和 `END` 用于确认应用层快照完整结束。

## Payload

```text
PRETTY-BYTES=<N>
<N bytes of original PRETTY>
OBSERVATIONS
plan_id,state,rescan_count,A_rows,E_rows
```

`PRETTY-BYTES` 是 PRETTY 与 observation 区域的唯一边界。observation 解析到 payload 结束，
允许零条记录。每条记录必须以 LF 结束。

`state` 使用固定枚举：

```text
0 = INCOMPLETE   已执行，但未证明当前 epoch 结束
1 = ACTIVE       快照时仍在一次执行调用中
2 = TERMINAL     当前 epoch 已到达本地 EOF
```

服务端只序列化已执行节点，因此协议不再发送 `executed`。客户端不使用 PlanState 指针做映射，
因此协议不再发送 `state_identity`。plan ID 必须为正且唯一；state 和 rescan count 必须是合法
非负整数；A/E rows 必须有限且非负。

## 服务端边界 / Server Boundary

服务端仍只遍历已知 PlanState 布局，并保留必要的空指针、节点总量和数值范围保护。
无 Instrumentation 的节点被跳过；无法安全遍历整个计划时不发送 HAPF 快照。
这些是实现保护，不是客户端/服务端版本协商。

## 客户端消费 / Client Consumption

客户端只有在以下条件都满足时才消费 observation：

1. 查询以 SQLSTATE `57014` 结束，结果已 drain，连接已 idle 或完成重连。
2. 当前 execution ID 的 `DATA/END` 序列完整，并能在资源上限内解析。
3. 原 PRETTY、相同 candidate 的重新规划 PRETTY 和 JSON 通过当前结构映射。
4. proof 根据 state、rescan count 和 A/E rows 产生 `EXACT`、`LOWER_BOUND` 或无证据。

proof 与 Rows evidence 合并属于业务行为，不属于 HAPF 传输协议校验。
