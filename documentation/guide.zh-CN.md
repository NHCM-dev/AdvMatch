# AdvMatch 接入说明

[README](../README.zh-CN.md) · [English](guide.en.md)

## API 入口

| Manager | 职责 |
| --- | --- |
| `assets()`、`users()`、`accounts()` | 资产定义、身份和余额 |
| `symbols()`、`books()`、`orders()` | 交易对、盘口与订单 |
| `positions()`、`contracts()` | 保证金、价格、资金费、强平和交割 |
| `conditionalOrders()` | 止损、止盈和 OCO |
| `rules()`、`trading()`、`audit()` | 手续费、风控、停牌和守恒检查 |

C++ future 使用 `.get()`，Java 使用 `.join()` 或异步串联。查询返回独立快照；下单成功可能仍未成交或仅部分成交。

## 交易规则

按资产及交易对精度使用整数单位。价格和触发价须对齐 `priceTick`；数量须满足 `quantityStep`、`minQuantity` 和最小名义金额。额外风险限额为零表示关闭，原有合约风控仍生效。

通过 `rules().configure(...)` 配置。大多数修改要求没有挂单或持仓；仅切换 `tickAwarePriceBand` 可在有敞口的正常交易合约上执行。先读取当前规则，仅修改目标字段。

现货从实际获得的资产扣费，买方扣 base、卖方扣 quote，每笔向下取整；合约在 quote 扣费，每笔向上取整。费率使用百万分之一，不支持负费率返佣。现货手续费可通过账户命令查询和提取。

自成交保护按交易所 **UserId** 判断，采用新进订单的策略：允许、取消 taker、取消 maker 或双方取消。默认允许；合约另外禁止同账户成交。PostOnly 拒绝立即成交，FOK 先模拟，失败不成交也不撤 maker。

市价需要流动性，合约市价单必须提供正数的独立保护价。合约带外挂单可能在撮合时被撤销。`tickAwarePriceBand` 默认关闭，启用后保护带至少覆盖标记价上下各一档；它不修改标记价，也不要求连续成交涨一档。

通过 `contracts().configureProtection(symbol, rate)` 设置成交保护幅度，单位为百万分比（10% = `100000`，关闭 = `0`，必须小于 `1000000`）。允许在有挂单及持仓时修改。关闭后仍检查资金、订单独立保护价和有界强平价格；预占仍使用标记价上下 10% 的有界估算。

## 合约参考价格

通过 `contracts().bindSpot(...)` 绑定同 base/quote 现货市场，或用 `configureReference(...)` 配置带权重的现货 / 外部来源。`publishPrice(...)` 发布外部价格，调用方定期提交 `refresh(...)`。

刷新会更新参考价格，并处理已配置的资金费和到期交割。必须有有效来源，不会拿当前价格补造历史结算。刷新失败前可能已经更新价格，应先查询状态。见[参考价格示例](../examples/contract-reference/main.cpp)。

## 条件单与停牌

条件单支持成交价、标记价或指数价触发，现货只支持成交价。等待期间不冻结资金，激活时重新风控；查询状态及拒绝原因。OCO 在子单被接受后取消另一张等待单，不保证成交。

全所与交易对停牌相互独立。停牌阻止新单、改单及撮合，仍允许撤单和查询；不会自动撤销已有订单或释放冻结。恢复按最新价格重新检查等待条件单。

## 原子余额与持久化

`BalanceTransaction` 接受 1–256 个不同 `(accountId, assetId)` 变更。`conserve=true` 要求每个资产变动总和为零；`conserve=false` 将外部净流入记入审计，宿主须限制权限。所有腿校验后一起提交。

原子性只覆盖引擎内部，物品交付、数据库及应用日志分别落盘。收到成功回执不代表已刷盘；提交 `Flush`，检查 `enabled`、`faulted`，并确认 `durableThrough` 覆盖回执序号。持久请求 ID 在配置的保留窗口内去重；结果未知时不要盲目重试。

默认启用恢复校验。`audit().report()` 检查余额、冻结及经济守恒，适合维护或测试，不宜每笔交易执行。停止引擎后备份整个持久化目录。Java 与原生版本须配套，schema 2 日志需要新引擎。

## 测试与压测

```powershell
ctest --preset release
.\java\gradlew.bat -p java :advmatch:build -PskipNativeBuild
.\build\release\advmatch_tps_benchmark.exe --orders 100000 --producers 4 --ring-size 32
```

Java 压测任务包含 `benchmarkTps`、`benchmarkScale`、`benchmarkPerpetual`、`benchmarkLiquidation`、`benchmarkObserve` 和 `benchmarkBigAllocs`，使用 `-PbenchmarkArgs=...` 传参。对应 C++ 程序在 `build/release/`。
