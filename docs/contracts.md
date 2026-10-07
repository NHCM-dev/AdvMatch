# 期货与永续合约

## 本版交易规则

实现报价资产作为抵押与结算货币的线性合约，支持逐仓、全仓、独立多仓与空仓，以及期货到期现金交割。仓位通过 PositionId 定位，不按用户和交易对自动合并；同一用户可以持有多个相同方向的仓位。positionId 为 0 的订单创建一个新仓位，指定已有 PositionId 的同向订单加仓，反向订单平仓且自动成为 reduce-only。禁止反向合约和实物交割。

价格与余额采用整数原子单位，费率采用百万分之一。quantity 表示合约数量，contractMultiplier 表示每份合约包含多少个标的计价单位；price 必须是每个这样的标的计价单位对应的报价资产原子金额。

```
notional = price × quantity × contractMultiplier
initialMargin = ceil(entryNotional / leverage)
longUnrealizedPnL = markNotional - entryNotional
shortUnrealizedPnL = entryNotional - markNotional
maintenanceMargin = ceil(markNotional × maintenanceMarginRate / 1,000,000)
```

默认支持 GTC、IOC、FOK；市价单必须提供 priceLimit。市价买单不能高于该价格成交，市价卖单不能低于该价格成交。订单的执行价格还需处于标记价上下的 priceBandRate 范围内。自成交会取消 taker，不生成成交。

默认杠杆为 10，最大杠杆为 100。这些是示例配置，不是特定交易所的规则。配置会验证维持保证金率、强平费用与最大杠杆的关系。maker/taker 费用单独计入 feeBalance，不与持仓盈亏混用。

## 代码职责

| 类型 | 职责 |
| --- | --- |
| BasicOrderFactory | 通用订单参数与 BasicOrder 创建，现货和合约复用 |
| ContractEngine | 产品登记、命令时序、仓位绑定、杠杆和保证金调整、协调生命周期 |
| ContractRiskEngine | 下单/改单校验、保证金预留计算、执行价格边界 |
| IndexedTreeOrderBook | 价格优先、同价 FIFO 撮合，通过 MatchPolicy 调用合约策略 |
| ContractSettleEngine | 成交前生成双方 PositionPlan；成交后提交余额、仓位、盈亏与费用 |
| ContractSettlementBatch | 资金费率和交割共用的账户净额预检查与批量提交 |
| LiquidateEngine | 判断保证金不足、撤单、通过真实订单簿减仓、管理待强平状态 |
| FundingRateEngine | 校验周期和费率、结算多空资金费用、整数尾差处理 |
| DeliveryEngine | 到期现金交割、释放保证金、结算盈亏、结束交易对生命周期 |
| ContractMath | 集中处理乘法、比例、向上取整与溢出检查 |

新业务路径通过 Result/enum 返回失败，不为余额不足、非法杠杆、价格保护或保险金不足抛异常。内部账本提交使用预检查后的不变量；内存分配失败属于资源故障，不作为普通业务拒单继续运行。

## 全仓与逐仓

逐仓的 Position.initialMargin 对应账户 reserved 中归该仓位使用的真实保证金。平仓损失不能使用该用户其他仓位或可用余额；需要穿仓补差的强平与交割必须先有足够保险金。

全仓的 Position.initialMargin 表示风险要求，实际抵押资产保留在 ContractAccount 的 active 中，成交只扣费用和实现盈亏。账户按抵押资产缓存全部全仓仓位的初始保证金、维持保证金、强平费用和未实现盈亏。转出和订单预留会检查该缓存，因此不需要遍历所有仓位。

全仓强平判断的权益包含全仓挂单已预留的资金。触发时先撤销该抵押资产下全部全仓挂单，再处理同一账户、同一抵押资产下的全仓仓位，跨交易对也包含在内。逐仓仓位和现货账户的资金不会用于补全仓亏损。

引擎管理的仓位不会被 removePosition 删除；尚有仓位或合约挂单的用户不能被移除。Position 对象由账户拥有，OrderNode 与撮合策略借用稳定地址。已关闭仓位保留供查询；活动索引只包含仍有仓位或挂单的对象，标记价和资金费率处理不扫描全部历史仓位。

## 强平

逐仓使用 `positionMargin + unrealizedPnL <= maintenanceMargin + estimatedLiquidationFee` 判定；全仓使用账户缓存的权益与同一抵押资产的总要求判定。价格采用标记价，成交价格采用真实对手订单价格。

标记价更新、资金费率结算、成交后的风险变化都会安排强平。普通下单只处理待强平队列。流动性不足、价格保护未通过或保险金不足时，剩余仓位保留并标记 liquidating，返回 PendingLiquidation，不生成虚假成交，也不把仓位直接清零。后续流动性到来或显式 processLiquidations 会重试。

保险金按产品独立保存，通过 fundInsurance 从真实账户转入。亏损补差会扣减保险金；它不会成为负数。手续费、清算资金与资金费尾差分别记账。

predictLiquidatePrice 和 predictBankruptPrice 是逐仓的参考价格，包含整数取整误差；实际判定始终使用实时权益。全仓参考价格为 0，因为一个仓位的价格不足以表达整个账户的强平边界。

## 永续资金费率与命令时序

外部价格服务输入标记价，外部调度器按产品的 nextFundingTimestamp 提交该期费率。引擎不从成交价自动生成可信标记价，也不把手续费或持仓 PnL 当作资金费率。

正费率由多仓支付、空仓收取，负费率反向；原始金额为 `markNotional × fundingRate / RATE_SCALE`，取整数原子金额。若仓位拆分导致收款总额的取整结果大于付款总额，按累计整数比例收缩收款，避免生成没有付款支撑的余额。付款多于收款的原子尾差留在 fundingRoundingBalance，可审计，不算普通交易手续费。

同一期只能成功结算一次；跳期、不在结算时间点执行、超出 maxFundingRate 的费率会拒绝。资金和仓位变化先完整预检查，再统一提交。失败时该期可以在补足条件后重试，不会重复扣费。

全仓现金不足时，会先释放相关全仓挂单预留再重试。仍不足则返回 InsufficientBalance，不从逐仓保证金或保险金借用资金完成全仓资金费支付。逐仓资金费从该仓位保证金支付；不足部分只能使用已注资的保险金，否则整期拒绝。

达到未结算的资金费时间点后，新下单、会产生成交的改单、标记价推进会返回 FundingRequired/TradingDisabled，保持该期持仓快照。调度器应先结算该期，再推进更晚的交易命令。时间是 Unix 毫秒，命令在同一撮合线程中有序执行。

## 期货到期与现金交割

expiryTimestamp 是交易截止时间。调度器调用 expire；到期后到来的下单或标记价命令也会触发停市与撤单。期货不收永续资金费用。

最终交割价由外部权威价格服务提供，调用 deliver。交割先按实际成交入场金额计算每个仓位盈亏，将同一账户的多个仓位变动净额合并，再预检查所有账户与保险金。成功后平掉仓位、释放逐仓保证金、移除全仓风险要求并进入 Settled，重复交割被拒绝。

保险金不足或余额溢出时，交割不修改仓位与交割资金；产品保持 Expired，可以补足保险金后重试。到期撤销挂单是此前独立执行的动作，不会恢复交易。

clearingBalance 是有符号清算对账项，用于抵消已实现盈亏的现金流；它不是用户可用余额，也不是保险金。审计资金守恒时，将用户 active/reserved、保险金、feeBalance、clearingBalance 和资金费尾差同时纳入。

## 使用入口

```cpp
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"
#include "advmatch/util/Utils.h"

using namespace advmatch;

UserManager users;
SymbolManager symbols;
MatchingEngine matching(users, symbols);

const UserId buyer = users.createUser();
const UserId seller = users.createUser();
const auto* symbol = symbols.createSymbol("BTC-PERP", SymbolType::Perpetual, 1, 2);
const Timestamp now = util::getCurrentTime();

ContractSpec spec;
spec.firstFundingTimestamp = now + spec.fundingInterval;
const auto registered = matching.addContractBook(symbol->symbolId, spec, 100, now);

// 示例入金；实际由资金账户流程调用，SpotAccount 也可先入金再转入。
const auto buyerDeposit = users.getUser(buyer)->getContractAccount().updateBalance(2, 10000);
const auto sellerDeposit = users.getUser(seller)->getContractAccount().updateBalance(2, 10000);

ContractOrderOptions options;
options.marginMode = MarginMode::Cross;
options.leverage = 10;
TradeBuffer trades;

const auto ask = matching.placeContractOrder(seller, symbol->symbolId, OrderSide::Ask,
    OrderType::Limit, TimeInForce::GTC, 100, 10, now, options, trades);
const auto bid = matching.placeContractOrder(buyer, symbol->symbolId, OrderSide::Bid,
    OrderType::Limit, TimeInForce::IOC, 100, 10, now, options, trades);

if (bid)
{
    const Position* position = matching.getContractEngine().queryPosition(bid.positionId);
    // 平仓时设置 options.positionId = bid.positionId，提交反向订单。
}
```

使用现有 SingleThreadConsumer 的子类承接这些命令。生产者只提交命令；账户、标记价、资金费、交割、强平和撮合均在消费线程修改。不要从另一个线程直接读取正在变化的持仓；将值复制到响应或事件队列。

setEventSink 可注册 ContractEventSink，接收成交、资金费、强平和交割事件。成交事件含订单 ID、对手订单 ID、仓位 ID、盈亏、费用和保险金补差。回调在完整成交或批量提交后执行，必须 noexcept，不能重入修改引擎；事件接收者需覆盖注册期间的生命周期。自动强平成交也会发事件，不依赖调用方的 TradeBuffer。

`getContractEngine().reserve(marketCount, positionCount)` 可在预加载前预留市场和仓位索引。ID、账户和仓位查询不复制整个集合。

## 算法与测试

| 操作 | 复杂度 |
| --- | --- |
| OrderId 查询 | 最坏 O(1)，直接数组下标 |
| PositionId、市场 ID、余额、风险缓存查询 | 平均 O(1)，哈希索引；内联余额最多检查 10 项 |
| 单笔成交的仓位、账户与风险更新 | 平均 O(1) |
| 撤单脱链 | O(1)；最后一个价格档的树删除另计 O(log L) |
| 插入价格档、删除价格档 | O(log L) |
| 撮合 | 与实际成交及风险取消的对手订单数量相关；不是固定 O(1) |
| FOK 模拟 | O(F) 次状态更新与访问，按价格档遍历；只复制涉及的账户/仓位 |
| 标记价、资金费、交割 | O(P)，P 为该产品活动仓位数 |
| 全仓强平 | 与该账户有关的活动仓位、撤单和真实成交数量相关 |

没有把哈希的平均 O(1) 宣称为最坏 O(1)，也没有将多订单撮合或整产品结算称为 O(1)。普通订单不会扫描全部历史仓位。

测试入口：

```powershell
cmake --build --preset release
ctest --preset release
cmake --build --preset debug
ctest --preset debug
```

advmatch_contract_tests 覆盖双向独立仓位、加减保证金和杠杆、订单修改/取消、全仓余额转出限制、跨交易对全仓强平、部分强平与流动性不足、保险金补差、资金费正负方向和重复周期、费用/尾差、FOK 原子性、自成交、价格保护、交割失败/重试、溢出、持续开平仓资金守恒、多空数量守恒与稳定地址。现货和原有 TPS/规模 smoke 测试一起运行。

## 规则参考

标记价触发强平和多空资金费方向参考 [Bybit 合约规则](https://www.bybit.com/en/contract-rules) 与 [资金费计算](https://www.bybit.com/en/help-center/article/Funding-fee-calculation)。期货到期支持现金结算的产品形态参考 [CME 到期与结算说明](https://www.cmegroup.com/education/courses/introduction-to-futures/get-to-know-futures-expiration-and-settlement)。本项目的默认费率、价格带、保险金策略与取整规则由 ContractSpec 和上述实现定义。

本版实现上述交易规则，没有包含价格指数采集、自动做市、ADL、分层风险限额、组合保证金、持久化恢复或外部资金托管。标记价、期费率和最终交割价由调用方提供。
