# AdvMatch 当前性能分析与优化路线

本文按当前代码重新整理。早期版本中 `shared_ptr + promise/future`、`std::map` 余额、订单哈希表等结论已经不再适用，不能继续作为当前瓶颈依据。

## 当前状态

目前的主路径是：多生产者提交命令，`SingleThreadConsumer` 顺序消费，`MatchingEngine` 在一个线程中完成风控、订单簿修改和结算。这个模型能保证同一引擎内的命令顺序，并避免在订单簿内部加锁。

最近一次本机结果可作为调参基线：

- 100 万条命令、4 个 producer、RingBuffer 32、publish batch 8：约 **222 万 TPS**。
- 同类配置的多次运行约 **217–229 万 TPS**，短压测会受线程调度、CPU 频率和后台进程影响。
- 中等规模数据集（10 万用户、30 万余额、1000 个 Symbol、10 万挂单）：命令吞吐约 **166 万 TPS**，p99 约 **81 微秒**，工作集约 **54 MiB**。

这些数字不能直接代表 300 万用户、1000 万账户、10 万订单簿和 400 万挂单的完整目标。数据结构调整后还需要在内存充足、CPU 固定频率的机器上重新跑完整规模测试。旧文档中的“400 万挂单约 15 万 TPS”来自早期实现，不再能说明当前性能。

## 已完成的关键优化

### 1. 命令直接进入 RingBuffer

[SingleThreadConsumer.h](src/advmatch/consuming/SingleThreadConsumer.h) 的正常路径已经使用：

- RingBuffer Event 内的 Command；
- 调用方持有的 `ResponseSlot`；
- 轻量 `RequestTicket`；
- `postBatch()` 批量占用和发布连续序号。

因此，正常提交路径已经没有每条命令的 `make_shared<Request>`、`promise/future` 共享状态和 `shared_mutex`。RingBuffer 的预分配现在能够覆盖命令事件本身。

文件中仍保留了早期 Future 请求所需的 `Request`、`EventType::Future` 和对应消费分支，但当前公开提交路径没有使用它们。这些代码不会造成每命令分配，不过会增加 Event 大小和维护成本，后续可以单独清理。

### 2. 连续 ID 使用直接索引

[MatchingEngine.h](src/advmatch/matching/MatchingEngine.h) 使用：

```cpp
std::vector<std::unique_ptr<OrderNode>> orders;
std::vector<std::unique_ptr<OrderBook>> orderBooks;
```

连续 ID 通过 `id - 1` 直接定位，查询是 O(1)，同时 `unique_ptr` 指向的对象不会因为 vector 扩容而移动。这里已经不再承担 `unordered_map` 节点分配和哈希查找成本。

### 3. 用户余额使用小对象优化

[User.h](src/advmatch/account/User.h) 当前在用户对象内保存前 **6** 条余额：

```cpp
SmallVector<AssetBalanceEntry, InlineBalanceCapacity>
```

超过内联容量后再迁移到 `DenseMap`。大多数持有少量资产的用户不需要额外哈希表和独立节点，遍历时也有更好的缓存局部性。

### 4. Manager 已按地址稳定性选择容器

[Containers.h](src/advmatch/util/Containers.h) 统一提供 `SmallVector`、`DenseMap`、`DenseSet`、`FlatHashMap` 和 `NodeHashMap`。

- `UserManager`、`AssetManager`、`SymbolManager` 的主对象表使用 `NodeHashMap`，rehash 后对象地址保持稳定。
- 名称、交易对等只保存 ID 的二级索引使用 `DenseMap` 或 `DenseSet`，获得更好的局部性。
- 对外保留的指针只能指向地址稳定的主对象，不能指向 dense 容器里的元素。

### 5. 订单簿采用排序结构和精确索引的组合

[SpotOrderBook.h](src/advmatch/matching/orderbooks/SpotOrderBook.h) 的每一侧同时维护：

```cpp
std::map<Price, PriceLevel, Compare> ordered;
DenseMap<Price, PriceLevel*> exact;
```

`ordered` 负责最优价格、价格优先级和稳定的 `PriceLevel` 地址；`exact` 负责指定价格 O(1) 平均查找。订单本身在价位内使用侵入式双向链表，撤单和移动已知订单是 O(1)。

这套结构确实为每个价位维护了两份索引，但它解决的是不同问题。没有真实价格分布和 profile 证据前，不应仅为了减少一个容器而破坏最优价查询或地址稳定性。

### 6. 风控 Hold 保留 `std::unordered_map`

[SpotRiskEngine.h](src/advmatch/matching/riskengines/SpotRiskEngine.h) 仍使用：

```cpp
std::unordered_map<OrderId, Hold> holds;
```

这不是遗漏。A/B 测试中，`DenseMap` 约为 156–166 万 TPS，而当前 `std::unordered_map` 约为 199–215 万 TPS。这个负载包含大量删除，dense 容器删除时移动较大的 `Hold` 数据，反而更慢。容器选择应服从当前工作负载，而不是统一替换。

## 当前主要瓶颈

### 1. 结束订单没有回收

虽然订单查询已经改为数组直接索引，但每个成功接纳的 `OrderNode` 仍单独分配，且 Filled、Cancelled 等终态订单继续留在 `orders` 中。长期运行时，吞吐问题最终会转化成内存增长和缓存污染问题。

建议将订单拆成两个生命周期：

1. 活跃订单保留完整 `OrderNode`，由订单簿和风控引用。
2. 终态订单写入异步 journal 或历史服务，确认持久化后回收节点。

为了继续支持按 ID 查询近期结果，可以保留固定大小的终态窗口或分段归档索引，而不是无限保存所有完整节点。

节点分配建议改成固定块对象池或 slab。这样能够批量申请内存，减少 allocator 元数据、碎片和随机地址访问。

### 2. 每条命令仍创建 `std::vector<Trade>`

订单簿和引擎接口目前都传递 `std::vector<Trade>&`。压测消费者为每条命令创建一个 vector，发生一笔或多笔成交时可能触发堆分配。

可以按下面的顺序优化：

1. 将缓冲区改成 `SmallVector<Trade, 4>` 或专用 `TradeBuffer`。
2. 在消费线程中复用缓冲区，每条命令只调用 `clear()`。
3. 给不需要成交明细的调用方提供直接发布 `TradeEvent` 的路径。

普通订单通常只产生少量成交，内联容量 4 是较合理的起点；最终容量应根据“一条 taker 命令产生的成交笔数”分布决定。

### 3. 风控与结算仍有重复定位

一笔成交需要定位订单、用户、Symbol、Hold 和余额。当前容器已经加快了各次查询，但查询次数仍然存在。

可以将与订单生命周期一致的预占信息放入 `OrderNode`，或让 Hold 保存稳定的 `User*`、资产 ID 和剩余冻结量。结算接口直接接收 maker/taker 节点，可以减少先生成 Trade、再按 OrderId 反查订单的过程。

缓存指针前必须固定生命周期规则：

- `UserManager` 和 `SymbolManager` 中的对象在引擎运行期间不能删除或移动；
- `OrderNode` 只有在订单簿、风控 Hold 和结果发布都不再引用后才能回收；
- 跨线程只能传递有明确所有权或代际校验的句柄。

### 4. 多 producer 竞争同一个发布游标

一个 MPMC RingBuffer 并不会随着 producer 数量增加而线性变快。所有 producer 都会竞争同一个 sequencer；RingBuffer 满时还会等待唯一的消费者推进 gating sequence。

当 producer 数接近或超过逻辑 CPU 数时，会出现：

- 消费线程得不到足够运行时间；
- 多个 producer 争用同一缓存行；
- 某个 producer 占到序号后被抢占，后续已发布序号也要等待这个缺口；
- 小 RingBuffer 很快填满，等待和唤醒占据主要时间。

RingBuffer 大小主要吸收突发流量，不能提高单消费者的长期处理上限。producer 过多时继续增大 ring 只能缓解部分抖动。

### 5. 单个 MatchingEngine 的吞吐上限是一个核心

当前所有 Symbol 最终由一个消费线程处理。增加 gateway 或 producer 线程只能提升输入并发，不能提升撮合线程的服务率。

需要更高总吞吐时，应按 Symbol 分片：

```text
Gateway / protocol threads
          |
          +-- matching shard 0: hot symbol A
          +-- matching shard 1: hot symbol B
          +-- matching shard 2: normal symbols by hash
          +-- matching shard N: normal symbols by hash
```

一个 Symbol 始终只属于一个 shard，单个订单簿仍然无锁且顺序确定。热点交易对可以独占线程，低流量交易对按哈希或负载表分配。

账户余额会跨 Symbol，因此不能让多个 shard 直接修改同一个 `User`。可选方案是先由账户风险分片完成额度预占，再把已经授权的命令交给撮合分片；成交后用有序事件回到账户分片结算。

## RingBuffer 与 producer 参数建议

`--batch-size` 和 `--publish-batch` 含义不同：

- `batch-size` 控制 producer 同时保留多少未完成响应，会影响端到端排队延迟。
- `publish-batch` 控制一次占用并发布多少个连续 RingBuffer 槽位，会减少 sequencer 竞争，但一个 producer 占用过多时会影响公平性。

以当前 8 核 16 线程机器为起点：

| Producer | RingBuffer | Publish batch | 用途 |
| ---: | ---: | ---: | --- |
| 1–4 | 32–64 | 1–8 | 追求最高稳定吞吐和低延迟 |
| 4–8 | 256–512 | 8–32 | 输入存在突发流量 |
| 8–16 | 1024 | 16–64 | 必须接收更多并发来源时的折中 |
| 32+ | 不建议单 MPMC | 不适用 | 改成入口聚合、每入口 SPSC 或撮合分片 |

目前 4 producer、ring 32 的结果最好。16 producer 在 ring 32 下会严重退化，ring 1024 可以恢复到约 150–160 万 TPS；32 producer 即使扩大 ring 也明显低于 4 producer。生产环境中的网络连接数不应直接等于向撮合队列发布的线程数，I/O 线程应先聚合命令，再由少量固定线程发布。

如果采用每入口 SPSC 队列，消费线程需要确定性的轮询或按全局 sequence 合并，不能因为队列切换改变业务要求的先后顺序。

## 延迟指标应怎样解释

当前端到端延迟包含：

```text
producer 准备与提交
-> 等待 RingBuffer 空位
-> 队列等待
-> 风控与撮合
-> 结算
-> ResponseSlot 可见
```

它不等于订单簿函数本身的耗时。`batch-size` 较大时，producer 主动保留更多在途请求，p50 和 p99 会包含更多排队时间。

建议分别记录：

- publish 等待时间；
- queue wait；
- matching service time；
- risk/settlement time；
- end-to-end latency；
- RingBuffer 最大和平均 backlog；
- 每条命令的成交数量；
- 对象池使用量和终态订单回收速率。

百分位统计建议使用 HDR Histogram，避免为了计算分位数保存和排序全部样本。

## 下一轮实施顺序

1. **TradeBuffer 内联与复用。** 改动局部，能直接移除热路径中的小额分配。
2. **OrderNode 对象池。** 先替换单节点分配，再测分配耗时和内存占用。
3. **终态订单归档与回收。** 建立清晰的持久化确认、近期查询窗口和安全回收规则，解决 24/7 内存增长。
4. **合并订单与 Hold 的热数据。** 减少成交结算中的哈希查找，同时明确指针生命周期。
5. **补齐分段性能指标。** 先确认时间花在排队、撮合、风控还是回收，再决定下一项。
6. **入口聚合或 SPSC lanes。** producer 数量确实较多时再实施，避免单 MPMC sequencer 竞争。
7. **按 Symbol 分片 MatchingEngine。** 单核服务率到顶后，用多个独立单线程 shard 扩展总吞吐。
8. **评估价格梯子、radix tree 或 B-tree。** 只有 profile 显示价位树占比足够高，并且已掌握 tick size、价格范围和稀疏度后再替换。
9. **清理未使用的 Future 消费路径。** 缩小 Event 并简化状态机，随后重新测量 ring 容量和缓存行为。

当前最值得先做的是 **Trade 缓冲区复用、OrderNode 对象池、终态订单回收**。前两项改善热路径，第三项决定系统能否长期稳定运行。RingBuffer 调参可以改善特定并发组合，但不能替代对象生命周期管理或多撮合分片。
