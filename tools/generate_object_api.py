"""Generate the Java object facade from one operation table.

The facade only submits public commands. Handles are Exchange-scoped IDs;
there are no native pointers or duplicated matching/risk/settlement rules.
Run with --check to verify checked-in generated sources.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "java/advmatch/src/main/nhcm/advmatch"

outputs = {}


def emit(path, text):
    outputs[path] = text.strip() + "\n"


emit(JAVA / "object/Entity.java", """
package nhcm.advmatch.object;

import nhcm.advmatch.Exchange;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;

// An owner + ID reference, not a C++ pointer. Reads return immutable snapshots.
// Removing/archiving an entity makes later operations fail in the native engine.
public abstract class Entity<T>
{
    protected final Exchange exchange;
    private final long id;

    protected Entity(Exchange exchange, long id)
    {
        this.exchange = Objects.requireNonNull(exchange);
        if (id == 0)
        {
            throw new IllegalArgumentException("Entity ID must be nonzero");
        }

        this.id = id;
    }

    public final long id()
    {
        return this.id;
    }

    public final long handle()
    {
        return this.id;
    }

    public final void requireOwner(Exchange owner)
    {
        if (this.exchange != owner)
        {
            throw new IllegalArgumentException("Handle belongs to another Exchange");
        }
    }

    public abstract CompletableFuture<T> snapshot();

    @Override
    public final boolean equals(Object value)
    {
        return value != null && value.getClass() == this.getClass() && ((Entity<?>) value).exchange == this.exchange && ((Entity<?>) value).id == this.id;
    }

    @Override
    public final int hashCode()
    {
        return Objects.hash(System.identityHashCode(this.exchange), this.getClass(), this.id);
    }
}
""")

emit(JAVA / "manager/Manager.java", """
package nhcm.advmatch.manager;

import nhcm.advmatch.Exchange;
import java.util.Objects;

public abstract class Manager
{
    protected final Exchange exchange;

    protected Manager(Exchange exchange)
    {
        this.exchange = Objects.requireNonNull(exchange);
    }
}
""")

emit(JAVA / "BusinessException.java", """
package nhcm.advmatch;

import nhcm.advmatch.model.*;

// Raw commands return business results; the object facade rejects failed calls.
public final class BusinessException extends RuntimeException
{
    private final CommandReceipt receipt;
    private final ReadPage page;

    public BusinessException(CommandReceipt receipt)
    {
        super("Command rejected: " + receipt.result().type() + " state=" + receipt.result().state());
        this.receipt = receipt;
        this.page = null;
    }

    public BusinessException(ReadPage page)
    {
        super("Read failed: " + page.state());
        this.receipt = null;
        this.page = page;
    }

    public CommandReceipt receipt()
    {
        return this.receipt;
    }

    public ReadPage page()
    {
        return this.page;
    }
}
""")

emit(JAVA / "internal/ApiResults.java", """
package nhcm.advmatch.internal;

import nhcm.advmatch.BusinessException;
import nhcm.advmatch.model.*;

public final class ApiResults
{
    private ApiResults()
    {
    }

    public static CommandReceipt accepted(Receipt result)
    {
        if (!result.result().accepted())
        {
            throw new BusinessException(result.receipt());
        }

        return result.receipt();
    }

    public static long created(CreationResult result)
    {
        accepted(result);
        if (result.id() == 0)
        {
            throw new IllegalStateException("Accepted creation has no ID");
        }

        return result.id();
    }

    public static <T> T one(ReadResponse<T> response)
    {
        if (response.page().state() != ReadState.Success || response.values().size() != 1)
        {
            throw new BusinessException(response.page());
        }

        return response.values().getFirst();
    }
}
""")


# Each method contains its Java declaration and body.
def method(jdecl, jbody):
    return jdecl, jbody


def query(java):
    return "new ObservationQuery()." + java


def snapshot(read, jq, view):
    return method(f"CompletableFuture<{view}> snapshot()", f"return this.exchange.submit(new {read}({jq})).thenApply(ApiResults::one);")


def checked(jdecl, command):
    return method("CompletableFuture<CommandReceipt> " + jdecl, f"return this.exchange.submit({command}).thenApply(ApiResults::accepted);")


def readpage(name, read, view, jp, jq):
    return method(f"CompletableFuture<ReadResponse<{view}>> {name}({jp})", f"return this.exchange.submit(new {read}({jq}));")


def output_class(name, package, base, methods, jextra="", jctor=""):
    jimports = "\n".join("import nhcm.advmatch.command." + group + ".*;" for group in ["account", "contract", "management", "order", "read"])

    jlines = ["// Generated by tools/generate_object_api.py.", f"package nhcm.advmatch.{package};", "", "import nhcm.advmatch.*;", "import nhcm.advmatch.object.*;", "import nhcm.advmatch.model.*;", "import nhcm.advmatch.internal.ApiResults;", jimports, "import java.util.concurrent.CompletableFuture;", "", f"public {'abstract' if name == 'Account' else 'final'} class {name} extends {base}", "{"]

    params = "Exchange exchange" + (", long id" if package == "object" else "")

    parentargs = "exchange" + (", id" if package == "object" else "")
    if jextra:
        jlines += [jextra, ""]

    jlines += [f"    {'protected' if name == 'Account' else 'public'} {name}({params})", "    {", f"        super({parentargs});"]

    if jctor:
        jlines += ["        " + jctor]

    jlines += ["    }"]

    for jd, jb in methods:
        jlines += ["", "    public " + jd, "    {"] + ["        " + line if line else "" for line in jb.splitlines()] + ["    }"]

    jlines += ["}"]

    emit(JAVA / package / (name + ".java"), "\n".join(jlines))


for name, view, field in [("Asset", "AssetView", "assetId"), ("Symbol", "SymbolView", "symbolId"), ("User", "UserView", "userId"), ("Account", "AccountView", "accountId"), ("Order", "OrderView", "orderId"), ("Position", "PositionView", "positionId"), ("OrderBook", "BookView", "symbolId")]:
    jq = query(f"with{field[0].upper() + field[1:]}(this.id())")
    methods = [snapshot("ReadBook" if name == "OrderBook" else "Read" + name, jq, view)]
    if name in ("Asset", "Symbol", "User"):
        methods.append(checked("remove()", f"new Remove{name}(this.id())"))
    if name == "User":
        methods += [method("CompletableFuture<SpotAccount> spotAccount()", "return this.snapshot().thenApply(value -> new SpotAccount(this.exchange, value.spotAccountId()));"), method("CompletableFuture<ContractAccount> contractAccount()", "return this.snapshot().thenApply(value -> new ContractAccount(this.exchange, value.contractAccountId()));"), readpage("orders", "ReadUserOpenOrders", "OrderView", "ObservationQuery query", "query.withUserId(this.id())"), readpage("positions", "ReadPositions", "PositionView", "ObservationQuery query", "query.withUserId(this.id())")]
    if name == "Symbol":
        methods.append(method("OrderBook book()", "return this.exchange.books().get(this.id());"))
    if name == "Account":
        methods += [checked("updateBalance(Asset asset, long change)", "new UpdateBalance(this.id(), asset.id(), change)"), checked("transferTo(Account target, Asset asset, long amount)", "new Transfer(this.id(), target.id(), asset.id(), amount)"), readpage("balances", "ReadBalances", "BalanceView", "ObservationQuery query", "query.withAccountId(this.id())")]
        # Validate every passed handle before submitting IDs.
        m = methods[-3]
        methods[-3] = (m[0], "asset.requireOwner(this.exchange);\n" + m[1])
        m = methods[-2]
        methods[-2] = (m[0], "target.requireOwner(this.exchange);\nasset.requireOwner(this.exchange);\n" + m[1])
    if name == "Order":
        methods.append(method("CompletableFuture<Position> position()", "return this.snapshot().thenApply(value -> value.positionId() == 0 ? null : this.exchange.positions().get(value.positionId()));"))
        for jd, expr in [("cancel(long timestamp)", "new CancelOrder(value.userId(), this.id(), timestamp)"), ("modifyPrice(long price, long timestamp)", "new ModifyPrice(value.userId(), this.id(), price, timestamp)"), ("modifyQuantity(long change, long timestamp)", "new ModifyQuantity(value.userId(), this.id(), change, timestamp)")]:
            methods.append(method("CompletableFuture<CommandReceipt> " + jd, f"return this.snapshot().thenCompose(value -> this.exchange.submit({expr})).thenApply(ApiResults::accepted);"))
    if name == "Position":
        for jd, expr in [("setLeverage(int leverage, long timestamp)", "new SetLeverage(this.id(), value.userId(), value.symbolId(), leverage, timestamp)"), ("addMargin(long amount)", "new AddMargin(this.id(), value.userId(), value.symbolId(), amount)"), ("liquidate(long timestamp)", "new Liquidate(value.symbolId(), this.id(), timestamp)")]:
            methods.append(method("CompletableFuture<CommandReceipt> " + jd, f"return this.snapshot().thenCompose(value -> this.exchange.submit({expr})).thenApply(ApiResults::accepted);"))
    if name == "OrderBook":
        methods += [readpage("depth", "ReadDepth", "DepthView", "OrderSide side, long limit", "new ObservationQuery().withSymbolId(this.id()).withSide(side).withLimit(limit)"), readpage("orders", "ReadBookOrders", "OrderView", "ObservationQuery query", "query.withSymbolId(this.id())"), method("CompletableFuture<Order> place(User user, OrderSide side, OrderType type, TimeInForce timeInForce, long price, long quantity, long timestamp)", "return this.exchange.matching().place(user, new Symbol(this.exchange, this.id()), side, type, timeInForce, price, quantity, timestamp, null);"), method("CompletableFuture<Order> placeContract(User user, OrderSide side, OrderType type, TimeInForce timeInForce, long price, long quantity, long timestamp, ContractOrderOptions options)", "return this.exchange.matching().place(user, new Symbol(this.exchange, this.id()), side, type, timeInForce, price, quantity, timestamp, options);")]
    output_class(name, "object", f"Entity<{view}>", methods)

# Account subtype constructors deliberately validate the native type when read.
output_class("ConditionalOrder", "object", "Entity<ConditionalOrderView>", [
    snapshot("ReadConditionalOrder", "new ObservationQuery().withOrderId(this.id())", "ConditionalOrderView"),
    method("CompletableFuture<Order> order()", "return this.snapshot().thenApply(value -> value.orderId() == 0 ? null : this.exchange.orders().get(value.orderId()));"),
    method("CompletableFuture<CommandReceipt> cancel(long timestamp)", "return this.snapshot().thenCompose(value -> this.exchange.submit(new CancelConditionalOrder(value.request().userId(), this.id(), timestamp))).thenApply(ApiResults::accepted);"),
    method("CompletableFuture<CommandReceipt> modify(long triggerPrice, long price, long quantity, long timestamp)", "return this.snapshot().thenCompose(value -> this.exchange.submit(new ModifyConditionalOrder(value.request().userId(), this.id(), triggerPrice, price, quantity, timestamp))).thenApply(ApiResults::accepted);")])

output_class("ConditionalOrderManager", "manager", "Manager", [
    method("ConditionalOrder get(long id)", "return new ConditionalOrder(this.exchange, id);"),
    method("CompletableFuture<ConditionalOrder> find(long id)", "var handle = this.get(id);\nreturn handle.snapshot().thenApply(value -> handle);"),
    method("CompletableFuture<ConditionalOrder> place(ConditionalOrderRequest request)", "return this.exchange.submit(new PlaceConditionalOrder(request)).thenApply(value -> this.get(ApiResults.accepted(value).result().entityId()));"),
    method("CompletableFuture<ConditionalOrder> place(User user, Symbol symbol, ConditionalOrderRequest request)", "user.requireOwner(this.exchange);\nsymbol.requireOwner(this.exchange);\nreturn this.place(request.withUserId(user.id()).withSymbolId(symbol.id()));"),
    readpage("list", "ReadConditionalOrders", "ConditionalOrderView", "ObservationQuery query", "query")])

for name, kind in [("SpotAccount", "Spot"), ("ContractAccount", "Contract")]:
    output_class(name, "object", "Account", [method("CompletableFuture<AccountView> snapshot()", f"return super.snapshot().thenApply(value ->\n{{\n    if (value.type() != AccountType.{kind})\n    {{\n        throw new IllegalStateException(\"Account type mismatch\");\n    }}\n\n    return value;\n}});")])

for name, view, field in [("Asset", "AssetView", "assetId"), ("User", "UserView", "userId"), ("Symbol", "SymbolView", "symbolId"), ("Order", "OrderView", "orderId"), ("Position", "PositionView", "positionId"), ("OrderBook", "BookView", "symbolId")]:
    manager = "OrderBookManager" if name == "OrderBook" else name + "Manager"
    methods = [method(f"{name} get(long id)", f"return new {name}(this.exchange, id);"), method(f"CompletableFuture<{name}> find(long id)", "var handle = this.get(id);\nreturn handle.snapshot().thenApply(value -> handle);")]
    plural_read = {"Asset": "ReadAssets", "User": "ReadUsers", "Symbol": "ReadSymbols", "Order": "ReadOrders", "Position": "ReadPositions", "OrderBook": "ReadBooks"}[name]
    methods.append(readpage("list", plural_read, view, "ObservationQuery query", "query"))
    if name in ("User", "Asset", "Symbol"):
        jp = {"User": "", "Asset": "String displayName, String assetName, int scale", "Symbol": "String name, SymbolType type, Asset base, Asset quote"}[name]

        ja = {"User": "", "Asset": "displayName, assetName, scale", "Symbol": "name, type, base.id(), quote.id()"}[name]

        jcheck = "base.requireOwner(this.exchange);\nquote.requireOwner(this.exchange);\n" if name == "Symbol" else ""

        methods.append(method(f"CompletableFuture<{name}> create({jp})", jcheck + f"return this.exchange.submit(new Create{name}({ja})).thenApply(value -> this.get(ApiResults.created(value)));"))
    if name == "OrderBook":
        methods += [method("CompletableFuture<OrderBook> addSpot(Symbol symbol, OrderBookImplementation implementation)", "symbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new AddSpotBook(symbol.id(), implementation)).thenApply(value ->\n{\n    ApiResults.accepted(value);\n    return this.get(symbol.id());\n});"), method("CompletableFuture<OrderBook> addContract(Symbol symbol, ContractSpec spec, long markPrice, long timestamp, OrderBookImplementation implementation)", "symbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new AddContractBook(symbol.id(), spec, markPrice, timestamp, implementation)).thenApply(value ->\n{\n    ApiResults.accepted(value);\n    return this.get(symbol.id());\n});")]
    # Native SymbolId and AssetId are uint32; Java facade IDs use long uniformly.
    output_class(manager, "manager", "Manager", methods)

output_class("AccountManager", "manager", "Manager", [method("CompletableFuture<Account> find(long id)", "return this.exchange.submit(new ReadAccount(new ObservationQuery().withAccountId(id))).thenApply(value ->\n{\n    var account = ApiResults.one(value);\n    return account.type() == AccountType.Spot ? new SpotAccount(this.exchange, id) : new ContractAccount(this.exchange, id);\n});"), readpage("list", "ReadAccounts", "AccountView", "ObservationQuery query", "query")])

output_class("MatchingEngine", "manager", "Manager", [method("CompletableFuture<Order> place(User user, Symbol symbol, OrderSide side, OrderType type, TimeInForce timeInForce, long price, long quantity, long timestamp, ContractOrderOptions options)", "user.requireOwner(this.exchange);\nsymbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new PlaceOrder(user.id(), symbol.id(), side, type, timeInForce, price, quantity, timestamp, options)).thenApply(value ->\n{\n    ApiResults.accepted(value);\n    return this.exchange.orders().get(value.id());\n});")])

contracts = [checked("setAutomaticLiquidationBudget(long maxCandidates)", "new SetAutomaticLiquidationBudget(maxCandidates)"), method("CompletableFuture<MarketView> market(Symbol symbol)", "symbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new ReadMarket(new ObservationQuery().withSymbolId(symbol.id()))).thenApply(ApiResults::one);")]
for jd, jc in [
    ("updateMark(Symbol symbol, long price, long timestamp)", "new UpdateMarkPrice(symbol.id(), price, timestamp)"),
    ("configureReference(Symbol symbol, ReferencePriceConfig config)", "new ConfigureReferencePrice(symbol.id(), config)"),
    ("configureProtection(Symbol symbol, long priceBandRate)", "new ConfigureContractProtection(symbol.id(), priceBandRate)"),
    ("configureSchedule(Symbol symbol, ContractSchedule schedule, long timestamp)", "new ConfigureContractSchedule(symbol.id(), schedule, timestamp)"),
    ("publishPrice(Symbol symbol, long sourceId, long price, long timestamp)", "new PublishExternalPrice(symbol.id(), new ExternalPrice(sourceId, price, timestamp))"),
    ("refresh(Symbol symbol, long timestamp)", "new RefreshReferencePrice(symbol.id(), timestamp)"),
    ("updateReference(Symbol symbol, ReferencePrices prices, long timestamp)", "new UpdateReferencePrice(symbol.id(), prices, timestamp)"),
    ("settleFunding(Symbol symbol, int rate, long timestamp)", "new SettleFunding(symbol.id(), rate, timestamp)"),
    ("expire(Symbol symbol, long timestamp)", "new Expire(symbol.id(), timestamp)"),
    ("deliver(Symbol symbol, long price, long timestamp)", "new Deliver(symbol.id(), price, timestamp)"),
    ("processLiquidations(Symbol symbol, long timestamp, boolean force, long maxCandidates)", "new ProcessLiquidations(symbol.id(), timestamp, force, maxCandidates)"),
    ("fundInsurance(Symbol symbol, Account source, long amount)", "new FundInsurance(symbol.id(), source.id(), amount)")]:
    m = checked(jd, jc)
    extra = "source.requireOwner(this.exchange);\n" if "Account source" in jd else ""

    contracts.append((m[0], "symbol.requireOwner(this.exchange);\n" + extra + m[1]))
contracts.append(method("CompletableFuture<CommandReceipt> bindSpot(Symbol symbol, Symbol spot)", "spot.requireOwner(this.exchange);\nreturn this.configureReference(symbol, new ReferencePriceConfig().withSources(java.util.List.of(new PriceSource().withSourceId(spot.id()))));"))
output_class("ContractManager", "manager", "Manager", contracts)

output_class("TradingControlManager", "manager", "Manager", [
    method("CompletableFuture<CommandReceipt> halt(String reason, long timestamp)", "return this.exchange.submit(new SetTradingHalt(0, true, timestamp, reason)).thenApply(ApiResults::accepted);"),
    method("CompletableFuture<CommandReceipt> halt(Symbol symbol, String reason, long timestamp)", "symbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new SetTradingHalt(symbol.id(), true, timestamp, reason)).thenApply(ApiResults::accepted);"),
    method("CompletableFuture<CommandReceipt> resume(long timestamp)", "return this.exchange.submit(new SetTradingHalt(0, false, timestamp, \"\")).thenApply(ApiResults::accepted);"),
    method("CompletableFuture<CommandReceipt> resume(Symbol symbol, long timestamp)", "symbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new SetTradingHalt(symbol.id(), false, timestamp, \"\")).thenApply(ApiResults::accepted);"),
    method("CompletableFuture<TradingStatusView> status()", "return this.exchange.submit(new ReadTradingStatus(new ObservationQuery())).thenApply(ApiResults::one);"),
    method("CompletableFuture<TradingStatusView> status(Symbol symbol)", "symbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new ReadTradingStatus(new ObservationQuery().withSymbolId(symbol.id()))).thenApply(ApiResults::one);")])

output_class("AuditManager", "manager", "Manager", [method("CompletableFuture<AuditReport> report()", "return this.report(64);"), method("CompletableFuture<AuditReport> report(long maxIssues)", "return this.exchange.submit(new ReadAudit(new ObservationQuery().withLimit(maxIssues))).thenApply(ApiResults::one);")])
output_class("TradingRuleManager", "manager", "Manager", [
    method("CompletableFuture<CommandReceipt> configure(Symbol symbol, TradingRules rules)", "symbol.requireOwner(this.exchange);\nreturn this.exchange.submit(new ConfigureTradingRules(symbol.id(), rules)).thenApply(ApiResults::accepted);"),
    method("CompletableFuture<UserRiskView> userRisk(Symbol symbol, User user)", "symbol.requireOwner(this.exchange);\nuser.requireOwner(this.exchange);\nreturn this.exchange.submit(new ReadUserRisk(new ObservationQuery().withSymbolId(symbol.id()).withUserId(user.id()))).thenApply(ApiResults::one);"),
    method("CompletableFuture<FeeBalanceView> spotFees(Asset asset)", "asset.requireOwner(this.exchange);\nreturn this.exchange.submit(new ReadSpotFeeBalance(new ObservationQuery().withAssetId(asset.id()))).thenApply(ApiResults::one);"),
    method("CompletableFuture<CommandReceipt> withdrawSpotFees(Account destination, Asset asset, long amount)", "destination.requireOwner(this.exchange);\nasset.requireOwner(this.exchange);\nreturn this.exchange.submit(new WithdrawSpotFees(destination.id(), asset.id(), amount)).thenApply(ApiResults::accepted);")])

emit(JAVA / "MatchingExchange.java", """
package nhcm.advmatch;

import nhcm.advmatch.model.ExchangeConfig;
import java.util.concurrent.Executor;

// Exchange lifecycle/commands with its object managers; one native owner thread.
public final class MatchingExchange extends Exchange
{
    public MatchingExchange()
    {
        super();
    }

    public MatchingExchange(ExchangeConfig config)
    {
        super(config);
    }

    public MatchingExchange(ExchangeConfig config, Executor callbacks)
    {
        super(config, callbacks);
    }

    public MatchingExchange(ExchangeConfig config, Executor callbacks, TransportOptions transport)
    {
        super(config, callbacks, transport);
    }
}
""")


changed = []
for path, text in outputs.items():
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        changed.append(str(path.relative_to(ROOT)))
        if "--check" not in sys.argv:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
if changed and "--check" in sys.argv:
    raise SystemExit("Object facade is stale: " + ", ".join(changed))
print(f"Object API: {len(outputs)} files, {len(changed)} changed")
