# AdvMatch 1.4.0

[English](README.md) · 简体中文

C++20 交易引擎，提供 C++ 和 Java SDK，支持**现货、线性交割合约和永续合约**。

## 功能

- 单线程撮合、多线程提交命令与异步结果。
- 账户、余额、冻结资金、订单、成交及不可变快照。
- 全仓 / 逐仓、杠杆、强平、资金费与现金交割。
- 止损、止盈、OCO、手续费、风险限额及自成交保护。
- 原子余额事务、停牌、守恒审计与 WAL 恢复。
- 按资产和交易对精度使用整数金额。

Java SDK 支持共享状态订阅和认证网络传输，多个应用可连接同一个交易所。参见[网络与订阅 API](docs/java-remote.md)。订阅会合并状态更新，断线后自动恢复订阅；交易修改不会自动重发。

[有界读取、请求调度与持久化请求 ID](docs/subscriptions-performance.md)

## 构建

需要 C++20 编译器、**CMake 3.24+**、Ninja 和 Git；Java 接入需要 **JDK 21**。首次构建会下载依赖。

```powershell
.\build.cmd release
```

Linux / macOS 使用 `./build.sh release`。脚本执行构建与测试，release 还会在 `build/release/` 生成 ZIP。

构建 Java SDK：

```powershell
.\java\gradlew.bat -p java :advmatch:build :advmatch:runExample
```

产物为 `java/advmatch/build/libs/advmatch-1.4.0.jar`，包含构建平台的原生库。已有兼容原生库时可加 `-PskipNativeBuild`。

同时构建并测试 Windows 与 Linux amd64，将两个平台原生库打入同一个 Java SDK JAR：

```powershell
.\java\gradlew.bat -p java buildMultiplatform
```

Windows 下使用 WSL Ubuntu，可通过 `-PwslDistribution=名称` 指定发行版。Linux 环境需要 GCC、CMake、Ninja、Git 和无图形界面的 JDK 21+。单独构建 Linux 使用 `bash tools/build_linux.sh`，动态库及原生 SDK ZIP 在 `build/linux-release/`，打包暂存目录为 `build/native-bundles/linux-amd64/`。加载器自动选择 `native/windows-amd64` 或 `native/linux-amd64`。当前 Linux amd64 产物要求 glibc **2.38+**，不支持 musl/Alpine；如需兼容较旧 glibc，请在目标最低版本环境重建。不包含 ARM64 动态库。

## 快速接入

```java
import nhcm.advmatch.MatchingExchange;
import nhcm.advmatch.model.ExchangeConfig;
import nhcm.advmatch.model.PersistenceConfig;

var config = new ExchangeConfig()
    .withPersistence(new PersistenceConfig().withEnabled(false));
try (var exchange = new MatchingExchange(config)) {
    var user = exchange.users().create().join();
    var account = user.spotAccount().join();
    System.out.println(account.id());
}
```

示例使用内存模式。默认开启持久化，正式接入应指定数据目录。实体 Manager 返回 future，`submit()` 提交命令。创建交易对后还需创建订单簿，才能撮合。

[C++ 交易示例](examples/cpp-sdk/main.cpp) · [Java 交易示例](java/advmatch/src/main/nhcm/advmatch/example/SpotExample.java) · [合约参考价格](examples/contract-reference/main.cpp)

安装 C++ SDK：`cmake --install build/release --prefix build/sdk`。外部项目使用 `find_package(AdvMatch CONFIG REQUIRED)` 并链接 `AdvMatch::sdk`；编译器及运行库须兼容，应用需携带 SDK 动态库。

## 接入指南

[中文接入说明](documentation/guide.zh-CN.md) · [English guide](documentation/guide.en.md)

指南包含交易规则、合约价格、条件单、原子转账、恢复与压测。

## 兼容性

Java SDK 与原生库须配套更新。命令使用 schema 2，支持读取 schema 1 并保持历史重放行为。不具备相同命令集的旧引擎无法读取较新的操作；降级需使用完整兼容备份。

`TradingRules.tickAwarePriceBand` 默认关闭。启用后，合约保护带至少覆盖标记价上下各一个价格步长。仅切换此项可保留挂单、持仓和手续费配置。

`contracts().configureProtection(symbol, rate)` 以百万分比设置成交保护：`100000` 表示 10%，`0` 关闭。市价单独立保护价、资金风控与有界强平仍生效。

开发协助：GPT-5.6、GPT-6.1。
