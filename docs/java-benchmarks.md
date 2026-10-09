# Java benchmark

六类 benchmark 全部在 `java/advmatch/src/main/nhcm/advmatch/benchmark`。每个都能作为普通 Java main 运行，Gradle 也提供独立任务；`benchmarkSmoke` 会运行所有六类的小规模验证，包括全仓、逐仓、两种订单簿、上涨/下跌强平、恢复及流动性不足。`build/check` 包含这些烟雾测试和跨语言集成测试。

## 构建与运行

项目根目录运行，首次会自动构建 Release JNI 库：

```powershell
.\java\gradlew.bat -p java :advmatch:build :advmatch:runExample
.\java\gradlew.bat -p java :advmatch:benchmarkSmoke
```

性能测量建议使用已构建的 JAR，排除构建过程并明确 JVM 内存配置：

```powershell
java -Xms1g -Xmx4g -cp java/advmatch/build/libs/advmatch-0.0.1.jar nhcm.advmatch.benchmark.TpsBenchmark --orders 1000000 --producers 4 --batch-size 256 --ring-size 32
```

Gradle 参数通过 `-PbenchmarkArgs` 传入，JVM 参数通过 `-PbenchmarkJvmArgs` 传入：

```powershell
.\java\gradlew.bat -p java :advmatch:benchmarkTps -PskipNativeBuild "-PbenchmarkArgs=--orders 100000 --producers 4 --batch-size 256 --ring-size 32" "-PbenchmarkJvmArgs=-Xms1g -Xmx4g"
```

## 与 C++ benchmark 对应

| C++ 源文件 | Java main | Gradle 任务 | 验证 |
|---|---|---|---|
| benchmarks/tps.cpp | TpsBenchmark | benchmarkTps | 预挂卖单、并发买单、成交与最终余额 |
| benchmarks/scale.cpp | ScaleBenchmark | benchmarkScale | 用户/余额记录/交易对/挂单规模、多轮买单与补单 |
| benchmarks/perpetual.cpp | PerpetualBenchmark | benchmarkPerpetual | 逐仓/全仓、杠杆、手续费、标记价、资金费、资金与持仓守恒 |
| benchmarks/liquidation.cpp | LiquidationBenchmark | benchmarkLiquidation | 多轮冲击、价格反馈、分层流动性、保险基金、候选预算、恢复与资金/持仓守恒 |
| benchmarks/observe.cpp | ObserveBenchmark | benchmarkObserve | 索引开关、订单读取、20档盘口、缓存槽刷新和读取 |
| benchmarks/big_allocs.cpp | BigAllocsBenchmark | benchmarkBigAllocs | 同一原生 BigAllocs 与 new/delete 工作负载、校验和 |

所有 main 的包前缀都是 `nhcm.advmatch.benchmark.`。交易类测试关闭 WAL、交易历史环和合约事件历史环；结果校验通过读取实时不可变 DTO 完成。输入或业务结果不符会以非零退出码结束。

## 参数

### 现货 TPS

与 C++ 一样，默认 `--orders 1000000 --producers 4 --batch-size 256 --ring-size 32`。先预挂对应数量的卖单，然后多个 producer 提交同价 IOC 买单。只有经过 Filled/数量/余额验证的成交进入结果；准备与预挂时间单独报告。

Java 的现货/永续 benchmark 使用真实 `Exchange.submitBatch`。`--publish-batch` 默认 min(batch-size,256)，允许 1..min(batch-size,1024)，每批一次 JNI 调用、一个原生队列任务，批内按提交顺序执行；`--batch-size` 是每个 producer 收集/等待结果的逻辑批次。原生 C++ postBatch 则一次 claim 多个 ring entry，两者不能直接比较 ring-size 的数值。可使用 `--publish-batch`（1..1024，且不超过 batch-size） 测量不聚合提交的情况。

结果通过原生完成队列批量 drain，并在原生条件变量上等待新结果；Java 不再逐个扫描 ticket，也不依赖 Windows 上短时间 park 的计时精度。每个完成批次共用一个回调任务，SDK 命令从不可变批次缓冲区直接解码。自定义 Command 未实现切片解码时自动复制，保持兼容。

交易 benchmark 共用 BackpressureSubmitter：单条提交占一个许可，批量提交整批占一个许可；批内单独保留 Future。原生 pending 上限与 Java outstanding 上限另外限制请求和待解码响应数量。只有错误码 -3 的未执行命令会重试，已成功的同批命令不会重复执行。30秒无进展失败；业务拒绝、取消、关闭、编码错误不会重试。输出 logical_submissions、queue_full_retries、producer_admission_wait_ms（各 producer 等待时间累加）。普通 SDK 的 submit/submitBatch 保留满队列失败的语义。

完整基础设施说明和重复测量工具见 [performance.md](performance.md)。

### 大规模状态

```powershell
.\java\gradlew.bat -p java :advmatch:benchmarkScale -PskipNativeBuild "-PbenchmarkArgs=--profile target --producers 4 --batch-size 256 --ring-size 1024" "-PbenchmarkJvmArgs=-Xms1g -Xmx4g"
```

默认小规模：`--users 3000 --accounts 10000 --symbols 100 --pending-orders 4000 --cycles 1000 --rounds 2 --producers 4 --batch-size 64 --ring-size 1024`。

`--profile target` 使用 3M users、10M asset balance records、100K books、4M pending orders、每轮100K cycles、3 rounds。与原 C++ scale.cpp 一样，`--accounts` 的历史含义是**资产余额记录数量**，不是 Account 实体数量；每位 User 自动创建 SpotAccount 和 ContractAccount，另外报告 actual_account_entities。Java 使用真实账户 ID，并按实际 BalanceCount 补齐所需余额记录，测试结束再次校验。

可以覆盖以上参数；`--force` 绕过状态内存估算与空闲物理内存检查。JVM 的 -Xmx 只限制 Java heap，不限制原生订单/账户/订单簿内存。完整 target 会占用大量原生内存和准备时间，本轮自动测试运行的是小规模场景。

### 永续

```powershell
.\java\gradlew.bat -p java :advmatch:benchmarkPerpetual -PskipNativeBuild "-PbenchmarkArgs=--orders 100000 --warmup 1000 --users 1024 --symbols 8 --producers 4 --ring-size 32 --batch-size 256 --margin-mode cross --leverage 10 --latency-sample 1024"
```

支持 `--orders`、`--warmup`（0关闭）、`--users`（必须偶数）、`--symbols`、`--producers`、`--batch-size`、`--ring-size`、`--publish-batch`（1..1024，且不超过 batch-size）、`--margin-mode isolated|cross`、`--leverage`、`--maker-fee-rate`、`--taker-fee-rate`、`--latency-sample`（0关闭）。默认与 C++ 相同：100K订单、1024用户、1交易对、4 producers、杠杆10、费率100/500，费率单位1,000,000。

每个 maker/taker 订单创建独立 Position；资金费处理数量应为2倍订单数。准备、预挂、撮合、标记价和资金费结算耗时分别报告。延迟采样测到 Java Future 完成，包含编码、提交和响应，使用周期采样。

### 连锁强平

```powershell
.\java\gradlew.bat -p java :advmatch:benchmarkLiquidation -PskipNativeBuild "-PbenchmarkArgs=--users 20000 --positions-per-user 4 --symbols 8 --waves 8 --cross-percent 50 --shock-percent 35 --impact-percent 30 --depth-percent 10 --levels 8 --slippage-bps 100 --insurance-percent 10 --resting-per-user 1 --ring-size 32 --direction down --recover 1 --book radix"
```

支持以上全部参数，另有 `--candidate-budget N`（0表示关闭自动候选处理；默认足够大的预算）、`--direction up|down`、`--book radix|tree`、`--recover 0|1`。例如 `--depth-percent 0 --recover 0 --candidate-budget 2` 模拟流动性枯竭与候选处理限额。

复现 C++ 合成场景：杠杆5/10/20/40/80混合，逐仓/全仓混合，挂 reduce-only 平仓单；外部冲击加已强平数量反馈推动标记价，fixture 提供带滑点的多档流动性。恢复阶段注资保险基金并补足流动性。输出每波价格、关闭仓位数量、待强平队列、总耗时和强平 TPS。

mark_api_ms/maintenance_api_ms 是 Java 调用往返时间，包含跨语言和回复调度；不等于 C++ 的纯 engine_ms。波次耗时包含监控读取；审计另计。总关闭仓位从市场和 fixture 仓位状态计算，再全量校验，未开启事件历史环。

`--trades-per-wave N`（默认0）在同一个引擎每波额外执行 N 笔普通永续成交，使用额外控制交易对及两名全额保证金用户。每笔买卖单分别创建仓位，使用 submitBatch 提交；最多10M普通成交。`ordinary_trade_tps_under_stress` 的时间包括整个 stress 阶段的标记价、强平、读取和普通成交；这是共享单写线程的混合压力测试，控制交易对不与受害仓位争抢流动性。最终审计也覆盖控制交易对的资产和仓位。

### 读取

支持 `--orders 50000 --queries 1000000 --ring-size 1024`，也兼容 C++ 的两个位置参数 `50000 1000000`。分别运行 index=off/on，输出写入、订单查询、20档盘口查询、native slot load 的速率，以及刷新时间/校验和。

缓存 load 不向撮合线程提交查询，但仍经过 JNI 复制和 DTO 解码。`native_slot_loads_s` 不能等同于 C++ shared_ptr 快照 load 速率；已有 Java DTO 可直接保留读取，但这里测量的是原生缓存接入路径。

### 对象池

支持 `--objects 1000000 --warmup 10000 --rounds 3`。通过 RegisterNatives 调用与 C++ executable 共用的 AllocatorBenchmark.h，输出原生 pool/new-delete 耗时、ops/s、速度比和 Java 总墙钟时间。它测量 C++ 对象池，没有用 Java GC 对象池冒充 BigAllocs，也没有启动外部可执行程序。

## 如何比较结果

交易类 Java 结果标识 `measurement=java_api_end_to_end`，包括命令对象、编码、JNI、原生队列/业务操作、响应轮询、解码及 Future 完成。原 C++ TPS/perpetual/scale 使用轻量 ResponseSlot 快路径，observe 直接调用 observer，因此两者代表不同的调用路径。

对比时使用同一机器、Release原生库、相同规模/方向/保证金模式/费率，重复运行并关注稳定区间；保留完整输出。观察 java_heap 与 GC 指标，它们只覆盖 JVM 内存，原生 state 另外消耗内存。小规模 smoke 的 TPS 仅用于确认程序正确，不能代表满负载稳定性能。
