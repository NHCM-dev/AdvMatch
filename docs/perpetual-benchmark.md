# 永续合约 benchmark

## 构建与运行

在项目根目录执行：

```powershell
cmake --preset release
cmake --build --preset release --target advmatch_perpetual_benchmark
.\build\release\advmatch_perpetual_benchmark.exe
```

默认：10 万笔计时买单、10 万笔预加载卖单、1024 个用户、1 个永续交易对、4 个生产者、32 槽 RingBuffer、10 倍逐仓。先用独立引擎预热 1000 笔，预热数据不会进入正式测试的订单、仓位或统计。

调整测试规模与队列：

```powershell
.\build\release\advmatch_perpetual_benchmark.exe --orders 1000000 --users 10000 --symbols 100 --producers 4 --ring-size 4096 --batch-size 256 --publish-batch 32
```

比较全仓与逐仓时，保持其他参数一致：

```powershell
.\build\release\advmatch_perpetual_benchmark.exe --orders 100000 --margin-mode isolated --leverage 10
.\build\release\advmatch_perpetual_benchmark.exe --orders 100000 --margin-mode cross --leverage 10
```

用 `--help` 查看全部参数。用户数必须是至少 2 的偶数，生产者和交易对数不能超过计时订单数。`--warmup 0` 关闭预热，`--publish-batch 0` 自动选择发布批量；RingBuffer 大小必须是 2 的幂。

## 实际执行的流程

1. 创建真实 ContractAccount、入金，登记永续合约。
2. 经 SingleThreadConsumer 的 ResponseSlot / RequestTicket 批量接口提交卖单，全部预加载入簿。
3. 多个生产者同时提交 IOC 限价买单，由单一消费线程执行风控、保证金预留、撮合、仓位及余额更新、手续费结算和成交事件回调。
4. 经同一消费线程提交一次所有产品的标记价刷新，再提交一次所有产品的资金费率结算，各阶段单独计时。
5. 停止消费线程，核对订单簿、持仓、逐仓预留或全仓风险缓存、每个账户的余额与费用/资金费、事件及总资金守恒。

每笔卖单和买单各创建一个独立仓位；`orders=N` 最后产生 `2N` 个持仓和 `N` 笔成交。买卖用户分开，按照固定全局序号分配用户和产品，不会自成交。大量 users/symbols 会稀释每个账户、每个产品的负载；当前分配采用取模，某些用户数和产品数的组合会形成固定关联。

价格为 100000 原子单位，数量为 1、合约乘数为 1。maker 默认费率 100，taker 默认费率 500，费率分母 1000000，因此每笔成交分别扣 10 和 50 原子单位。可通过 `--maker-fee-rate`、`--taker-fee-rate` 改成 0 或其他费率。

所有交易命令使用相同的模拟时间，避免多生产者发布顺序不同造成时间倒退。标记价不变，资金费率为 +1000，即 0.1%，多仓支付 100、空仓获得 100。模拟时间与吞吐计时使用的 steady_clock 分开。

## 输出含义

| 字段 | 含义 |
| --- | --- |
| setup_seconds | 创建账户、产品、预分配测试及引擎存储、启动消费线程 |
| preload_seconds | 卖单提交、处理和结果回收耗时 |
| elapsed_seconds | 从放行生产者到全部买单响应回收、生产者退出的耗时 |
| command_tps | 计时阶段成功买单数 / elapsed_seconds |
| trade_tps | 计时阶段实际成交数 / elapsed_seconds；本场景每个买单成交一次 |
| match_latency_p50/p95/p99/max_us | 采样命令从构造时刻到消费线程完成撮合与结算的延迟 |
| mark_update_seconds | 刷新全部产品标记价的耗时，包含提交和响应等待 |
| funding_seconds | 全部产品本期资金费结算耗时，包含提交和响应等待 |
| funding_positions_per_second | 本期处理仓位数 / funding_seconds |
| validation=passed | 成交数量、仓位与资金验证全部通过 |

撮合延迟包含批次构造、发布等待、队列等待和实际执行，不包含消费线程发布响应后的生产者唤醒与结果回收。默认每个 1024 笔的区间选一个确定性的分散采样点，避免只采到批次首条命令或同一用户。`--latency-sample 1` 测所有买单，`--latency-sample 0` 关闭采样。采样、回调和响应统计都有成本；稀疏或小规模样本的 p99 参考价值有限。

吞吐计时不包含预热、账户创建、卖单预加载、标记价更新、资金费率结算、验证及最终对象销毁。引擎保留已完成订单，且本场景持仓一直增加，因此包含新订单与新仓位创建成本，内存随 N 增长。大规模参数需要对应的可用内存。

这是永续开仓与资金费率处理的可复现基线。没有混入平仓、改单撤单、价格波动、强平、网络、磁盘日志或行情发布，不能作为完整生产环境的容量结论。标记价刷新保持原价，因此测的是风险缓存刷新与扫描开销，不会产生强平。

## 回归验证

```powershell
cmake --build --preset release
ctest --preset release
```

CMake 已登记逐仓和全仓的 smoke 场景；全仓使用不能整除的订单数、生产者数和多个产品，检查分配余数及并发时序。
