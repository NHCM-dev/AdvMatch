# 交易数据读取（observe）

交易所对外入口现在使用 `Exchange` + `api/command/read/ReadCommands.h`，所有查询返回 CompletableFuture，见 `docs/exchange-api.md`。下文仍介绍所属线程读取器本身及其低层适配方式。canonical 目录为 abstract / model / impl / cache / consuming，原根目录头文件保留转发兼容。

## 入口和线程模型

包含 `advmatch/observe/TradingObserver.h`，链接 `AdvMatch::core`，使用 `advmatch::observe` 命名空间。

`TradingObserver` 在撮合所属线程的命令边界读取状态，返回独立的值对象。结果没有 User、OrderNode、Position、价格档等内部指针，持有结果不会阻止订单归档，也不会因容器 rehash 或对象池复用变成悬空引用。

读取分为三种方式：

1. 所属线程通过 `order(id)`、`position(id)` 等接口直接读取；数字 DTO 不分配堆内存。
2. 外部线程将 `ObservationRequest` 提交给 `ObservedConsumer`，与交易命令串行执行，取得一致的不可变响应。
3. 外部线程持有 `ObservationSlot`，通过 `load()` 读取已发布快照。刷新由开发者按请求、时间间隔或业务变化提交给消费线程；读线程不接触撮合状态。

**不能保证读取完全没有性能成本。** 查询与交易共用 CPU 时，复制数据和刷新缓存会占用消费线程时间。因此限制每页返回量、扫描量和批量查询工作量，复用缓冲区，热门数据发布缓存。没有每笔成交自动复制全量订单簿/账户，也没有为读取给撮合容器加互斥锁。`atomic<shared_ptr>` 的内部实现是否使用锁取决于标准库，不能称为保证无锁。

运行中的所有 `TradingObserver` 方法和索引重建均需在所属线程且不与交易并发。可在消费线程 start 前构造，在线程完全 stop 后销毁。所有 Manager 和 MatchingEngine 必须比 observer 活得更久。只有 cache 的创建/加载/发布可以从其他线程执行；`refresh(observer)` 仍然必须在所属线程。数值点读接口不额外检查来源的 fault 状态，使用它们之前应检查 `stamp().sourceFaulted`；通用 `read`、`readDepth`、`readBatch` 会返回 `SourceFaulted`。

## 可读取数据

列表默认不包含停用用户、账户和交易对，`includeInactive=true` 可读取仍保留的记录。点读快捷方法返回保留记录及其 `active` 字段；通用查询会应用支持的过滤条件。归档后已从内存移除的历史数据要到历史存储读取。

| ReadKind | 内容 / 主要选择条件 |
|---|---|
| Overview | 活跃/保留数量、ID 上界、订单槽数、市场数、自动强平预算、近期事件范围 |
| Asset / Assets | 资产 ID、名称、显示名、精度；assetId |
| User / Users | 用户状态、现货/合约 AccountId；userId |
| Account / Accounts | 类型、归属用户、余额/仓位/订单计数；accountId / userId |
| Balance / Balances | 可用 active、预留 reserved、total；accountId / userId / assetId |
| CrossMargin / CrossMargins | 按结算资产的全仓初始/维持保证金、PnL、订单预留、强平状态；**需 accountId**，可选 assetId / userId |
| Symbol / Symbols | 交易对、产品类型、基础/报价资产、状态；symbolId |
| Book / Books | 最优买卖价及数量、买卖价格档数；symbolId |
| Level | 精确价格档；**需 symbolId、side、price** |
| Depth | 盘口价格档、累计数量、订单数；**需 symbolId、side**，从最优价格开始 |
| BookOrders | 按价格优先、同价 FIFO 的挂单；**需 symbolId、side**；可选 userId / accountId / price |
| Order / Orders | 完整订单、时间戳、预留资金、账户、仓位、杠杆、reduceOnly、强平订单标记；orderId（点读）/ userId / accountId / symbolId / price |
| UserOpenOrders | 用户正在盘口上的订单；**需 userId 且启用索引**；可选 accountId / symbolId / price |
| Position | positionId；引擎管理仓位可全局定位，手工录入的仓位还需 accountId |
| Positions / AccountPositions | 全部保留仓位，包含手工录入；accountId / userId / symbolId / positionFilter |
| ManagedPositions | 引擎管理的仓位；同上，可用活跃索引避免扫描账户历史 |
| MarketPositions | 指定合约市场的引擎管理仓位；**需 symbolId**；userId / accountId / positionFilter |
| Market / Markets | 合约规格、标记价及时间、到期/交割状态、资金费时间、保险基金、清算/手续费余额、仓位/强平数量 |
| PendingLiquidations | 指定市场的待强平仓位，含是否排队；**需 symbolId**；userId / accountId / positionFilter |
| Trades | 有界近期成交记录、命令序号、买卖方用户/账户；symbolId / userId / accountId / orderId |
| ContractEvents | 有界 Fill / Funding / Liquidation / Delivery 事件；symbolId / userId / accountId / positionId / orderId |

点读种类需提供相应 ID。列表的零 ID 表示不按该字段过滤。只应用表中列出的过滤条件，其余字段不作为权限约束；API 认证、用户数据访问权限需由接入层校验。普通 Orders 默认包含终态订单，设置 `includeTerminal=false` 排除。`PositionFilter::All` 为保留记录，`Open` 为非零持仓，`Trading` 为非零持仓或还有活动订单。

Accounts 支持直接定位用户的两个账户，无需扫描所有用户。Positions/AccountPositions 会遍历所选账户的保留仓位以保证手工录入数据不遗漏；ManagedPositions/Open、ManagedPositions/Trading 在指定账户/用户时使用账户活跃索引。MarketPositions/Trading 使用活跃仓位与订单参与者索引的并集，并去重，不扫描市场已完成仓位。

AccountView 的 openPositionCount/tradingPositionCount 是**引擎管理索引**的计数；positionCount 包含账户所有记录。余额 total 和全仓 liquidationEquity 以宽整数求和后检查范围，不能装入 Balance 时返回空 optional，避免截断。全仓 liquidationEquity 与引擎强平判断的 `active + unrealizedPnL + orderReservation` 对齐。

资金费率输入及成交统计不会凭空生成 K 线、24 小时涨跌幅、指数行情等当前引擎没有维护的数据。开发者可通过近期成交/合约事件或持久化事件出口交给独立行情服务聚合。

## 所属线程直接使用

```cpp
#include "advmatch/observe/TradingObserver.h"

using namespace advmatch;
using namespace advmatch::observe;

ObservationConfig config;
config.indexUserOpenOrders = true;
config.expectedUsersWithOrders = 100'000;
config.tradeHistoryCapacity = 16'384;
config.contractEventCapacity = 16'384;

TradingObserver observer(assets, users, symbols, matching, config);

const auto order = observer.order(orderId);
const auto account = observer.account(accountId);
const auto position = observer.position(positionId, contractAccountId);

ObservationQuery query;
query.kind = ReadKind::Depth;
query.symbolId = symbolId;
query.side = OrderSide::Ask;
query.limit = 20;
query.scanBudget = 20;

std::array<DepthView, 20> buffer;
const auto page = observer.readDepth(query, buffer);
for (std::size_t index = 0; index < page.count; ++index)
{
    // buffer[index] 是独立数值副本。
}
```

`readOrders(span<OrderId>, span<optional<OrderView>>)` 同样复用调用者内存，按输入顺序返回，缺失 ID 对应空 optional，处理数量为两个 span 长度的较小值。`read(query)` 为返回列表分配 vector，资产/交易对名称需要复制字符串；低延迟路径优先使用数字点读和 `readDepth`。

直接使用内存引擎时，**每批变更调用 `observer.notifyChanged()`**，包括账户/资产/标记价/归档等；也可构造时提供只读的版本提供器。忘记通知会使分页版本检查失效。`ObservedConsumer` 自动通知交易命令；持久化构造版本自动读取 lastSequence/durableThrough/fault。即使交易失败，consumer 也使旧游标过期，避免读取异常后可能已部分改变的状态。

## 分页、批量与工作预算

`ObservationQuery.limit` 限制返回行数，`scanBudget` 限制扫描候选数，均必须为正且不超过配置。默认上限分别为 1024 和 16384。过滤稀疏数据、空账户或归档后的空槽时，结果可能没有行，但仍然 `hasMore=true`；必须继续 next 游标。

```cpp
auto result = observer.read(query);
if (result.page.state == ReadState::Success && result.page.hasMore)
{
    query.cursor = result.page.next;
    result = observer.read(query);
}

std::array<ObservationQuery, 3> queries;
queries[0].kind = ReadKind::User;
queries[0].userId = userId;
queries[1].kind = ReadKind::Balances;
queries[1].userId = userId;
queries[2].kind = ReadKind::AccountPositions;
queries[2].accountId = contractAccountId;

const auto batch = observer.readBatch(queries);
```

每个 child 结果有独立 state；batch 的 Success 表示批量请求本身成功，不表示所有 ID 都存在。默认最多 16 个查询，scanBudget 总和最多 65536。批量在同一消费边界读取，结果版本相同，适合用户资产/持仓页面。不同缓存槽的刷新版本可以不同，要用 batch 或比较 stamp，不能把不同槽自动当成全局一致快照。

普通游标绑定 observer session、revision、命令 sequence 和全部筛选条件。交易变更或过滤条件变化返回 CursorExpired，重新从空 cursor 开始。limit/scanBudget 可变。游标不保存迭代器或指针，也不从头重扫已访问记录。不要自行构造 anchor/secondary。

Trades/ContractEvents 游标允许新命令继续发生；读到的近期记录被环形历史覆盖时返回 CursorExpired。没有容量则 HistoryUnavailable。

只有盘口及 BookOrders 有明确价格/FIFO 顺序；hash/dense 索引列表顺序不属于稳定业务排序。高频交易下跨多次请求的全量一致扫描可能持续过期；百万条数据的同一时点导出需要专门的快照/导出窗口，代价是 O(N) 的复制和内存。这里提供有界在线查询，不会隐藏这项成本。

## 外部线程查询和缓存

包含 `ObservedConsumer.h`，继承 `ObservedConsumer<业务命令, 业务响应>`，实现 `consumeCommand(Command&&)`，成员完成初始化后 `bindObserver(observer)`，再 start。派生类析构最先调用 `stopBeforeDerivedDestruction()`，确保 worker 停止后才销毁引擎与 observer。

```cpp
ObservationCache cache(1024);
auto handle = cache.create(query);

consuming::ResponseSlot<ObservedResponse<BusinessResponse>> response;
auto ticket = worker.post(ObservationRequest{query, handle, {}}, response);
const auto reply = ticket.get();
const auto image = std::get<1>(reply);

// 任意读线程；刷新前可能是 nullptr。
const auto cached = handle->load();
if (cached != nullptr && cached->page.state == ReadState::Success)
{
    const auto levels = cached->rows<DepthView>();
    // levels 的有效期受 cached 生命周期约束。
}
```

query 必须与 handle 创建时的查询一致，否则不会发布。单条读取回复在 variant index 1，交易响应在 index 0，batch 在 index 2。提交 batch 时设置 request.batch，不能同时设置单查询 slot。普通查询不带 slot 时仅回复，不缓存。

缓存不是自动更新的。通过再次提交同一 ObservationRequest 刷新，检查 `page.stamp` 判断新鲜度。旧 shared_ptr 可继续使用，更新只发布新的不可变响应；序号、版本和已落盘水位倒退的响应不会覆盖新响应。同一版本的多个刷新可替换。长期持有旧响应会占用内存，应由调用者管理订阅和生命周期。cache 限制存活槽数量，单页大小仍受 observer 配置限制。

完整可运行用法见 `examples/observe/main.cpp`。

## 用户挂单索引

`indexUserOpenOrders` 默认 false，普通交易只多一次空 listener 分支。开启后，挂单插入/移除更新 `userId -> orderId set`，用户挂单读取无需扫描全部订单。初始化/显式重建遍历全部已有挂单，属于冷路径；后续新建订单簿自动接入。

索引增加内存、hash 操作及可能的分配，开关由开发者按业务选择。同一 MatchingEngine 只能附加一个启用该索引的 observer，可另外创建只读 observer。索引分配失败不会中断交易，查询返回 IndexUnavailable；所属线程显式 `rebuildOpenOrderIndex()` 恢复。关闭索引时，可用 BookOrders 查询指定市场或 Orders 有界扫描用户订单。

## 近期历史与持久化接入

```cpp
TradingObserver observer(persistentEngine, config);
ObservationEventSink sink(observer, existingEventSink);
persistentEngine.setEventSink(&sink);

// execute 结束后的事件回调记录近期成交/合约事件。

persistentEngine.setEventSink(nullptr);
// sink、observer 的销毁发生在 engine 销毁之前。
```

事件桥按完整命令批次记录，不分配历史节点。内存引擎可自行收集 TradeBuffer 和 ContractEventSink 的事件，在命令完整结束后调用 `recordBatch(sequence, trades, events)`；每个 sequence 仅接受一次，0/重复/旧批次忽略。记录成交前不要归档本批订单，归档后无法补出账户/用户元数据。事件数据不再用于结算，避免重复执行资金转移。

容量达到上限后覆盖最旧记录，observer 重建不自动回填以前的事件。`sequence` 是命令次序，`durableThrough` 才是实际持久化水位；未落盘的结果不应对外宣称已持久化。完整历史、用户长期账单、K 线及审计数据接持久化事件出口/历史数据库，近期环不是历史存储。

## 复杂度和扩展

- 订单/账户直接 ID 索引点查 O(1)，用户、资产、交易对、仓位 hash 查找为平均 O(1)。inline 余额最多扫描 10 个槽，溢出余额平均 O(1)。返回名称仍需 O(字符串长度) 复制。
- Radix 价格点查的层数固定，盘口遍历按访问档数计；Tree 的 next 使用有序树 upper_bound，每档 O(log 价格档数)。FIFO 挂单读取 O(扫描挂单数)，跨档还包含价格索引成本。
- 其他分页 O(扫描候选数 + 返回数据复制量)，由工作预算限定，不把全量输出称为 O(1)。
- 缓存句柄无需数据集查找，load 取得一个 shared_ptr；实际多线程吞吐受原子引用计数、标准库实现和内存带宽影响。发布/销毁旧图像仍有复制和释放成本。

自定义 OrderBook 派生类需实现 `observedBestLevel`、`observedNextLevel`、`observedLevelCount`，并在挂单进入/离开时调用 `notifyRested`/`notifyRemoved`。自定义 ContractAccount 实现 `observedPositionAt(index)`，在所属线程提供 O(1) 或有界的位置访问。接口只供 observer 的值复制使用，不向外部调用者暴露借用节点。

## 构建与测试

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
.\build\release\advmatch_observe_example.exe
.\build\release\advmatch_observe_benchmark.exe 50000 1000000
```

benchmark 对比用户挂单索引开关，输出挂单写入 TPS、数字订单点读、20 档盘口读、缓存 load 及单次刷新耗时。它不包含成交、网络、WAL、撮合队列和结算，不能等价于交易 TPS，也不能据此保证所有生产环境读写开销。
