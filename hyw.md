有，而且目前的瓶颈比较明确。当前 400 万挂单规模下约 **15 万成交 TPS**，主要消耗已经不是价格撮合，而是命令对象、内存分配、账户查找和订单留存。

## 最大瓶颈

### 1. 每条命令都有多次堆分配

[SingleThreadConsumer.h](D:/CppWorks/AdvMatch/src/advmatch/consuming/SingleThreadConsumer.h:24) 每次 `post()` 都会产生：

- `std::make_shared<Request>`
- `std::promise/std::future` 的共享状态
- `shared_ptr` 原子引用计数
- `shared_mutex` 加锁
- future 唤醒和线程同步
- 一次虚函数调用

这基本抵消了 Disruptor 预分配 RingBuffer 的优势。现在 RingBuffer 里存放的只是一个指向堆对象的 `shared_ptr`。

**建议：**

- Command 直接写进 RingBuffer Event
- Event 中预留 response、状态和请求序号
- 返回轻量 `RequestTicket`，避免 `promise/future`
- 支持回调或批量拉取结果
- 正常提交路径去掉 `shared_mutex`
- 生命周期状态用原子变量管理

这是优先级最高的优化。

---

### 2. 每笔订单都独立分配，而且永远不删除

[MatchingEngine](D:/CppWorks/AdvMatch/src/advmatch/matching/MatchingEngine.h:19) 当前使用：

```cpp
std::unordered_map<OrderId, std::unique_ptr<OrderNode>> orders;
```

每笔订单至少涉及：

- `OrderNode` 单独堆分配
- `unordered_map` 节点分配
- 哈希查找
- 指针跳转
- terminal order 永久保留

实际压力测试中，300 万笔成交之后内存增长约 **1.2 GB**，即每 100 万成交增长约 **398 MB**。按这个速度无法 24/7 运行。

**建议：**

- 用固定块对象池分配 `OrderNode`
- 活跃订单和历史订单分开
- Filled、Cancelled、Rejected 订单写入异步 journal 后回收
- 订单查询历史交给外部存储
- `OrderId` 连续时，可以用分段数组代替 `unordered_map`

这一项同时改善吞吐、缓存命中和长期内存稳定性。

---

### 3. 1000 万余额记录使用 `std::map`

[User.h](D:/CppWorks/AdvMatch/src/advmatch/account/User.h:14) 中：

```cpp
std::map<AssetId, AssetBalance> balances;
```

`std::map` 每条余额都是独立红黑树节点。一次查询需要多次指针跳转，缓存局部性很差。成交结算会反复查询买卖双方的多个资产余额。

对于多数用户只持有少数资产的情况，建议：

```cpp
SmallVector<AssetBalanceEntry, 4>
```

少量资产直接顺序扫描通常比树更快。资产很多的用户再切换到 flat hash table。

可选方案：

- `boost::container::small_vector`
- `ankerl::unordered_dense`
- `absl::flat_hash_map`
- 自己实现内联 4 或 8 个余额槽

---

### 4. 风控和结算重复哈希查找

当前一笔成交会重复进行：

- 查订单
- 查用户
- 查 Symbol
- 查 Hold
- 查买方余额
- 查卖方余额
- 再次 reconcile maker 和 taker

`SpotRiskEngine` 还用独立的：

```cpp
std::unordered_map<OrderId, Hold> holds;
```

预占信息本来就和订单一一对应。

**建议：**

直接将预占数据放进 `OrderNode`：

```cpp
AssetId holdAssetId;
Balance heldAmount;
User* user;
const Symbol* symbol;
```

创建订单时缓存这些指针。单线程撮合期间对象地址稳定，无需每次重新哈希查找。

结算接口也可以直接接收 maker 和 taker 节点：

```cpp
settleTrade(OrderNode& maker, OrderNode& taker, Price price, Quantity quantity);
```

避免先创建 `Trade`，再通过 OrderId 反查两个订单。

---

### 5. 每条命令都会创建 `std::vector<Trade>`

压测消费者中，每次命令都会：

```cpp
std::vector<Trade> trades;
```

一次普通成交通常只有一条 Trade，但 vector 仍可能进行堆分配。

建议：

- 使用固定容量的 `SmallVector<Trade, 4>`
- 由调用方复用 Trade 缓冲区
- 撮合线程直接发布 TradeEvent
- 如果调用方不需要成交明细，提供无结果收集路径

例如：

```cpp
TradeBuffer<8> trades;
engine.placeOrder(command, trades);
```

---

## OrderBook 的问题

当前价位同时存在：

```cpp
std::map<Price, PriceLevel>
std::unordered_map<Price, PriceLevel*>
```

优点是 best price 和指定价格查找都方便，但代价是：

- 每个价位在两个容器中都有记录
- 多次堆分配
- 红黑树指针跳转
- 哈希表额外内存
- 创建、删除价位需要同步两个容器

如果价格有固定 tick，可以将价格转换为 tick index：

```cpp
priceIndex = price / tickSize;
```

然后选择：

1. 价格范围较小：数组价格梯子。
2. 价格范围较大：分段数组或 radix tree。
3. 活跃价位稀疏：flat hash map 加 best bid/ask 缓存。
4. 暂时保留树结构：换成 cache friendly B-tree。

订单 FIFO 链表设计本身可以保留。

---

## 单线程架构上限

一个 `MatchingEngine` 当前负责全部 10 万个 Symbol。无论 producers 有多少，所有订单最终都由一个线程处理。

生产者增加只会增加提交竞争，不能提高撮合上限。之前 1 producer 比 4 producers 更快也说明了这一点。

真正扩展的方法是按 Symbol 分片：

```text
Gateway threads
      │
      ├── shard 0 consumer: BTC/USDT ...
      ├── shard 1 consumer: ETH/USDT ...
      ├── shard 2 consumer: ...
      └── shard N consumer
```

同一个 Symbol 始终只属于一个撮合线程，因此仍然保持：

- 单线程订单簿
- 无锁撮合
- 确定性顺序
- 无需在 OrderBook 内加锁

建议先按 CPU 物理核心部署 **4–8 个撮合分片**。热点交易对可以独占线程，低流量交易对按哈希分配。

账户风险是跨分片共享状态，需要选择：

- 每个账户固定归属一个风险分片；
- 先在风险线程预占，再把已授权命令发送到撮合分片；
- 或按交易对维护专用资产额度。

不能让多个撮合线程直接无锁修改同一个 `User`。

---

## 当前压测延迟的含义

当前 p50 约 3.2 ms、p99 约 4.2 ms，测量的是：

```text
生产线程提交
→ 在批次中排队
→ RingBuffer 排队
→ 撮合
→ 风控结算
→ future 完成
```

压测使用 `batchSize=256`，所以这个数字包含明显的排队时间，并不等同于撮合函数自身耗时。

建议拆分指标：

- publish latency
- queue waiting latency
- matching service time
- settlement time
- end to end latency
- RingBuffer backlog
- 每次成交产生的 fills 数量

并使用 HDR Histogram，避免为了算百分位保存并排序全部延迟数据。

## 推荐实施顺序

1. **移除每条命令的 `shared_ptr + promise + future`。**
2. **订单对象池和 terminal order 回收。**
3. **把 Hold、User、Symbol 指针放进 OrderNode。**
4. **Trade 使用固定容量缓冲区。**
5. **User 余额从 `std::map` 改成内联小数组或 flat hash。**
6. **连续 ID 容器改成数组或分段数组。**
7. **价格档位改为价格梯子、radix tree 或 B-tree。**
8. **按 Symbol 将 MatchingEngine 分成多个单线程 shard。**
9. **补充 queue wait、service time 和端到端延迟指标。**

其中前四项不用改变撮合语义，风险相对较低，也最适合先实施。当前最先应该处理的是 **Consumer 每命令分配**和**已结束订单不回收**，这两个问题同时限制了 TPS、延迟和 24/7 稳定性。