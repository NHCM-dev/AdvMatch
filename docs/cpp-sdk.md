# C++ 对象 SDK

入口是 `<advmatch/sdk/AdvMatch.h>`，命名空间为 `advmatch::sdk`。CMake 目标为 `AdvMatch::sdk`。SDK 使用现有 Exchange 的单写队列、风控、结算、观察和持久化服务，在同一进程内提交 C++ 值命令，不经过 JNI 或二进制编解码。

## 构建、打包与接入

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release -R advmatch_sdk
cmake --install build/release --prefix build/sdk
cpack --config build/release/CPackConfig.cmake
```

`ADVMATCH_BUILD_CPP_SDK` 默认开启。安装目录包含 SDK 头文件、DLL/共享库、链接库、CMake package 和独立消费者示例。外部项目不需要本仓库的 src、Boost、Abseil、Disruptor 或其他第三方头文件。

```cmake
find_package(AdvMatch CONFIG REQUIRED)
add_executable(my_exchange main.cpp)
target_link_libraries(my_exchange PRIVATE AdvMatch::sdk)
```

配置外部项目时指定 `-DCMAKE_PREFIX_PATH=安装目录`；Windows 运行时将 `bin/advmatch_sdk.dll` 放在应用程序旁边，或将该 bin 目录加入 PATH。其他平台设置库搜索路径或 RPATH。完整可运行示例在 `examples/cpp-sdk`；该目录可以直接作为独立 CMake 项目编译。

C++ ABI 要求消费方与 SDK 使用兼容的编译器、架构、标准库与运行库；MinGW 的 DLL 不能直接给 MSVC 的 C++ 应用链接。交付给另一种工具链时，应使用其工具链重新构建 SDK。Java 的 JNI/C ABI 接口继续独立工作。

## 对象与 Manager

```cpp
#include <advmatch/sdk/AdvMatch.h>
namespace sdk = advmatch::sdk;

sdk::ExchangeConfig config;
config.persistence.enabled = false;
config.observation.indexUserOpenOrders = true;
sdk::MatchingExchange exchange(config);

auto btc = exchange.assets().create("Bitcoin", "BTC", 0).get();
auto usd = exchange.assets().create("Dollar", "USD", 0).get();
auto user = exchange.users().create().get();
auto account = user.spotAccount().get();
account.updateBalance(usd, 10'000).get();
auto symbol = exchange.symbols().create("BTCUSD", sdk::SymbolType::Spot, btc, usd).get();
auto book = exchange.books().addSpot(symbol).get();
auto order = book.place(user, sdk::OrderSide::Bid, sdk::OrderType::Limit, sdk::TimeInForce::GTC, 100, 2, 1).get();
order.modifyPrice(99, 2).get();
order.cancel(3).get();
exchange.close();
```

| 入口 | 功能 |
| --- | --- |
| assets / users / symbols | 创建、get/find/list、实体 snapshot/remove |
| accounts | 账户类型分派、余额与全仓风险快照；User 返回真实 SpotAccount/ContractAccount ID |
| SpotAccount / ContractAccount | 余额、预留余额、划转、账户仓位；updateBalance 用 BalanceChange，transferTo 用 PositiveBalanceChange |
| books / matching / orders | 创建现货或合约簿、下单、改单、撤单、盘口/价位/挂单/成交读取 |
| positions | 独立仓位快照、杠杆、逐仓保证金、强平、managed/pending 查询 |
| contracts | 标记价、资金费、保险基金、强平预算/处理、期货到期和现金交割、合约事件 |
| observations | 所有 ReadKind、批量读取、总览、缓存槽、重建订单索引 |
| persistence | 状态、flush/tick/checkpoint、已提交历史、已批准历史归档 |

实体只保存 `shared_ptr<ExchangeApi>` 和真实 ID。`id()` 与 `handle()` 等价；构造/丢弃包装不会创建/删除引擎实体。get 构造轻量引用，find 通过查询验证；AccountManager.find 返回实际子类型的 `shared_ptr<Account>`。所有异步回调按值捕获会话/ID，包装对象提前销毁不会产生悬空 this。

实体属于原会话，跨会话传入 User、Asset、Symbol、Account 或缓存槽会被拒绝。删除/归档后的 snapshot 通过 ReadError 报告，Exchange 销毁后的调用通过失败 Future 报告 Closed。SDK 不提供可变内部 Manager 或 User*/OrderNode*/Position*。

### 接入已有原生 Exchange

仓库内已经持有原生 Exchange 的代码可以使用 `wrapExchange` 共用同一个队列和状态：

```cpp
auto native = std::make_shared<advmatch::Exchange>(config);
sdk::MatchingExchange facade(sdk::wrapExchange(native));
auto user = facade.users().create().get();
```

这里需要原生头文件并链接 `AdvMatch::core`；普通外部 SDK 应用只用 `AdvMatch::sdk`。包装销毁会关闭该共享会话；多个 facade 共享 API 时，任意一个关闭都会停止同一会话的准入。不能把不同适配器包装的实体互相传入，即使它们背后使用同一个原生对象。

## 命令与异步结果

`<advmatch/sdk/command/Commands.h>` 包含全部交易、读取和持久化命令；自定义命令可以继承 `sdk::command::Command<T>`，在 `execute(ExchangeApi&)` 中组合公开异步能力。它不接触内部引擎对象，也不会绕过订单串行执行和 WAL。

```cpp
auto result = exchange.submit(sdk::command::CreateUser{}).get();
auto id = result.id(); // CreationResult 同时保留 receipt
sdk::ObservationQuery query;
query.userId = id;
auto user = exchange.submit(sdk::command::ReadUser(query)).get();
```

对象层的业务拒绝转为 BusinessError，包含完整 CommandReceipt；单对象读取错误转为 ReadError，包含 ReadPage。原始命令返回未经转换的业务状态，调用方应检查 accepted/state。列表查询保留分页状态、游标和 scanBudget，不自动扫描全部结果。返回 DTO 拥有数据，getShared 可以保留 Future 的只读结果；不会借用引擎容器。

`PendingLiquidation` 表示存在尚未完成的强平，标记价更新等前置状态可能已经生效；应按 receipt 中的 ContractState 处理，不能把所有非成功结果理解为无状态变化的拒绝。需要直接处理这类状态时，使用原始命令返回的 CommandReceipt。

Future 提供 get/join/getFor、thenApply/thenCompose、whenComplete、exceptionally、allOf/anyOf。回调用配置的异步 Executor；可使用 SDK 的 ThreadPoolExecutor 或自定义 Executor。不要在有限回调线程池中阻塞等待其他回调。组合 Future 的 cancel 只取消组合结果，不能保证取消其上游交易；创建命令的 CreationResult 转换也属于组合 Future。需要原生取消语义时使用返回 CommandReceipt 的原始命令，例如 ExecuteTrading，它只能在尚未开始时撤销。等待超时不撤销交易。

Order/Position 的便利方法需要先读取归属信息，再提交修改，因此会多一次队列查询。已知 userId/symbolId 的热路径可以直接提交对应命令，保留一次准入及原生取消语义。SDK 不承诺把查询、分配或任意规模分页变成 O(1)。

`closeAsync()` 立即关闭准入并返回排空 Future，`close()` 等待排空；析构也执行关闭，已接受的交易不会被取消。保留的实体仍指向关闭的会话，后续提交安全失败。显式等待关闭可以观察 I/O 错误。

## 缓存和持久化

`observations().createSlot(query)` 返回 `shared_ptr<ObservationSlot>`，显式 `refresh()` 发布快照，`load()` 返回最近的拥有型快照。未刷新时 load 为 null；关闭后可继续读取已发布快照，refresh 会失败。最后一个槽引用释放容量。atomic shared_ptr 是否无锁由标准库实现决定。

持久化默认开启。多个实例使用各自目录，恢复后先读取数据，不能重复初始化。requestId=0 不去重；非零 requestId 必须符合现有 WAL 的全局单调规则。资金费、标记价、到期、交割与强平调度仍由调用方提交命令。SDK 对 Futures、Cross/Isolated、双向独立 Position 的规则与核心引擎保持一致。
