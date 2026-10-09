# 性能与 Java 传输基础设施

目标是现货 10M 成交/秒、永续带强平压力 1.5M 成交/秒。当前实现尚未达到这两个目标。实际速度取决于硬件、订单规模、持仓生命周期、保证金模式、持久化和读取索引；以校验通过的重复测试结果为准。

## 当前热路径优化与复测

2026-10-08，Release+IPO、可移植指令集，沿用4 producers、ring32、batch1024与相同订单规模。保存本轮开始时的原生可执行文件和Java JAR，逐场景交替运行前后版本，各5次取中位数；交换先后顺序以减小温度与后台负载漂移。原始报告：`build/performance-complete-retained.json`及同名CSV。Java两版JAR都连接同一份最终原生DLL，Java百分比衡量SDK编码变化；C++百分比比较原生热路径变化。

| 场景 | 对照中位数 | 当前中位数 | 变化 |
|---|---:|---:|---:|
| C++现货 | 3,616,700 | 3,644,250 成交/秒 | +0.8% |
| C++普通永续 | 552,974 | 654,834 成交/秒 | +18.4% |
| C++连锁强平，包含恢复 | 80,427 | 83,883 仓位/秒 | +4.3% |
| C++强平压力下的普通永续成交 | 88,123 | 93,356 成交/秒 | +5.9% |
| Java现货完整API | 1,878,612 | 2,178,574 成交/秒 | +16.0% |
| Java普通永续完整API | 363,450 | 401,986 成交/秒 | +10.6% |

现货范围为2,942,360–3,703,620，普通永续621,559–674,492，混合强平普通成交87,896–93,573；Java现货2,053,374–2,252,492，Java普通永续373,853–513,301。现货与强平场景前后范围重叠，Java普通永续波动也较大，表中的中位数变化不能视为所有负载的保证。现货仍使用1M预挂单，普通永续100K成交；混合场景80K受害仓位、100K控制市场成交，包含冲击、补流动性和强平。现货10M与强平压力永续1.5M目标仍未达到。

### 保留的实现

- **直接仓位索引与紧凑遍历**：`PagedPointerIndex`每页1024个借用指针，两次数组定位；`ContractEngine`用紧凑指针数组遍历，交换删除并更新被移动参与者的槽位。去掉全局仓位索引的重复哈希维护。预留容量会分配索引页，最后一个条目被归档时释放该页；目录仍保留，约每1024个历史ID一个目录指针。插入/增长和批量reserve仍涉及分配，完整遍历随记录数量增长。
- **强平提交上下文**：同一次同步提交复用已解析的市场、订单簿和参与者。工厂、时间戳、风控、订单ID分配、资产预占和实际结算仍通过共享实现。
- **失败重试缓存**：仅为保险金或余额不足的强平保存精确依赖值，逐值比较，不依赖哈希碰撞概率或单个市场revision。覆盖maker/taker仓位、账户余额、全仓汇总、最优maker身份/数量、价格和合约参数。依赖相同则保留取消的IOC历史，跳过重复结算计算、挂接/摘除及零额账本操作；依赖变化立即重新验证。maker不再可执行时仍先处理maker。缓存从对象池分配，归档参与者时回收；普通订单不构造此缓存。
- **订单布局**：BasicOrder的价格、数量、身份和类型集中于前部，历史时间戳集中于后部。订单簿链表指针相邻，标志位与资产ID放在同一块。在当前x64构建中BasicOrder为104字节，OrderNode由224缩至216字节，400万节点的净字段占用减少约32MB；池元数据和索引另计。订单查询快照仍完整拥有时间戳数据。
- **局部SettlementContext**：多笔现货成交在一次settleTrades调用内复用四个固定余额槽，单笔路径直接定位。Account以结构generation校验不透明余额token；余额值更新时重新读取，插入、删除、提升到DenseMap或重排后重新定位。缓存不向OrderNode暴露裸余额地址。
- **Java批次直接编码**：生成的66种命令实现`encodeTo`，直接写入有容量预留的批次缓冲区，回填每条命令长度。去掉每条命令临时WireWriter、独立请求byte[]和中间复制。单条encode()及自定义Command保留兼容入口，后者默认使用原来的byte[]实现。WireWriter以小端VarHandle写入整数，协议版本和字节格式不变。

重试缓存默认开启；原生开发者可在单写线程调用`getSettleEngine().setLiquidationRetryCache(false)`作负载对照。benchmark提供`--retry-cache 0|1`和`liquidation_retry_cache_hits`，大规模场景复用了348,012次失败计算。每份精确依赖记录576字节，仅阻塞仓位按需分配；池保留峰值容量供后续复用，因此应结合实际阻塞比例评估内存。

候选仍通过现有队列和预算调度，风险锁定与全仓账户全部目标订单取消保持原顺序。这一实现保留内部IOC订单ID、取消历史、affectedPositions及回放结果；需要完全停止生成失败IOC的独立等待队列时，应同时定义新的历史与持久化执行版本。当前没有改变该语义。订单与仓位历史仍由开发者显式archiveHistory并确认归档，查询可见性继续遵守原有保留边界。

### 验证与复现

C++全部24项CTest通过；强平对照测试运行12种负载、保险金预过滤和重试缓存的四种组合，共48次，比较订单/仓位/账本最终指纹和有序事件指纹。新增回归覆盖账户余额提升和重排、缓存依赖变化、maker失效优先取消、保险金补充、阻塞强平checkpoint/WAL恢复，以及分页索引与归档槽位更新。Java集成测试和六类benchmark冒烟测试通过，包含直接编码、小端边界值、非对齐写入、动态扩容及旧自定义Command兼容。

```powershell
# 在改动和重新构建之前，保存上一版三个benchmark到一个单独目录。
# Java对照可保存上一版advmatch-0.0.1.jar。
python tools/benchmark_performance.py --rounds 5 --ring-size 32 --java --baseline-dir build/optimization-before --baseline-jar build/optimization-baseline-java.jar --output build/performance-compare.json
.\build\release\advmatch_liquidation_benchmark.exe --users 20000 --positions-per-user 4 --symbols 8 --waves 8 --retry-cache 0
.\build\release\advmatch_tps_benchmark.exe --orders 100 --producers 1 --profile-clock
```

`--baseline-dir`目录中的文件名须与当前benchmark一致；Java对照使用当前nativeDir里的DLL。`--profile-clock`在撮合计时窗口外测试100万次getCurrentTime并打印对象大小，本机一次结果约34.77ns/读取。工厂的未来时间戳校验继续逐订单执行；按批次改用一个时间截点会改变边界校验语义，未将该变化混入性能测试。

实验中的全局仓位双索引和在每笔prepare内构造重试上下文曾让普通永续回落；最终改成紧凑遍历数组、分页直接索引，以及普通成交与强平缓存独立的调用路径，未保留这些较慢的中间方案。中间报告留在build中，最终以performance-complete-retained为准。

## 上一轮算法优化与实测

2026-10-08，在同一台机器重新测量优化前后的 C++ 基线，各运行3次取中位数。Release+IPO、可移植指令集、4 producers、ring32、batch1024，工作负载与下节一致。报告为 `build/performance-algorithm-before.json` 与 `build/performance-algorithm-final.json`，工具同时保存每轮完整输出和 CSV。

| 场景 | 本轮优化前 | 本轮最终结果 | 中位数变化 |
|---|---:|---:|---:|
| C++ 现货 | 3,505,190 | 3,443,760 成交/秒 | -1.8% |
| C++ 普通永续 | 592,734 | 586,273 成交/秒 | -1.1% |
| C++ 连锁强平（包含恢复） | 66,281 | 81,082 仓位/秒 | +22.3% |
| C++ 强平压力下的普通永续成交 | 67,334 | 77,892 成交/秒 | +15.7% |
| Java 现货完整 API | — | 1,929,406 成交/秒 | 未重新测 Java 基线 |
| Java 普通永续完整 API | — | 394,823 成交/秒 | 未重新测 Java 基线 |

现货与普通永续的前后范围重叠，不能把小幅变化当作稳定提升。现货最终范围3,260,130–3,552,620，普通永续534,650–608,270，强平压力普通成交77,188–91,535。混合测试仍是80K受害仓位与100K控制市场普通成交，包含标记价、补流动性和强平时间；没有降低强平预算或改成空资金检查。**10M现货、1.5M强平压力永续目标仍未达到。**

这轮保留的改动：

- UserManager、SymbolManager 用连续 ID 的指针槽直接定位，保留 NodeHashMap 的稳定对象所有权和原来的遍历方式。停用对象仍保留，查找继续拒绝停用 ID。额外索引约为每个历史 ID 一个指针，不随对象地址变化。
- ContractMarket 在标记价或价格带参数变化时计算强平限价；强平候选直接复用，避免每次尝试重新做宽整数除法。卖出限价仍向下取整，执行价格带下沿仍向上取整。
- ContractSettleEngine 先复用已算出的平仓损益、释放保证金和余额，检查保险资金是否能覆盖缺口；不足时停止后续剩余仓位估值、汇总和余额更新准备。maker 校验仍先执行，不改变取消顺序；订单编号和历史订单仍正常生成。批量交割保持原来的校验路径。
- 合约已有仓位/挂单的索引不再重复覆盖相同指针。首笔挂单、归零、撤单和归档仍维护原有索引。
- 无符号乘法溢出检测在 GCC/Clang 上使用编译器内建检查，其他编译器保留原算法；OrderNode 构造时减少一次 BasicOrder 值传参复制。

曾试用按仓位缓存保险拒绝结果与 maker 结算方案，交替测量反而变慢，已移除。常规余额宽整数检查也予以保留，没有为提高数字删除溢出或资金校验。

验证：24/24 CTest 通过，Java integrationTest、六类 benchmark smoke 和打包通过。新增强平差分测试在开关预检查两侧比对逐用户余额、全仓汇总、所有仓位/订单及有序成交事件的指纹，同时审计现金和净仓位守恒；覆盖涨跌、全仓/逐仓、radix/tree、手续费、限额、流动性耗尽与保险注资恢复。事件指纹默认关闭，不计入常规吞吐测试。

```powershell
cmake --build --preset release
ctest --preset release --output-on-failure
python tools/benchmark_performance.py --rounds 3 --ring-size 32 --java --output build/performance-algorithm-final.json
.\build\release\advmatch_liquidation_benchmark.exe --users 20000 --positions-per-user 4 --symbols 8 --waves 8 --trades-per-wave 12500 --insurance-prefilter 0
.\build\release\advmatch_liquidation_benchmark.exe --users 20000 --positions-per-user 4 --symbols 8 --waves 8 --trades-per-wave 12500 --insurance-prefilter 1
```

`--insurance-prefilter` 用于同一二进制的算法对照，默认1；`--audit-fingerprint 1` 开启有序事件审计；`--fee-rate N` 设置此压力 fixture 的 maker/taker/强平费率（ppm，默认0，须小于5000）。使用 Java 重复测量前先打包最新原生库，如下文构建示例所示。

额外队列测量（1M成交，各3轮，BusySpin，batch1024）：1 producer/ring4096/publish256 为3,596,370；1 producer/ring65536/publish1024 为3,555,110；4 producers/ring4096/publish128 为2,290,920。原始报告 `build/performance-algorithm-queue.json`。增加 ring 或发布批次没有让吞吐成倍增长，持续自旋和 producer 争用反而可能降低性能；生产默认等待策略保持不变。

## 上一轮实测

Ryzen 7 7435HS（8核16线程），Windows x64，GCC 16 UCRT64，JDK 21；Release+IPO，未启用本机专用指令。2026-10-08，同一配置重复3次取中位数，原始报告为 `build/performance-final.json` 和 CSV。现货1M成交、永续100K成交；4 producers、ring32、逻辑 batch1024，C++ 默认 publishBatch=8，Java publishBatch=1024。普通现货/永续的计时区间不包括预挂单与最终审计。

| 场景 | 中位数 | 三次范围 |
|---|---:|---:|
| C++ 现货，Blocking | 3,411,240 成交/秒 | 3,094,340–3,909,320 |
| C++ 正常永续，Blocking | 617,081 成交/秒 | 596,228–621,499 |
| Java 现货完整 API | 1,866,390 成交/秒 | 1,604,395–1,875,402 |
| Java 正常永续完整 API | 385,077 成交/秒 | 339,673–418,159 |
| C++ 连锁强平（包括恢复） | 62,936 仓位/秒 | 60,416–67,459 |
| C++ 永续普通成交与连锁强平混合 | 67,989 成交/秒 | 66,848–75,116 |

混合场景：20K受害用户、每用户4仓位、8受害市场、8轮冲击，每轮12,500笔控制市场普通成交。它包括 stress 阶段的标记价、强平、补流动性与100K普通成交；每笔普通成交计入两次实际下单，不把强平仓位数计入普通成交数。正常永续每笔 maker/taker 各创建新仓位，杠杆10、逐仓，maker/taker费率100/500（单位1M）。这些结果不代表开启同步 WAL、扩大到4M挂单状态或长时间24/7运行的速度。

上一轮另测配置（1 producer、ring32、batch1024、BusySpin）现货中位数3,720,490，正常永续572,127。该报告中的混合普通成交71,891仍使用固定的单调用者、ring32、Blocking队列，不受该脚本的 producer/BusySpin 参数影响；原始报告 `build/performance-final-spin.json`。改变等待策略和生产线程数不能保证所有产品同时提升，开发者应以自己的规模和延迟目标选择参数。

## Java 批量命令

`Exchange.submitBatch` 接收同一结果类型的命令列表，返回每条命令的 `CompletableFuture<T>`。每批最多 1024 条，一次 JNI 调用、一个原生队列任务；不再为绑定层额外创建 C++ CompletableFuture。单条 `submit`、Manager 和对象包装继续可用。

```java
var config = new ExchangeConfig().withPersistence(new PersistenceConfig().withEnabled(false));
var transport = new TransportOptions(1024, 64, 1_000, 4096);
try (var exchange = new MatchingExchange(config, ForkJoinPool.commonPool(), transport))
{
    List<Command<CreationResult>> commands = List.of(new CreateUser(), new CreateUser());
    var futures = exchange.submitBatch(commands);
    CompletableFuture.allOf(futures.toArray(CompletableFuture[]::new)).join();
    for (var future : futures)
    {
        System.out.println(future.join().id());
    }
}
```

例子需要 `nhcm.advmatch`、`nhcm.advmatch.command.Command`、`nhcm.advmatch.command.management.CreateUser`、`nhcm.advmatch.model.*` 和标准库相关 import。关闭持久化仅适合无需崩溃恢复的场景；需要恢复时保留原有 PersistenceConfig。

批内执行顺序固定，批次不是事务：业务拒绝或取消只影响对应命令。编码/协议校验失败时整批不会执行；进入队列后每条命令分别返回结果。跨 producer 的顺序由队列接纳顺序决定。批量长度也会影响其他命令的等待时间，生产环境应结合 p99 延迟调整，不能仅看 TPS。

### 完成、取消与关闭

- 原生维护有界请求表与侵入式完成队列。响应线程一次取走多条完成结果，原生 ticket 在成功生成响应包后释放。
- Java 每个完成批次只安排一个回调任务。SDK 生成的命令直接读取不可变批次缓冲区切片，不复制每条响应；自定义 Command 可实现 `decode(byte[], offset, size)`，默认实现保持复制兼容。
- 没有结果时先短暂自旋，再在原生条件变量等待。生产线程无需等待响应线程持有 Java admission 锁；Windows 不再依赖微秒级 Java park 的实际计时精度。
- 原始 submit/submitBatch Future 的 cancel 只有在原生命令尚未开始时成功。取消完成结果提前读取也不会再次发布。对象接口组合出来的 Future 保留 CompletableFuture 标准组合语义。
- closeAsync 停止接纳并排空原生已接纳命令；它不取消已开始命令。回调必须使用异步 executor，且应在 Exchange 关闭后再关闭 executor。普通非 Async continuation 会占用回调工作线程，重任务应使用 thenApplyAsync 等接口。

### 两层限额

`ExchangeConfig.maxPendingRequests` 限制原生请求数；`TransportOptions.maxOutstanding` 限制 Java 已提交而响应解码任务尚未开始的数量。回调暂时停滞时，原生 ticket 可回收，但 Java 内存仍受自己的上限约束。

`pendingRequests()` 是原生结果尚未 drain 的数量，`outstandingRequests()` 还包含等待回调的响应。后者在解码任务开始前释放容量，不计入正在执行的用户 continuation。`responseBatchSize` 是一次 drain 的条数，`spinCount` 控制等待前的自旋次数，`activeParkNanos` 用于关闭阶段轮询。

公开 API 满载时返回错误码 -3 的失败 Future，不隐式无限排队。只有 -3 表示命令未执行、可以安全重试；业务拒绝与未知错误不能当成背压重试。BenchmarkContext 的 BackpressureSubmitter 提供有界许可、仅重试未执行项、超时和统计；它用于测试，不改变 SDK 语义。

批次占一个原生 ring entry，批内请求仍分别计入上述限额。批长超过可用请求限额会整批拒绝，增加 ring-size 并不能解除这个限制。

## C++ 热路径

- 具体值类型 API 命令内联存储于 CommandTask，去掉额外 shared_ptr 命令分配。
- WAL 关闭且 requestId=0 时，固定长度内存命令由集中验证器检查枚举，不再 encode/decode 往返。WAL、幂等请求和可变长度 payload 保留原有编码、长度校验与回放流程。
- BinaryCodec 按小端批量写入整数并预留缓冲区，线格式保持不变。读取保留边界检查，并用 memcpy 避免未对齐访问。
- 常见价格乘数量使用带溢出检查的 64 位乘法。合约除法在可证明数值适配时走 64 位路径，较大或负数值保留宽整数算法和原来舍入规则。
- BasicOrder 热 getters/fill 内联；成交、风控、余额和手续费检查仍执行。
- 合约市场以 SymbolId 直接索引稳定节点地址；创建仓位时获取新节点，避免再次查找。逐仓结算跳过不发生变化的全仓汇总计算。
- 强平候选为空时直接返回；合约 benchmark 在准备阶段预留仓位池、账户与市场索引容量，避免计时区间扩容。

这些接口仍要求单写线程。缓存指针的所有者继续拥有对象，删除/归档须走现有生命周期规则。这里没有把变长遍历或成交多笔的实际工作宣称为 O(1)。

C++ 开发者可在准备阶段调用 `AccountManager::reservePositions(total)` 预分配全局仓位池，`ContractAccount::reservePositions(perAccount)` 预留账户索引，`ContractEngine::reserveMarket(symbolId, positions, openOrderParticipants)` 预留市场索引。reserve 调用属于状态修改，也应在单写线程或引擎启动前执行；它不会改变已有 Position/ContractParticipant 的地址。

### 构建与等待策略

```powershell
cmake --preset performance
cmake --build --preset performance --parallel
ctest --preset performance
.\java\gradlew.bat -p java :advmatch:build -PskipNativeBuild -PnativeDir=D:/CppWorks/AdvMatch/build/performance
```

performance preset 开启 Release IPO/LTO，保持可移植指令集。也可在现有 release 目录配置 `cmake --preset release -DADVMATCH_ENABLE_IPO=ON`。`ADVMATCH_NATIVE_ARCH=ON` 是单独的本机指令选项，产物不应直接部署到不同 CPU；默认关闭。

C++ TPS/perpetual benchmark 增加 `--busy-spin`，持续自旋减少唤醒成本，会持续占用一个核心。默认仍使用 BlockingWaitStrategy。可继承 `SingleThreadConsumer<Command, Response, ConfigurableWaitStrategy>`，在 start 前设置 `waitStrategyConfig().busySpin=true`。配置只允许启动前修改。Java Exchange 的撮合队列目前仍为阻塞策略，不能把 Java 响应线程自旋参数当作撮合线程等待策略。

```powershell
.\build\performance\advmatch_tps_benchmark.exe --orders 1000000 --producers 1 --batch-size 1024 --publish-batch 256 --ring-size 4096 --busy-spin
.\build\performance\advmatch_perpetual_benchmark.exe --orders 100000 --producers 1 --batch-size 1024 --publish-batch 256 --ring-size 4096 --busy-spin
```

### 重复测量

```powershell
python tools/benchmark_performance.py --build-dir build/performance --rounds 3 --java
python tools/benchmark_performance.py --build-dir build/performance --rounds 3 --producers 1 --busy-spin --output build/performance-spin.json
python tools/benchmark_performance.py --build-dir build/performance --orders 1000000 --rounds 3 --sweep --output build/performance-sweep.json
```

工具串行运行测试，保存命令、构建选项、每次完整输出、median/min/max，以及 CSV。加入 --java 前需构建 JAR。扫描 producer=1/2/4/8、ring=32/1024/4096、batch=256/1024；请在无并行构建的同一机器上测量，保留原始报告。

现货统计实际成交数并验证最终余额；永续每笔创建独立 maker/taker Position，校验保证金、手续费、资金费和现金守恒。强平测试是连锁强平仓位处理速度，单位为仓位/秒，不能当作“带强平的订单成交 TPS”。达到混合压力目标还需要同一计时窗口的普通订单、标记价和强平工作负载验证。

混合测试使用 `liquidation_benchmark --trades-per-wave N`，Java LiquidationBenchmark 支持同名参数。在同一个引擎单写线程上交错执行标记价冲击、补流动性/强平和普通成交，每笔普通成交实际提交买卖两单、分别创建仓位，并校验全系统现金和净仓位。普通成交使用一个额外控制交易对，避免流动性被受害仓位抢占；这里测的是跨交易对共享线程的强平压力，并非普通订单与强平单争抢同一盘口。混合普通成交 TPS 的分母包含整个 stress 阶段，强平仓位/秒另外报告。C++ 将每波普通订单作为一项批量队列任务，Java 采用真实 submitBatch，两者调度粒度不同。

```powershell
.\build\performance\advmatch_liquidation_benchmark.exe --users 20000 --positions-per-user 4 --symbols 8 --waves 8 --trades-per-wave 12500
.\java\gradlew.bat -p java :advmatch:benchmarkLiquidation -PskipNativeBuild "-PbenchmarkArgs=--users 20000 --positions-per-user 4 --symbols 8 --waves 8 --trades-per-wave 12500"
```

重复测量工具默认包括 C++ 混合场景：80K 受害仓位、100K 普通成交；`--java` 另外测 Java 现货和正常永续。Java 大规模混合场景可用上面的独立任务执行。

C++ 轻量 ResponseSlot 与 Java 完整 Exchange/DTO/Future 路径不同；后者还包含输入校验、观察索引维护、编码、JNI、响应分发与解码。比较时保留 workload、批次、保证金模式、索引与持久化参数，不能把整个差距归因于语言。

后续优先测量大规模留存订单/仓位的内存带宽、合约参与者索引维护、强平流动性不足下的重复尝试，以及单写队列的调度成本。只有确认单核热路径已经优化到位，再评估跨交易对分片；全仓账户跨片资金约束需要明确的一致性协议。
