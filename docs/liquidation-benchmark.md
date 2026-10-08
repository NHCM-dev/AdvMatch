# 连锁强平压力测试

## 构建与运行

```powershell
cmake --preset release
cmake --build --preset release --target advmatch_liquidation_benchmark
.\build\release\advmatch_liquidation_benchmark.exe
```

程序为 `build/release/advmatch_liquidation_benchmark.exe`。默认构造 2 万风险用户、每人 4 个仓位、8 个永续产品，共 8 万风险仓位，50% 用户全仓、50% 逐仓。另有一个提供对手仓位和深度的账户，以及一个真实出资的保险金账户。所有市场命令通过 SingleThreadConsumer 执行，单生产者顺序推进冲击，避免压测参数本身引入乱序标记价。

这是一种模拟 2025-10-11 那类极端行情压力的可复现模型，没有使用当天的逐笔行情、订单簿、真实爆仓金额或用户仓位数据，不宣称复刻真实事件。

## 反馈模型

1. 在价格 100000 建立 5/10/20/40/80 倍杠杆的风险仓位，并预挂 reduce-only 平仓单。
2. 每轮外部价格冲击加深，上一轮之前已完成的强平成交量再增加价格冲击。
3. 全仓跨产品风险由真实风险缓存触发；逐仓按自身保证金触发。强平先撤单，再通过真实盘口成交。
4. 每轮只补充有限深度，盘口价格按配置增加滑点；初始保险金也有限。盘口为空或保险金不足时保留待强平状态。
5. 默认最后从真实资金账户补充保险金和深度，排空仍待强平的仓位，再检查现金和多空数量守恒。

```text
externalMovePpm = shockPercent × 10000 × wave / waves
feedbackMovePpm = impactPercent × 10000 × liquidatedQuantity / initialVictimQuantity
downMark = entryPrice × (1000000 - externalMovePpm - feedbackMovePpm) / 1000000
upMark   = entryPrice × (1000000 + externalMovePpm + feedbackMovePpm) / 1000000
```

价格反馈由 benchmark 的外部价格模型输入，交易引擎不会自行把成交价格当作可信标记价。计价为整数，所有风险仓位初始数量为 1。对手账户的仓位预先补足逐仓保证金以承受设定的极端冲击，手续费和强平费设置为 0，便于集中测强平、资产结算及保险金补差。

## 参数

| 参数 | 默认值 | 含义 |
| --- | ---: | --- |
| `--users` | 20000 | 风险用户数，不含对手/保险金账户 |
| `--positions-per-user` | 4 | 每用户独立仓位数 |
| `--symbols` | 8 | 永续产品数，共享抵押资产，全仓可跨产品传导 |
| `--waves` | 8 | 冲击轮数 |
| `--cross-percent` | 50 | 全仓用户百分比；其余逐仓 |
| `--shock-percent` | 35 | 最终外部价格变动百分比 |
| `--impact-percent` | 30 | 全部初始数量强平后额外冲击的百分比 |
| `--depth-percent` | 10 | 每轮各产品补充的深度占其初始数量的百分比 |
| `--levels` | 8 | 补充深度的价格档数 |
| `--slippage-bps` | 100 | 最远深度档相对标记价的滑点，100 = 1% |
| `--insurance-percent` | 10 | 初始保险金占产品初始名义价值的百分比 |
| `--resting-per-user` | 1 | 每用户第一个仓位的预挂平仓单数，触发强平时撤销 |
| `--direction` | down | down 急跌强平多仓；up 急涨强平空仓 |
| `--recover` | 1 | 最后补充资金/深度，要求待强平队列排空；0 保留压力结束状态 |
| `--book` | radix | radix 为新订单簿；tree 用原树索引作对照 |
| `--candidate-budget` | 不限 | 每次自动强平调度最多处理多少个候选；0 暂停自动执行，恢复阶段仍显式处理 |
| `--ring-size` | 32 | 消费队列容量，必须为正的 2 次幂 |

例子：

```powershell
# 80 万风险仓位，跨 64 个产品
.\build\release\advmatch_liquidation_benchmark.exe --users 200000 --positions-per-user 4 --symbols 64

# 急涨，纯全仓，额外冲击加深
.\build\release\advmatch_liquidation_benchmark.exe --direction up --cross-percent 100 --shock-percent 40 --impact-percent 40

# 深度枯竭与保险金为空，观察待强平堆积
.\build\release\advmatch_liquidation_benchmark.exe --depth-percent 0 --insurance-percent 0 --recover 0

# 同参数比较订单簿，顺序执行；重复运行取中位数
.\build\release\advmatch_liquidation_benchmark.exe --book radix
.\build\release\advmatch_liquidation_benchmark.exe --book tree

# 固定候选调度预算，观察积压和每轮耗时
.\build\release\advmatch_liquidation_benchmark.exe --candidate-budget 256
```

## 输出与口径

每轮输出标记价、外部/反馈冲击、实际强平数、补深度前后的积压和完整轮次耗时。汇总包括：

- `stress_liquidation_tps`：冲击阶段实际完成强平数 / 冲击阶段墙钟时间，包含价格重估、调度、撤单、真实成交、深度补充和命令提交。
- `total_liquidation_tps`：冲击与恢复阶段完成的强平数 / 两阶段总时间。不能和普通订单成交 TPS 直接比较。
- `mark_engine_ms`：标记价命令在消费线程的执行时间，包含它触发的强平。
- `maintenance_engine_ms`：补深度、补保险金、显式处理命令的消费线程时间。
- `wave_p50_ms` / `wave_max_ms`：完整冲击轮的耗时，不是单订单延迟分位数。
- `peak_pending` / `pending_final`：队列积压与最终剩余风险仓位。
- `insurance_used`：实际事件中消耗的保险金，不能为负。
- `peak_resident_mib`：Windows 进程峰值工作集；其他平台或无法读取时为 0，表示未提供测量。
- `validation=passed`：余额非负、多空数量守恒、现金守恒、已关闭风险仓位和实际强平事件数量一致；开启恢复时还要求最终无待强平记录。

初始化和最终审计不计入 TPS。工作区和历史对象的内存占用会随规模增加；请从默认规模逐步增大。

### 一次大规模实测

2026-10-07，本机 Windows / AMD Ryzen 7 7435HS，Release，20 万风险用户、每人 4 仓位、64 产品、8 轮冲击，候选预算不限；其余默认参数：

| 指标 | 测量值 |
| --- | ---: |
| 初始风险仓位 | 800000 |
| 压力阶段完成强平 | 502628 |
| 待强平峰值 | 560000 |
| 恢复前积压 | 297372 |
| 压力耗时 | 15.366 秒 |
| 恢复耗时 | 0.808 秒 |
| 两阶段完整强平吞吐 | 49462 仓位/秒 |
| 最慢冲击轮 | 4009.407 ms |
| 峰值工作集 | 2472.605 MiB |
| 最终积压 | 0 |

该次 `validation=passed`。这是单次压力模型的实测，不是稳定吞吐承诺。保险金不足使后两轮无法继续成交，但仍会做风险处理和重试，因此完整 TPS 与一般开仓成交 TPS 差异很大。可以使用固定 `--candidate-budget` 观察自动调度量与积压之间的取舍；一个全仓候选的实际撤单和成交数量仍不能用预算限定。

## 数据结构与复杂度边界

默认订单簿是 `RadixOrderBook`，与保留的 `IndexedTreeOrderBook` 共用 `PriceTimeOrderBook` 撮合算法。Radix 使用 uint64 价格的压缩二叉路径，查找最多 64 个分支，插入包含数次各自不超过 64 层的路径；空间与实际档位数有关，没有按价格最大值分配巨型数组。最优价和相邻档直接通过首尾指针与双向链表访问。档位及分支由对象池保存，地址在扩容后保持稳定。

`matching.reservePriceLevels(symbolId, bidCapacity, askCapacity)` 可以提前预留档位与分支；对象池增长和操作系统内存分配没有固定延迟保证。采用任意宽度价格时 Radix 为 O(w)，本项目固定 w=64，因此索引操作相对于档位数是 O(1)。

强平入队、已有任务去重、删除、出队、全仓组链接/脱链均为 O(1) 指针操作。队列使用本轮栅栏避免重新入队无限循环，不拷贝待处理队列；全仓组使用侵入式访问标记去重，无额外哈希集合。

整场强平仍是 O(仓位 + 撤单 + 成交)，标记价重估仍是 O(活动仓位)。候选预算限制队列工作量，但一个全仓候选可以涉及多个产品和大量挂单；它不构成单命令固定延迟保证。跨账户/产品的其他哈希查询仍为平均 O(1)。不会把这些操作称为全部最坏 O(1)。
