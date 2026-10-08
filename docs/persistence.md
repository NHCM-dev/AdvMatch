# 可配置的交易持久化

交易所对外入口现在使用 `Exchange` + 异步命令，见 `docs/exchange-api.md`。persistence 按 abstract / config / model / command / codec / impl 分包；下文的旧头文件路径保留转发兼容，持久化格式与操作码未改变。

## 入口与范围

包含 `advmatch/persistence/PersistentTradingEngine.h`，链接 `AdvMatch::core`。

`PersistentTradingEngine` 拥有 AssetManager、UserManager（含 AccountManager）、SymbolManager、MatchingEngine。它在构造时恢复已有数据，空目录则创建新系统。全部持久化变更通过 `execute(TradingCommand, requestId)` 提交，查询通过 const getter 完成。

支持的命令：

| 类别 | `commands` 命名空间中的类型 |
|---|---|
| 资产、用户、交易对 | CreateAsset / RemoveAsset / CreateUser / RemoveUser / CreateSymbol / RemoveSymbol |
| 账户资金 | UpdateBalance / Transfer |
| 产品与订单簿 | AddSpotBook / AddContractBook，支持 Radix 和 IndexedTree |
| 订单 | PlaceOrder / ModifyPrice / ModifyQuantity / CancelOrder |
| 合约风险 | SetLeverage / AddMargin / UpdateMarkPrice |
| 合约生命周期 | FundInsurance / SettleFunding / Expire / Deliver |
| 强平调度 | Liquidate / ProcessLiquidations / SetAutomaticLiquidationBudget |
| 历史留存 | ArchiveHistory |

风险校验、预留资金、成交结算仍由现有引擎执行。`PlaceOrder.contractOptions` 留空表示现货，传入则使用合约下单流程。标记价、资金费率、时间戳均由开发者输入，重放不会读取实时行情或当前系统时间。用户创建时分配现货和合约两个账户，调用 `getAccountId()` 取得账户 ID，避免硬编码。

直接创建的旧 `MatchingEngine` 继续是内存引擎。不要通过 const_cast、回调内借用指针或旧 Manager 接口修改持久化实例；这些变更无法进入 WAL。构造恢复完成之后，读写、维护及回调都在同一个 owner/consumer 线程执行，不能在生产者线程读内部对象。Getter 返回的引用随实例销毁而失效，历史归档也可能使订单/仓位地址失效。

## 最小使用示例

```cpp
#include "advmatch/persistence/PersistentTradingEngine.h"

using namespace advmatch;

PersistenceConfig config;
config.directory = "data/exchange-a";
config.flushPolicy = FlushPolicy::EveryCommand;

PersistentTradingEngine engine(config);

// 这里的初始化只适合一个全新数据目录。
const auto base = engine.execute(commands::CreateAsset{"Bitcoin", "BTC", 0}, 1);
const auto quote = engine.execute(commands::CreateAsset{"Dollar", "USD", 0}, 2);
const auto user = engine.execute(commands::CreateUser{}, 3);
const auto symbol = engine.execute(commands::CreateSymbol{"BTC-USD", SymbolType::Spot,
    static_cast<AssetId>(base.result.entityId), static_cast<AssetId>(quote.result.entityId)}, 4);

engine.execute(commands::AddSpotBook{static_cast<SymbolId>(symbol.result.entityId)}, 5);
const auto* owner = engine.getUserManager().getUser(user.result.entityId);
const auto accountId = owner->getSpotAccount().getAccountId();
engine.execute(commands::UpdateBalance{accountId, static_cast<AssetId>(quote.result.entityId), 10'000}, 6);

const auto receipt = engine.execute(commands::PlaceOrder{user.result.entityId,
    static_cast<SymbolId>(symbol.result.entityId), OrderSide::Bid, OrderType::Limit,
    TimeInForce::GTC, 100, 10, 1}, 7);

if (receipt.result && receipt.durable())
{
    // 可以确认订单；entityId 是 OrderId。
}

engine.close();
```

再次用同一目录构造即可恢复。对已有目录不要无条件重跑初始化；保留配置生成的 ID，或在 owner 线程查询并按业务规则初始化。`CommandResult.type` 决定 `state` 对应的枚举类型，`accepted`/显式 bool 表示业务结果。被业务拒绝的命令也记录，因为它可能消耗订单 ID 或触发到期/强平维护。`PendingLiquidation` 也可能已经更新标记价并入队，不能把非 Success 一概理解为完全未修改状态。

余额命令继续区分类型：`UpdateBalance.change` 是有正负的 `BalanceChange`，`Transfer.amount` 是非负的 `PositiveBalanceChange`。账户实际余额使用 `Balance`，预留操作由风控和账本完成。

## 配置

所有配置在构造前设置，可从应用自己的 JSON/TOML/环境变量映射到此结构；引擎没有全局配置单例，也不强制依赖某种配置文件解析库。

| 字段 | 默认 | 含义 |
|---|---|---|
| enabled | true | false 使用相同命令入口但只保留内存状态，不创建文件 |
| directory | data/advmatch | 实例目录；不同交易实例使用不同目录 |
| flushPolicy | EveryCommand | 每条刷盘 / Batch / Manual |
| flushEveryCommands | 256 | Batch 的累计未刷盘命令阈值，必须大于 0 |
| flushIntervalMilliseconds | 10 | Batch 的时间阈值，0 禁用时间条件 |
| checkpointEveryCommands | 0 | 每 N 条命令自动检查点，0 仅显式 checkpoint() |
| retainedCheckpoints | 2 | 保留最近 N 个完整检查点，至少 1 |
| pruneJournalAfterCheckpoint | true | 检查点持久化后删除已覆盖 WAL |
| tailRecovery | TruncateIncomplete | 只截断末尾不完整帧；Reject 则拒绝恢复 |
| maxRecordBytes | 1 MiB | 单条 Intent 或 Commit 的最大长度，Commit 包含所有成交及合约事件 |
| maxSnapshotBytes | 4 GiB | 单个重放检查点的最大长度 |
| retainedRequests | 10,000 | 最近非零 requestId 的去重缓存容量，0 不缓存响应但仍拒绝旧 ID |

Batch 时间条件在 `execute()` 或 `tick()` 时检查；没有后台定时器。在空闲期间需要按时间刷盘时，让消费者的维护任务调用 `tick()`，不要从第二个线程直接调用。`flush()`、`checkpoint()` 和正常 `close()` 都会显式刷盘。析构没有可报告错误的刷盘保证，应用应在 owner 线程调用 `close()` 后再停止消费者。

```cpp
config.flushPolicy = FlushPolicy::Batch;
config.flushEveryCommands = 512;
config.flushIntervalMilliseconds = 5;
config.checkpointEveryCommands = 0;
config.maxRecordBytes = 16 * 1024 * 1024; // 按单命令最大成交/事件数量选择
```

EveryCommand 返回成功前调用文件 flush 和操作系统持久化同步。Batch/Manual 的执行结果可能仅在内存/缓冲区中，`receipt.durable()` 为 false；必须等 `durableThrough() >= receipt.sequence` 才能声明已持久化。禁用持久化时永远不返回 durable=true。持久化保证依赖文件系统、磁盘正确履行同步要求，不包含磁盘损坏后的异地恢复。

## 写入、恢复与故障

```text
单消费者接收命令
    → WAL Intent（命令、外部 requestId）
    → 原有业务引擎执行
    → WAL Commit（结果、成交、合约事件）
    → 按配置 sync
    → 返回 sequence / durableThrough / result
```

Intent 在业务变更前写入，Commit 随后写入。默认一次 sync 覆盖两条记录。如果崩溃留下完整 Intent，恢复会执行它并补 Commit。因此未收到回复的命令可能已经提交；重试需要 requestId。末尾不完整 Intent 会被移除；完整 Intent 后的不完整 Commit 移除后重建。

文件采用显式小端整数、格式版本和 CRC32C，序列号参与校验，不保存裸指针或 C++ 内存布局。恢复按顺序执行，并逐字比较结果、成交、合约事件。序列缺口、完整帧校验失败、未知版本、重放结果不一致都会拒绝启动，不跳过变更，不自动回退到旧检查点。CRC 用于损坏检测，不提供恶意篡改防护。

数据目录用系统级锁保持单写者。同一目录的第二个实例会收到 `PersistenceFailure::Locked`。写入、刷盘或执行过程中出现资源异常，实例进入 faulted 状态，后续变更被拒绝；销毁该实例，解决存储问题，然后重新构造恢复。不要在未知提交结果后把同一业务请求换成新 ID 再发送。

## 请求去重

`requestId=0` 表示不启用重试去重。非零 ID 是这个实例范围内的单调递增数，首次使用必须大于此前最大的非零 ID，并按消费顺序分配；多个生产者提前分配 ID 后乱序提交可能被拒绝。业务客户端自己的随机请求 ID 可在接入层映射到这个序列。

相同 ID 和相同命令返回原来的结果和原来的 CommandSequence，`duplicate=true`，不会再次修改账户或撮合。相同 ID、不同命令报 RequestConflict。缓存过期或乱序的旧 ID 报 RequestExpired，不会作为新命令执行。请求最大值和缓存通过日志重放恢复，因此重启仍支持去重。缓存容量变化仅改变能返回旧结果的范围，不允许旧 ID 再次执行。

重复回复不重新产生 `getTrades()` 或实时回调。完整成交/合约事件从 `visitCommitted()` 读取。

## 成交、合约事件与外部消费者

`setEventSink(PersistentEventSink*)` 的实时回调发生在 Commit 写入与所选刷盘操作之后；Batch 回调的 receipt 可能还未持久化。回调必须 noexcept，不重入引擎。重启重放期间不会调用开发者的实时事件回调。

需要可靠交付时，使用 `visitCommitted(afterSequence, visitor)`。它先 flush，然后顺序读取已提交的命令结果、现货成交及合约事件，包括检查点中的记录。外部存储将 `(CommandSequence, tradeIndex/eventIndex)` 作为幂等键，并持久化已投递游标；失败后重复读取仍能去重。visitor 抛异常可以重试，不会修改交易状态。这是持久化事件读取能力，不自动保证数据库/消息队列的 exactly-once 投递。

`getTrades()` 是最近一次新命令的临时结果；恢复和重复请求会清空它。合约自动强平内部生成的成交以 ContractEvent::Fill 保存；不要再把 Fill 与同一笔 Trade 当作两笔独立结算。

## 检查点的当前边界

这版检查点是**完整命令重放镜像**，不是账户/订单/仓位的直接状态快照。它保留从第一条开始的命令和结果，重放时重新生成对象池及稳定指针、预留资金、价格时间队列、仓位和强平队列，不序列化内存地址。

`checkpoint()` 先刷盘，写 snapshot-N.bin.tmp，刷盘后原子发布 snapshot-N.bin，再原子发布 checkpoint.head，最后才截断已覆盖 WAL、清理旧镜像。临时文件不用于恢复；manifest 防止最新检查点文件缺失后静默回退。停止写入后备份整个目录，包括 manifest、检查点和 WAL。

**性能限制：恢复仍为 O(历史命令 + 重放撮合/结算工作)，检查点也为 O(历史数据)，有暂停和临时内存开销，镜像仍会持续增长。** WAL 文件裁剪不等于删除业务历史。默认关闭自动检查点，避免长时间运行时周期性复制大日志。当前实现不把这个方案宣称为 O(活动状态) 快速启动、常数磁盘占用或全天无停顿快照。大规模 24/7 部署需要后续增加值类型状态快照、分段日志与在线快照流程。

引擎语义变化需要迁移：TradingCommand alternative 只能追加，已有字段布局、撮合/风控规则变化要升级格式并提供旧版本重放工具。固定宽度整数避免编译器布局差异，但未经迁移不能承诺跨任意引擎版本恢复。数量预算字段对应 size_t，当前持久化实现面向 64 位目标。

## 自定义后端

继承 `PersistenceStore`，实现 loadSnapshot / replay / append / sync / publishSnapshot / resetJournalThrough，再注入：

```cpp
auto store = std::make_unique<MyPersistenceStore>(myConfig);
PersistentTradingEngine engine(config, std::move(store));
```

自定义后端需要保持单写者、记录顺序、损坏检测和原子检查点发布。`sync()` 必须落到持久化介质；`publishSnapshot()` 返回时镜像必须已持久化；`resetJournalThrough()` 只能删除已被持久化检查点完整覆盖的记录。接口中的 StoredSnapshot.payload 是版本化的重放镜像，不应当把它当成原始对象状态自行改写。后端传输远程数据时，连接重试不能导致重复 append 或乱序。

## 接入 SingleThreadConsumer、构建与测试

可编译示例位于 `examples/persistence/main.cpp`：派生 SingleThreadConsumer，消费者独占 PersistentTradingEngine，生产者提交命令；检查点、close 也作为维护消息交给相同消费者。查询通过维护消息返回值，避免主线程直接读正在变化的内部对象。示例保留主线程启动前的恢复，随后首次消费绑定 owner 线程。

```powershell
cmake --preset release
cmake --build --preset release --target advmatch_persistence_example advmatch_persistence_tests
ctest --preset release -R advmatch_persistence_tests

.\build\release\advmatch_persistence_example.exe data\demo every
.\build\release\advmatch_persistence_example.exe data\demo every
```

第二次运行先恢复上一次用户，再新增一个用户。每次执行都打印已恢复的 sequence 与 durable，通过重复运行能观察实际恢复。也可将 every 改成 batch 或 manual，观察下单响应的 durability 和检查点之后的刷盘进度。`ADVMATCH_BUILD_EXAMPLES=OFF` 可以关闭示例构建。

测试覆盖 FIFO、两种订单簿、全仓/逐仓、资金费率/交割/待强平、归档后 ID、去重、每条/批量/手动刷盘、锁冲突、残缺帧、损坏检查点、格式版本和序列错误、注入写盘失败，以及独立子进程在没有析构/清理时退出后的恢复。
