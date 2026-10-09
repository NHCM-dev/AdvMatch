# Exchange 与异步命令 API

## 使用入口

外部 C++ 项目建议使用可独立安装的对象 SDK：`<advmatch/sdk/AdvMatch.h>`、`advmatch::sdk::MatchingExchange` 和 `AdvMatch::sdk`。User、Account、Order、Position 及各种 Manager 的用法见 [C++ SDK](cpp-sdk.md)。下面介绍仓库内的原生命令接口。

包含 `advmatch/Exchange.h`、`advmatch/api/command/Commands.h`，链接 `AdvMatch::core`。这是 C++ API；Java 通过 JNI 调用同一套交易所命令，见 `docs/bindings.md`。

`Exchange` 拥有完整交易会话：资产、用户、账户、交易对、现货/期货/永续撮合、风控、资金预留与结算、仓位、杠杆、强平、资金费、交割、持久化和读取。功能范围与现有交易引擎相同，Option 类型的元数据不表示已经实现期权定价/交易引擎。

构造时加载或恢复持久化数据并启动一个消费线程。多个生产者通过 `api::ExchangeApi::submit(...)` 提交命令，操作与查询按队列发布顺序串行执行。API 不返回可变 Manager/Engine 引用，避免绕过 WAL 或从读线程访问正在变化的容器。

```cpp
#include "advmatch/Exchange.h"
#include "advmatch/api/command/Commands.h"

using namespace advmatch;
namespace command = advmatch::api::command;

ExchangeConfig config;
config.persistence.directory = "data/exchange-a";
config.persistence.flushPolicy = FlushPolicy::EveryCommand;
config.ringBufferSize = 1024;
config.observation.indexUserOpenOrders = true;

Exchange exchange(config);
api::ExchangeApi& api = exchange;

auto user = api.submit(command::CreateUser({}));
const auto receipt = user.get();

observe::ObservationQuery query;
query.userId = receipt.result.entityId;
const auto response = api.submit(command::ReadUser(query)).getShared();
if (response->page.state == observe::ReadState::Success)
{
    const auto accountId = response->values.front().spotAccountId;
}

api.close().get();
```

持久化恢复后不要重复初始化资产/用户/交易对。先提交 ReadOverview / ReadAssets / ReadUsers 等读取现有状态。测试或纯内存使用设置 `config.persistence.enabled=false`。构造失败直接抛异常，提交后的基础设施错误通过 Future 传播。

完整可运行的初始化、现货下单、链式查询、盘口缓存示例在 `examples/exchange/main.cpp`。

## 分包与职责

```text
advmatch/
  Exchange.h / Exchange.cpp           会话、线程、排空关闭、服务装配
  config/ExchangeConfig.h             交易所配置
  api/
    ExchangeApi.h                    抽象提交入口
    async/                           Future / CompletionSource / Executor
      detail/                        完成状态与等待机制
      impl/                          ThreadPoolExecutor
    command/
      Command.h / CommandContext.h   抽象命令与所属线程能力
      management/                    资产、用户、交易对
      account/                       余额与划转
      order/                         下单、改单、撤单、现货订单簿
      contract/                      保证金、杠杆、资金费、强平、交割
      read/                          业务查询与盘口缓存
      persistence/                   Flush / Tick / Checkpoint / 历史查询
      maintenance/                   已批准历史归档
      trading/                       公用交易命令执行模板
      model/                         原有可序列化交易命令数据
    model/                           CommandReceipt、查询响应
    service/                         TradingService
    detail/                          类型擦除的队列任务，不暴露到业务层
  observe/
    abstract/                        ObservationService、OrderBookListener
    model/                           查询、游标、状态与业务 DTO
    impl/                            TradingObserver、ObserverService、事件桥
    cache/                           不可变快照缓存
    consuming/                       原有 ObservedConsumer 适配器
  persistence/
    abstract/                        PersistenceStore / Service / EventSink
    config/                          持久化配置
    model/                           日志、检查点、状态、已提交历史
    command/                         所属线程业务执行
    codec/                           二进制编码与完整性校验
    impl/                            文件后端、持久化交易会话、服务适配
```

observe/persistence 的旧头文件路径保留为转发头，原有调用继续编译。新代码优先包含上述分包路径。移动文件没有改变 WAL 操作码、命令布局或持久化格式。

## 提交命令

每个命令继承 `Command<Response>`，响应类型在编译期确定。可以提交值、`shared_ptr<具体命令>` 或 `shared_ptr<const Command<Response>>`，返回 `CompletableFuture<Response>`。提交值时 API 持有副本；命令和响应不依赖调用栈上临时变量的寿命。

交易命令构造统一为 `命令(数据, requestId=0)`。例如：

```cpp
auto placed = api.submit(command::PlaceOrder({userId, symbolId, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, price, quantity, timestamp}));

ContractOrderOptions options;
options.marginMode = MarginMode::Cross;
options.leverage = 10;

auto contractOrder = api.submit(command::PlaceOrder({userId, contractSymbolId, OrderSide::Bid, OrderType::Limit, TimeInForce::GTC, price, quantity, timestamp, options}));

auto changed = api.submit(command::ModifyQuantity({userId, orderId, quantityChange, timestamp}));
auto cancelled = api.submit(command::CancelOrder({userId, orderId, timestamp}));
```

生产者之间没有业务依赖时可并行提交。有依赖时等待结果或使用 thenCompose；例如 CreateUser 完成后才用返回的 UserId 下单。requestId 非零时沿用持久化去重规则：全局执行顺序单调增长；相同 ID 重试同一数据返回 duplicate，不重新执行。多个生产者在提交前分配递增 ID 不等于入队顺序递增，要由接入层保证。

业务拒绝是正常响应：`receipt.result.accepted=false`，通过 `result.type` 解释 `state`，例如 PlaceOrderState、BalanceOpResult、ContractState。队列满、关闭、无效抽象命令、持久化 I/O、回调异常等是 exceptional completion，通过 get/join 或 exceptionally/handle 处理。不要把 Future 成功完成当成订单必然被接受，也不要把 `sequence` 当成落盘保证；`receipt.durable()` 检查持久化水位。

| 分类 | 命令 |
|---|---|
| 管理 | CreateAsset / RemoveAsset / CreateUser / RemoveUser / CreateSymbol / RemoveSymbol |
| 账户 | UpdateBalance / Transfer |
| 订单簿 | AddSpotBook / AddContractBook |
| 订单 | PlaceOrder / ModifyPrice / ModifyQuantity / CancelOrder |
| 合约风险 | SetLeverage / AddMargin / UpdateMarkPrice / FundInsurance |
| 生命周期 | SettleFunding / Expire / Deliver |
| 强平 | Liquidate / ProcessLiquidations / SetAutomaticLiquidationBudget |
| 留存 | ArchiveHistory（外部存储已确认的订单/仓位 ID） |
| 持久化 | ReadPersistenceStatus / Flush / Tick / Checkpoint / ReadCommitted |
| 缓存与索引 | CreateObservationSlot / RefreshObservation / RebuildOrderIndex |

`ExecuteTrading(TradingCommand, requestId)` 可提交运行时选择的原有命令 variant。各命令共用 TradingOperation 模板，交易逻辑集中在原有执行器，避免为每种 API 复制风控/资金处理。

## 读取

读取覆盖 observe 的全部 ReadKind：资产、用户、账户、余额、全仓风险、交易对、最优报价、深度、价格档、挂单、完整订单、所有/活跃/市场仓位、合约市场、待强平队列、近期成交和合约事件。

`ReadUser` / `ReadOrders` / `ReadDepth` 等具体命令返回 `ReadResponse<相应 DTO>`，读取 `values` 或 `rows()`，无需手动从 variant 判断类型。`ReadOverview` 返回 OverviewResponse，读取可选的 value。动态查询 `Read(query)` 和 `ReadBatch(queries)` 保留 observe 的通用响应结构。

```cpp
observe::ObservationQuery query;
query.symbolId = symbolId;
query.side = OrderSide::Ask;
query.limit = 20;
query.scanBudget = 20;
auto depth = api.submit(command::ReadDepth(query)).getShared();

if (depth->page.state == observe::ReadState::Success && depth->page.hasMore)
{
    query.cursor = depth->page.next;
    depth = api.submit(command::ReadDepth(query)).getShared();
}
```

筛选条件、游标过期、扫描预算、盘口顺序、inactive/terminal 处理沿用 `docs/observe.md`。普通分页在交易变更后会过期；全量一致导出不是零成本在线查询。ReadBatch 适合在同一命令边界读取用户、余额与持仓。

缓存句柄也通过命令创建和刷新：

```cpp
query.kind = observe::ReadKind::Depth;
auto slot = api.submit(command::CreateObservationSlot(query)).get();
api.submit(command::RefreshObservation(slot)).get();

// 任意读线程，检查 nullptr、ReadState 和 stamp 后使用。
auto image = slot->load();
```

slot 不自动刷新。默认最多 1024 个存活缓存槽，配置 observationCacheSlots。结果完全拥有数据，Exchange 关闭后仍可持有既有结果/缓存图像。

ReadCommitted 读取完整持久化出站历史，每次最多 maxHistoryRows 条，默认 1024；通过 nextSequence 翻页。底层仍扫描历史并可能先 flush，属于昂贵的管理操作，不应频繁投放到撮合队列。近期行情使用 ReadTrades / ReadContractEvents，完整审计/账单通过独立历史存储提供。

## CompletableFuture

支持 get / join / getShared / getFor、isDone / isCancelled / cancel、thenApply / thenAccept / thenRun / thenCompose / thenCombine、exceptionally / handle / whenComplete、completedFuture / failedFuture、同类型输入的 allOf / anyOf。Future 可复制、移动并多次读取，同一结果的所有句柄共享完成状态。

```cpp
auto result = api.submit(command::CreateUser({})).thenCompose([&api](const CommandReceipt& created)
{
    observe::ObservationQuery query;
    query.userId = created.result.entityId;
    return api.submit(command::ReadUser(query));
}).thenApply([](const api::ReadResponse<observe::UserView>& user)
{
    return user.values.front().contractAccountId;
});

const auto accountId = result.get();
```

- get/join 返回独立副本；大查询使用 getShared 取得共享只读结果，减少复制。回调参数以 const 引用读取。
- 所有用户 continuation 均调度到 Executor，包括原 Future 已完成后才注册的回调。默认共享两个后台线程；可传入 ThreadPoolExecutor 或自定义排队执行器。自定义 Executor 必须异步排队，不能在 execute 内直接运行用户回调。
- handle / whenComplete 参数为 `FutureOutcome<T>`，包含只读 value、exception_ptr error、cancelled。whenComplete 保留原响应；原任务失败且回调也失败时保留原异常。
- thenCompose 连接异步结果，不阻塞线程。回调中尽量使用它，不要等待同一有限线程池尚未执行的 continuation。
- getFor 超时只结束本次等待，不取消命令，也不表示交易没有执行。超时重试需要查询结果或使用有效的去重 requestId。
- cancel 只在命令/continuation 尚未开始时成功；执行中的交易、清算、WAL 写入不支持中断或回滚。取消一个派生 Future 不会取消父 Future。
- allOf 等所有输入结束后传播首个失败；空 allOf 成功。anyOf 取第一个已完成结果/失败，空输入报错。它们不自动取消其他输入。
- 撮合线程禁止等待尚未完成的 API Future，会返回 exceptional completion，避免自定义命令导致队列死锁。

Future 使用共享状态、同步通知和命令对象分配，并非原始 ResponseSlot 快路径的零开销替代。队列事件只拥有一个任务指针，不把大响应塞入 RingBuffer。原有 SingleThreadConsumer 的低层接口继续可用。

## 自定义命令与服务

```cpp
class ReadUserAndBalances final : public command::Command<observe::ObservationBatch>
{
    UserId userId;

public:
    explicit ReadUserAndBalances(UserId userId)
        : userId(userId)
    {
    }

    observe::ObservationBatch execute(command::CommandContext& context) const override
    {
        std::array<observe::ObservationQuery, 2> queries;
        queries[0].kind = observe::ReadKind::User;
        queries[0].userId = this->userId;
        queries[1].kind = observe::ReadKind::Balances;
        queries[1].userId = this->userId;
        return context.observation().readBatch(queries);
    }
};
```

CommandContext 只提供 TradingService、ObservationService、PersistenceService 能力。自定义命令运行在所属线程，必须短小、不阻塞、不保存 context/service 引用。持有共享自定义命令时不要从其他线程改变其数据。多个交易操作可以组合，但每个操作是独立 WAL 命令；组合命令不是自动回滚的数据库事务。

PersistenceStore 仍然可自定义：`Exchange(config, std::unique_ptr<PersistenceStore>)`。ObservationService / PersistenceService 作为抽象能力便于独立适配/测试；Exchange 默认装配 ObserverService / EnginePersistenceService。

## 背压与关闭

ringBufferSize 必须为正的二次幂，progressBatchSize 不超过 ring 大小，0 为自动选择。submit 不等待队列空位，满队列返回失败 Future（ApiFailure::QueueFull），由接入层限流或退避重试。提交与关闭有明确的接受边界：边界前接受的命令排空，边界后返回 ApiFailure::Closed。

close() 立即关闭准入，后台排空、在所属线程 flush 并释放数据目录锁，再完成 Future；关闭不取消已接受命令。重复 close 返回相同完成结果，关闭 Future 不可取消。即使 I/O 故障也会释放会话资源，但 Future 仍报告失败。析构等待关闭、不会向外抛异常，因此需要观测落盘错误时显式 close().get()。

API 对象必须比所有正在调用 submit/close 的生产者活得久，不支持与析构同时访问。先停止生产者，再 close；命令内不要销毁所属 Exchange。已完成响应和 Future 可以继续存在，捕获 Exchange 引用的延迟 continuation 则要求它仍存活。

FlushPolicy::Batch 的时间阈值不是自动定时器；没有交易时由调用方提交 Tick，或显式 Flush。定时标记价、资金费和到期/强平调度同样通过已有命令输入，不读取系统墙钟悄悄改变业务状态。

## 构建

其他 C++ 项目通过源码依赖接入，头文件路径与第三方链接依赖由目标传递：

```cmake
set(ADVMATCH_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ADVMATCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ADVMATCH_BUILD_JNI OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/AdvMatch)

add_executable(my_exchange main.cpp)
target_link_libraries(my_exchange PRIVATE AdvMatch::core)
advmatch_configure_runtime(my_exchange)
```

当前 install/ZIP 同时提供完整 C++ 对象 SDK（`AdvMatch::sdk`）、独立消费者示例和原有 C ABI/JNI 库。外部项目通过安装包的 CMake package 接入；上述原生 `AdvMatch::core` 接口仍用于仓库内开发。

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
.\build\release\advmatch_exchange_example.exe
```
