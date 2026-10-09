# Java 接入

Java 项目位于 `java/advmatch/src/main/nhcm/advmatch`，使用 Gradle。支持现有 Exchange 的全部 25 种交易命令、31 种查询、批量读取、缓存槽、持久化状态/刷新/检查点/出站历史。业务规则与 C++ Exchange 一致。

## 构建与运行

Java 需要 JDK 21、CMake 3.20+、Ninja 和支持 C++20 的编译器。仓库提供 Gradle 9.0.0 Wrapper，第一次使用会下载 Gradle；不需要另外安装 Gradle。项目自身没有 Maven 运行依赖。

```powershell
cd java
.\gradlew.bat :advmatch:build :advmatch:runExample
```

Gradle 默认在仓库根目录使用 release preset 构建原生 JNI 库，按指定的 `src/main` 源码目录编译、执行跨语言集成测试，并将当前平台的原生库装入 JAR。Java 成品在 `java/advmatch/build/libs/advmatch-0.0.1.jar`。

```powershell
# 使用已经构建好的原生库；nativeDir 可指向安装目录里的 bin。
.\gradlew.bat :advmatch:build -PskipNativeBuild -PnativeDir=D:/CppWorks/AdvMatch/build/release
```

Java 默认从 JAR 中解压并加载原生库。运行时可以设置 `ADVMATCH_NATIVE_DIR` 或 `-Dadvmatch.nativeDir=绝对路径`，指定目录需包含 `advmatch` 和 `advmatch_jni`。JAR 只携带构建平台的库，换 OS 或 CPU 架构需要对应平台重新构建；不要把 Windows x64 库加载到 ARM JVM。

## Java 使用

### 创建结果与对象接口

`CreateAsset`、`CreateUser`、`CreateSymbol` 现在返回 `CreationResult`：通过 `id()` / `handle()` 访问。这两个值都是创建实体的 ID；失败时为 0。`receipt()` 保留原来的完整执行结果、持久化序号和重复请求信息。通用结果代码可使用 `Receipt`。

推荐通过 `MatchingExchange` 的 Manager 创建和访问对象；原来的 `Exchange.submit` 和全部命令仍然可用。下面的对象代码与 C++ 共用同一套业务规则：

```java
try (var exchange = new MatchingExchange(config))
{
    var btc = exchange.assets().create("Bitcoin", "BTC", 0).join();
    var usd = exchange.assets().create("Dollar", "USD", 0).join();
    var user = exchange.users().create().join();
    var account = user.spotAccount().join();
    account.updateBalance(usd, 10000).join();
    var symbol = exchange.symbols().create("BTCUSD", SymbolType.Spot, btc, usd).join();
    var book = exchange.books().addSpot(symbol, OrderBookImplementation.Radix).join();
    var order = book.place(user, OrderSide.Bid, OrderType.Limit, TimeInForce.GTC, 100, 10, 1).join();
    System.out.println(order.id());
    order.modifyPrice(99, 2).join();
    order.cancel(3).join();
}
```

对象 API 按 `object`、`manager` 分包，抽象基类分别为 `Entity<T>`、`Manager`。提供 Asset、User、Symbol、Account、SpotAccount、ContractAccount、Order、Position、OrderBook，以及对应 Manager、MatchingEngine 和 ContractManager。Manager 的 `get` 只构造轻量引用，`find` 会通过读取验证对象是否存在；`list` 返回带分页状态和游标的不可变 DTO。AccountManager.find 会读取实际账户类型，选择现货或合约账户包装。

- User 的现货与合约账户 ID 从真实 UserView 获得；不使用 userId 算术推导 AccountId。账户余额操作和相互转账仍通过命令执行。
- Order 提供 snapshot、modifyPrice、modifyQuantity、cancel、position；现货订单的 position 返回 null。Position 提供 snapshot、setLeverage、addMargin、liquidate。ContractManager 提供标记价、资金费率、保险基金、强平预算/处理、到期与交割。
- 每个包装都持有 Exchange 和实体 ID，不持有 User*/OrderNode* 等原生地址。跨 Exchange 传入对象会被拒绝；ID 不会因容器 rehash 失效。删除、归档或关闭后的访问经过原生检查，读取的 DTO 不受后续状态变化影响。包装不是实体的所有者；丢弃包装不会删除原生用户/订单。
- 对象层的创建/修改操作将业务拒绝转成 `BusinessException`，包含 Receipt；单对象读取失败包含 ReadPage。列表读取仍应检查 page.state()。需要自行处理全部业务状态时直接提交原始命令。
- Order/Position 的部分操作先读取不可变归属信息，再提交实际命令，因此比已知 ID 的原始命令多一次查询。Java 对象接口由 thenApply/thenCompose 组合；取消这个组合 Future 不保证撤销底层命令，严格取消语义使用原始 submit Future。

Java 六类 benchmark 的参数和测量范围见 [Java benchmark](java-benchmarks.md)。

高吞吐提交支持 Java `Exchange.submitBatch`，每条命令保留独立的 typed Future。绑定层使用批量完成队列与原生唤醒，不再逐个扫描全部请求。`TransportOptions` 配置响应批长和 Java outstanding 上限，详见 [性能与传输](performance.md)。新 JAR、advmatch 与 advmatch_jni 必须来自同一次构建。

```java
import nhcm.advmatch.Exchange;
import nhcm.advmatch.command.management.CreateUser;
import nhcm.advmatch.command.read.ReadUser;
import nhcm.advmatch.model.*;

var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(false));
try (var exchange = new Exchange(config))
{
    var user = exchange.submit(new CreateUser()).thenCompose(receipt ->
    {
        if (!receipt.result().accepted())
        {
            throw new IllegalStateException("CreateUser rejected");
        }

        return exchange.submit(new ReadUser(new ObservationQuery().withUserId(receipt.result().entityId())));
    });
    System.out.println(user.join().values().getFirst().spotAccountId());
}
```

`submit(Command<T>)` 返回真正的 `CompletableFuture<T>`，支持 thenApply、thenCompose、exceptionally、allOf 等标准 Java 操作。Exchange 可配置完成回调 Executor，默认 commonPool。自定义 Executor 必须异步排队，不能 inline 执行。Future 已完成时，后挂的非 Async continuation 也可能直接在调用线程执行；需要固定异步执行时用 thenApplyAsync/thenRunAsync 并指定 Executor。get/join 只能用在可以等待的调用线程；不要在有界回调池里互相等待后续回调。

查询/配置是不可变 record，通过 `withUserId`、`withSymbolId`、`withLimit`、`withCursor` 等方法生成新值。带 requestId 的交易命令有最后一个 requestId 参数，不带时为 0。包括 uint64 的 Java long 保留原始 64 位，不把高位删除；大于 Long.MAX_VALUE 的值用 Long 的 unsigned 方法或 BigInteger 转换处理。uint32 用 long，uint16/uint8 用 int，编码时检查范围。余额与余额变动仍是有符号 long，价格/数量/ID/纯正额按 unsigned 语义解释。价格、数量、手续费率的定点单位沿用原引擎，禁止以浮点替代。

## 返回与生命周期

- 业务拒绝通过 CommandReceipt.result() 的 accepted/state 返回；不要把 Future 成功完成当成订单必定被接受。state 的枚举由 type 决定，例如 PlaceOrderState、BalanceOpResult、ContractState。
- 原生错误、无效包、满队列、会话关闭通过 exceptional Future 返回 NativeException。查询 NotFound / InvalidQuery / CursorExpired 等属于 ReadPage.state，数据可能为空；读取前先检查状态。
- requestId=0 不去重；非零 requestId 规则与 C++ 持久化一致，要求全局执行顺序单调递增。同一 ID 和同一数据可重试，不能在多个生产者分配 ID 后假定入队顺序一样。
- Java cancel(false) 只会取消尚未开始的原生命令。已开始的撮合/结算不能中断或回滚；取消失败会继续返回实际结果。等待超时不会撤销交易。不要手动 complete/obtrude API 返回的 Future；超时等待用 get(timeout)，修改 Future 状态不是修改引擎状态。
- closeAsync 关闭准入、排空原生命令，接收所有剩余响应后释放会话。重复关闭共享同一完成结果。Java AutoCloseable.close 等待关闭；显式异步关闭可观察 I/O 错误。关闭不取消先前接受的操作，也不等待用户 continuation 全部运行完。
- maxPendingRequests 默认 4096，限制尚未释放的原生请求句柄；maxRequestBytes 默认 4 MiB。C++ RingBuffer 仍有独立背压。不要无限重试满队列，也不要没有读取/释放响应就持续提交。
- 查询 DTO 完全拥有数据，不借用 C++ 容器地址。Cache Slot 通过 Exchange 创建、显式 refresh 后 load；关闭释放 native slot，既有 DTO 可保留。Exchange 必须显式关闭，后台响应线程不是 GC 自动释放的替代品。
- 持久化默认开启并自动恢复，目录默认 data/advmatch；使用独立实例应配置不同目录。恢复后先查询数据，不要重复初始化。后台资金费/标记价/交割/强平调度依旧由调用方提交命令。

## 原生边界与维护

`include/advmatch/advmatch.h` 提供 ABI 2 的 C 导出；JNI_OnLoad 通过 RegisterNatives 注册 nhcm.advmatch.internal.NativeBindings，并兼容 org.advmatch.AdvMatch 的旧 ABI 示例。Java 的 native 方法没有 Java_* 命名导出。

请求使用 binding schema 1 的小端二进制协议；交易部分复用现有 CommandCodec，WAL 格式未改变。原生 API 返回校验过的递增 ID，拒绝已释放/错误句柄，不让调用方持有对象地址。所有 C 导出捕获 C++ 异常。poll/slot_load 返回的 buffer 由 advmatch_buffer_free 释放；JNI 在复制后立即释放，关闭时再释放会话。原生请求句柄限额与队列限额分别生效。

C ABI 状态 -3 专门表示 QueueFull（包含原生队列和请求句柄限额），该命令未执行；Java 通过 NativeException.status() == NativeException.QUEUE_FULL 判断。其他异常或等待超时不代表命令未执行，不能据此盲目重试。Java benchmark 自带有界背压提交器，普通 Java submit 仍直接报告满队列。

DTO、枚举、交易命令、查询命令和 codec 已生成并提交在源码中，正常构建不需要 Python：

```powershell
python tools/generate_bindings.py
python tools/generate_bindings.py --check
python tools/generate_object_api.py
python tools/generate_object_api.py --check
```

修改 C++ DTO 字段或枚举后重新生成并运行跨语言测试。字段次序或编码含义变更需要升级 binding schema，同时考虑已有持久化命令格式的兼容迁移。生成器只支持当前的值类型语法，不是通用 C++ 解析器。

跨语言接口额外承担包编码、JNI、内存复制、DTO 分配和响应完成通知，不承诺与原始 ResponseSlot 路径相同的 TPS/延迟。大规模历史扫描和全量导出仍有原有成本；API 不跨语言暴露自定义 C++ RiskEngine/PersistenceStore 对象，自定义业务组合放在 Java 命令调用层，扩展原生算法使用 C++ 接口。

构建与异步语义参考：[Gradle Java SourceSets](https://docs.gradle.org/current/userguide/java_plugin.html)、[Gradle Wrapper](https://docs.gradle.org/current/userguide/gradle_wrapper.html)。
