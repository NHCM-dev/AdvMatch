# AdvMatch

(Credit to GPT-5.6, GPT-6.1)

C++20 交易引擎，支持现货、线性期货和永续合约，提供 C++ 对象 SDK 和 Java SDK。

- 单线程撮合，多线程提交命令，异步返回结果。
- 独立仓位、全仓 / 逐仓、杠杆、强平、资金费和现金交割。
- 账户与结算、盘口与数据快照、WAL 持久化与恢复。

## 构建

需要 C++20 编译器、CMake 3.20+、Ninja 和 Git；首次构建需要网络下载依赖，Disruptor 已内置。

```powershell
.\build.cmd release
```

Linux / macOS 使用 `./build.sh release`。脚本完成构建、测试和 ZIP 打包；调试版本将 `release` 换成 `debug`。

产物在 `build/release/`。单独执行构建、测试或安装：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/release --prefix build/sdk
```

## C++ 接入

外部项目配置时指定 `-DCMAKE_PREFIX_PATH=SDK安装目录`：

```cmake
find_package(AdvMatch CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE AdvMatch::sdk)
```

```cpp
#include <advmatch/sdk/AdvMatch.h>
namespace sdk = advmatch::sdk;

sdk::ExchangeConfig config;
config.persistence.enabled = false;
sdk::MatchingExchange exchange(config);
auto user = exchange.users().create().get();
auto account = user.spotAccount().get();
auto snapshot = user.snapshot().get();
exchange.close();
```

通过 `assets()`、`users()`、`accounts()`、`symbols()`、`books()`、`orders()`、`positions()` 等 Manager 操作实体；通过 `submit()` 提交命令。结果为 `CompletableFuture<T>`，创建实体返回真实 ID，读取返回独立快照。

持久化默认开启，使用 `config.persistence.directory` 指定实例目录；纯内存运行如上关闭持久化。完整交易示例：[C++ SDK](examples/cpp-sdk/main.cpp)。

Windows 将 `advmatch_sdk.dll` 放在应用旁边。C++ 应用与 SDK 需要使用兼容的编译器和运行库。

## Java 接入

需要 JDK 21。Gradle Wrapper 自动构建 JNI 并将当前平台的原生库打入 JAR：

```powershell
.\java\gradlew.bat -p java :advmatch:build :advmatch:runExample
java -jar java/advmatch/build/libs/advmatch-0.0.1-tests.jar
```

SDK JAR 位于 `java/advmatch/build/libs/`；测试 JAR 包含集成测试和全部 benchmark。已有原生构建可加 `-PskipNativeBuild`。Java 使用 `nhcm.advmatch.MatchingExchange` 和 `CompletableFuture`，用 try-with-resources 关闭会话；JAR 需要匹配运行平台和架构。

## 合约价格与结算

```cpp
auto contracts = exchange.contracts();
contracts.bindSpot(perpetual, spot).get();
contracts.refresh(perpetual, timestamp).get();
auto market = contracts.market(perpetual).get();
```

默认绑定同 base/quote 的现货双边盘口；指数取加权均价并剔除过期数据与中位数异常值，标记价限制盘口溢价。资金费为 `premium + clamp(interest - premium)`，再限制到合约上限；默认使用当前溢价，费率单位为百万分之一。

`configureReference` 配置最多 32 个现货 / 外部价格源、权重、单位换算、有效期和自动结算；`publishPrice` 输入外部行情。`configureSchedule` 调整资金费间隔、下次结算时间、上限和期货到期日。Java ContractManager 提供同名接口。

由调用方定时提交 `refresh`：到期自动结算资金费，期货按指数现金交割。延迟调用仅用仍有效的历史参考数据补结算，超过补结算预算或数据失效会拒绝；不会用当前报价补造过去的价格。失败可能发生在价格更新之后，应读取状态再处理。

手动 `updateMark` / `settleFunding` / `deliver` 仍可使用；关闭自动交割后，到期只停止交易。C++ 可继承 `ReferencePriceAlgorithm` 或默认实现，计算后通过 `updateReference` 提交，持久化记录计算结果。完整示例：[合约价格管理](examples/contract-reference/main.cpp)。JNI ABI 为 4，JAR 和原生库需一同更新。

## 止盈止损

通过 `exchange.conditionalOrders()` 创建、查询、修改和取消条件单；`ConditionalOrderRequest` 指定止盈 / 止损、触发价、成交价 / 标记价 / 指数价，以及市价 IOC / FOK 或限价子单。现货只支持成交价，合约支持三种价格源。

```cpp
sdk::ConditionalOrderRequest stop;
stop.quantity = 1;
stop.triggerPrice = 90'000;
stop.timestamp = timestamp;
auto pending = exchange.conditionalOrders().place(user, spot, stop).get();
auto state = pending.snapshot().get();
```

买入止损 / 卖出止盈在 `price >= triggerPrice` 时触发，卖出止损 / 买入止盈在 `price <= triggerPrice` 时触发。已有价格满足条件时立即触发；没有对应价格时等待首次更新。每张条件单只激活一次，`orderId` 指向实际订单。

等待期间不冻结资金；触发时重新风控，失败记录 `Rejected` 和 `rejectReason`。合约平仓传入 `positionId`、`reduceOnly` 和市价保护 `priceLimit`。`ocoOrderId` 关联同用户、同交易对的另一张等待单，任一子单被接受后取消另一张；风控拒绝保留另一张，手动取消只取消指定单。OCO 不保证成交，市价单受流动性和价格保护限制；期货到期会取消未触发单。条件单与触发结果随 WAL / checkpoint 恢复，C++ 和 Java 均使用同一命令接口。

## 手续费与交易规则

```cpp
sdk::TradingRules rules;
rules.priceTick = 5;
rules.quantityStep = 10;
rules.minQuantity = 10;
rules.fees = {100, 500}; // maker / taker，百万分之一
rules.risk.maxActiveOrders = 100;
rules.risk.maxAccountNotional = 1'000'000;
rules.selfTradePrevention = sdk::SelfTradePrevention::CancelTaker;
exchange.rules().configure(symbol, rules).get();
auto risk = exchange.rules().userRisk(symbol, user).get();
auto report = exchange.audit().report().get();
```

规则通过 `rules()` 配置，要求交易对没有挂单或未平仓仓位。支持单笔数量 / 名义金额、单仓与单用户交易对总敞口、活动订单数和杠杆上限；额外限额为 0 表示关闭，原有合约风控仍生效。总敞口采用持仓入场名义金额加待成交开仓单的保守估值，双向仓位不互相抵消。

金额与价格使用整数单位，不使用浮点数。价格和触发价必须是 `priceTick` 的整数倍；数量必须满足 `quantityStep`、`minQuantity` 和最小名义金额。修改订单、条件单激活也执行校验。`PostOnly` 拒绝会立即成交的下单或改价。

现货按实际获得的资产扣费：买方扣 base，卖方扣 quote，逐笔向下取整；合约在 quote 扣费，逐笔向上取整。费率范围 0–100%，暂不支持负费率返佣。`OrderView` / `TradeView` 返回费用；现货费用归入独立金库，可通过 `spotFees` 查询及 `withdrawSpotFees` 转入账户。

自成交以 `UserId` 判断，支持允许、取消 taker、取消 maker、双方取消。为兼容历史日志，默认 `Allow`，新交易对建议显式开启保护；合约仍保留原有的同账户禁止成交约束。FOK 先模拟余额、手续费和自成交处理，失败不成交也不取消 maker。

## 守恒与恢复校验

`audit().report()` 检查各资产外部净流入与账户余额、冻结资金、手续费、保险金、清算及资金费舍入余额之和，并校验订单 / 保证金冻结、盘口聚合和风控缓存。报告包含问题列表和经济状态指纹；总额使用宽整数并以十进制字符串返回。

校验在所属交易线程执行，会遍历并排序保留数据，适合维护和测试；不要每笔交易调用。`persistence.verifyRecovery` 默认开启，WAL / checkpoint 恢复完成后执行校验，异常阻止启动。外部存取款必须通过 `UpdateBalance` 命令，直接修改底层账户会绕过净流入记账。Java 提供同名 Manager 与读取接口。

## 压测

```powershell
.\build\release\advmatch_tps_benchmark.exe --orders 100000 --producers 4 --ring-size 32
.\java\gradlew.bat -p java :advmatch:benchmarkTps -PskipNativeBuild "-PbenchmarkArgs=--orders 100000 --producers 4 --ring-size 32"
```

其他 Java 压测任务：`benchmarkScale`、`benchmarkPerpetual`、`benchmarkLiquidation`、`benchmarkObserve`、`benchmarkBigAllocs`。对应 C++ 程序位于 `build/release/`。
