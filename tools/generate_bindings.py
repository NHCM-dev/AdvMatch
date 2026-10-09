"""Generate checked-in Java/JNI DTOs and codecs from the C++ value types.

Run python tools/generate_bindings.py after changing a public value type.
--check verifies that the checked-in files match, without modifying them.
This is deliberately a small value-type parser, not a general C++ parser.
Changing field order changes the binding protocol and needs a version bump.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "java/advmatch/src/main/nhcm/advmatch"

OUTPUTS = {}


def emit(path, text):
    # SDK commands decode slices of an immutable drained response batch. Keep
    # byte[] entry points for existing SDK users and custom Command classes.
    if path.suffix == ".java" and ("command" in path.parts or path.name == "ResponseCodec.java"):
        # Encode into an existing batch buffer without allocating a per-command
        # writer/byte array. The owning encode() entry point remains compatible.
        encoded = r"    public byte\[\] encode\(\)\n    \{\n        var writer = WireWriter.request\((\d+)\);\n(.*?)        return writer.bytes\(\);\n    \}"
        def direct(match):
            operation, content = match.groups()
            return ("    public byte[] encode()\n    {\n        var writer = new WireWriter();\n"
                "        this.encodeTo(writer);\n        return writer.bytes();\n    }\n\n"
                "    @Override\n    public void encodeTo(WireWriter writer)\n    {\n"
                f"        writer.requestHeader({operation});\n{content}    }}")
        text = re.sub(encoded, direct, text, flags=re.S)
        pattern = r"    public (static )?([\w<>]+) (decode|read|overview|batch)\(byte\[\] (response|bytes)\)\n    \{\n(.*?)\n    \}"
        def sliced(match):
            static, result, method, argument, content = match.groups()
            prefix = "static " if static else ""
            body = re.sub(r"WireReader.response\(" + argument + r", (\d+)\)", r"WireReader.response(" + argument + r", \1, offset, size)", content)
            for name in ["read", "overview", "batch"]:
                body = body.replace(name + "(" + argument + ")", name + "(" + argument + ", offset, size)")
            return (f"    public {prefix}{result} {method}(byte[] {argument})\n    {{\n"
                f"        return {method}({argument}, 0, {argument}.length);\n    }}\n\n"
                f"    public {prefix}{result} {method}(byte[] {argument}, int offset, int size)\n    {{\n{body}\n    }}")
        text = re.sub(pattern, sliced, text, flags=re.S)
    OUTPUTS[path] = text.rstrip() + "\n"


def source(path):
    text = (ROOT / path).read_text(encoding="utf-8")
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def body(text, name, kind="struct"):
    match = re.search(r"\b" + kind + r"\s+" + name + r"\b[^\{]*\{", text)
    if not match:
        raise ValueError("Missing " + name)
    start = match.end()
    depth = 1
    end = start
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end - 1]


def fields(text, name):
    result = []
    for line in body(text, name).splitlines():
        match = re.fullmatch(r"\s*([\w:]+(?:<[^;]+?>)?)\s+(\w+)\s*(?:=\s*([^;]+)|\{\})?;\s*", line)
        if match and match.group(2) != "operator":
            result.append(match.groups())
    return result


types = source("src/advmatch/util/ValueTypes.h")
aliases = dict(re.findall(r"using (\w+) = (std::(?:u?int\d+_t));", types))
numbers = {
    "std::uint8_t": ("int", "u8"), "std::uint16_t": ("int", "u16"),
    "std::uint32_t": ("long", "u32"), "std::uint64_t": ("long", "u64"),
    "std::int32_t": ("int", "i32"), "std::int64_t": ("long", "i64"),
    "std::size_t": ("long", "u64"), "int": ("int", "i32"),
}
for alias, underlying in aliases.items():
    numbers[alias] = numbers[underlying]

files = [
    "src/advmatch/trading/OrderTypes.h", "src/advmatch/trading/Symbol.h",
    "src/advmatch/trading/futures/Position.h", "src/advmatch/trading/futures/Contract.h",
    "src/advmatch/trading/futures/ReferencePrice.h",
    "src/advmatch/trading/conditional/ConditionalOrder.h",
    "src/advmatch/trading/rules/TradingRules.h", "src/advmatch/audit/model/AuditReport.h",
    "src/advmatch/trading/TradingControl.h",
    "src/advmatch/matching/orderbooks/OrderBookTypes.h", "src/advmatch/trading/Asset.h",
    "src/advmatch/ledging/account/AccountTypes.h", "src/advmatch/api/model/CommandResult.h",
    "src/advmatch/observe/model/ObservationTypes.h", "src/advmatch/persistence/config/PersistenceConfig.h",
]
enums = {}
for file in files:
    text = source(file)
    for name in re.findall(r"enum class (\w+)[^;\{]*\{", text):
        value = -1
        entries = []
        for entry in body(text, name, "enum class").split(","):
            entry = entry.strip()
            if not entry:
                continue
            parts = entry.split("=")
            if len(parts) > 1:
                value = {"true": 1, "false": 0}.get(parts[1].strip(), None)
                if value is None:
                    value = int(parts[1].strip())
            else:
                value += 1
            entries.append((parts[0].strip(), value))
        enums[name] = entries

models = {}
cppnames = {}


def add(file, names, prefix=""):
    text = source(file)
    for name in names.split():
        models[name] = fields(text, name)
        cppnames[name] = prefix + name


add("src/advmatch/trading/OrderTypes.h", "OrderFillingStatus")
add("src/advmatch/trading/futures/Contract.h", "ContractSpec ContractOrderOptions ContractEvent")
add("src/advmatch/trading/futures/ReferencePrice.h", "PriceSource ReferencePriceConfig ExternalPrice WeightedPrice ReferencePrices ContractSchedule")
add("src/advmatch/trading/conditional/ConditionalOrder.h", "ConditionalOrderRequest ConditionalOrderView")
add("src/advmatch/trading/rules/TradingRules.h", "FeeSchedule RiskLimits TradingRules UserRiskView FeeBalanceView")
add("src/advmatch/trading/TradingControl.h", "TradingStatusView")
add("src/advmatch/audit/model/AuditReport.h", "AssetAudit AuditIssue AuditReport", "audit::")
add("src/advmatch/matching/orderbooks/OrderBookTypes.h", "Quote")
add("src/advmatch/matching/Trade.h", "Trade")
add("src/advmatch/api/model/CommandResult.h", "CommandResult CommandReceipt")
add("src/advmatch/persistence/config/PersistenceConfig.h", "PersistenceConfig")
add("src/advmatch/observe/model/ObservationTypes.h", "ObservationConfig ReadStamp ReadIdentity ReadCursor ObservationQuery ReadPage AssetView UserView AccountView BalanceView CrossMarginView SymbolView OrderView PositionView BookView DepthView MarketView OverviewView TradeView ContractEventView", "observe::")
add("src/advmatch/persistence/model/CommittedCommand.h", "CommittedCommand")
add("src/advmatch/persistence/model/PersistenceStatus.h", "PersistenceStatus CommittedQuery CommittedPage", "persistence::")
models["ExchangeConfig"] = [(t.removeprefix("observe::"), n, d) for t, n, d in fields(source("src/advmatch/config/ExchangeConfig.h"), "ExchangeConfig")[:-1]]
models["ExchangeConfig"].extend([("std::size_t", "maxPendingRequests", "4096"), ("std::size_t", "maxRequestBytes", "4194304")])
cppnames["ExchangeConfig"] = "BindingConfig"

command_text = source("src/advmatch/api/command/model/TradingCommand.h")
command_names = re.findall(r"commands::(\w+)", command_text[command_text.index("using TradingCommand"):])
commands = {name: fields(command_text, name) for name in command_names}
row_types = ["AssetView", "UserView", "AccountView", "BalanceView", "CrossMarginView", "SymbolView", "OrderView", "PositionView", "BookView", "DepthView", "MarketView", "TradeView", "ContractEventView", "ConditionalOrderView", "AuditReport", "UserRiskView", "FeeBalanceView", "TradingStatusView"]
reads = {}
for name, kind, row in re.findall(r"using (Read\w+) = ReadOperation<observe::ReadKind::(\w+), observe::(\w+)>;", source("src/advmatch/api/command/read/ReadCommands.h")):
    reads[name] = (kind, row)


def inner(t):
    return t[t.index("<") + 1:-1]


def java_type(t):
    if t in numbers:
        return numbers[t][0]
    if t == "bool":
        return "boolean"
    if t in ("std::string", "std::filesystem::path"):
        return "String"
    if t.startswith("std::optional<"):
        base = java_type(inner(t))
        return {"long": "Long", "int": "Integer", "boolean": "Boolean"}.get(base, base)
    if t.startswith("std::vector<"):
        base = java_type(inner(t))
        base = {"long": "Long", "int": "Integer"}.get(base, base)
        return "List<" + base + ">"
    return t


def default(t, value):
    if value is not None:
        value = value.replace("'", "")
        value = re.sub(r"std::uint\d+_t\{(\d+)\}", r"\1", value)
        if "numeric_limits" in value:
            return "-1L"
        if "::" in value and value != "std::nullopt":
            return value.replace("::", ".")
        if value.startswith('"') or value in ("true", "false"):
            return value
        if value == "std::nullopt":
            return "null"
        if re.fullmatch(r"[\d *]+", value):
            value = str(eval(value, {"__builtins__": {}}, {}))
            if t in numbers:
                if numbers[t][0] == "long":
                    value += "L"
            return value
    if t in numbers:
        return "0"
    if t == "bool":
        return "false"
    if t in enums:
        return t + "." + enums[t][0][0]
    if t in ("std::string", "std::filesystem::path"):
        return '""'
    if t.startswith("std::optional<"):
        return "null"
    if t.startswith("std::vector<"):
        return "List.of()"
    return "new " + t + "()"


def cap(name):
    return name[0].upper() + name[1:]


def jwrite(t, expr):
    if t in numbers:
        return "writer." + numbers[t][1] + "(" + expr + ");"
    if t == "bool":
        return "writer.bool(" + expr + ");"
    if t in ("std::string", "std::filesystem::path"):
        return "writer.string(" + expr + ");"
    if t in enums:
        return "writer.u8(" + expr + ".code());"
    if t.startswith("std::optional<"):
        return "writer.bool(" + expr + " != null);\n        if (" + expr + " != null)\n        {\n            " + jwrite(inner(t), expr) + "\n        }"
    if t.startswith("std::vector<"):
        return "writer.u64(" + expr + ".size());\n        for (var item : " + expr + ")\n        {\n            " + jwrite(inner(t), "item") + "\n        }"
    return "Wire.write" + t + "(writer, " + expr + ");"


def jread(t):
    if t in numbers:
        return "reader." + numbers[t][1] + "()"
    if t == "bool":
        return "reader.bool()"
    if t in ("std::string", "std::filesystem::path"):
        return "reader.string()"
    if t in enums:
        return t + ".fromCode(reader.u8())"
    if t.startswith("std::optional<"):
        return "(reader.bool() ? " + jread(inner(t)) + " : null)"
    if t.startswith("std::vector<"):
        return "reader.list(() -> " + jread(inner(t)) + ")"
    return "Wire.read" + t + "(reader)"


for name, entries in enums.items():
    constants = ",\n    ".join(n + "(" + str(v) + ")" for n, v in entries)
    emit(JAVA / "model" / (name + ".java"), f"""// Generated by tools/generate_bindings.py; binding protocol v1.
package nhcm.advmatch.model;

public enum {name}
{{
    {constants};

    private final int code;

    {name}(int code)
    {{
        this.code = code;
    }}

    public int code()
    {{
        return this.code;
    }}

    public static {name} fromCode(int code)
    {{
        for (var value : values())
        {{
            if (value.code == code)
            {{
                return value;
            }}
        }}

        throw new IllegalArgumentException("Unknown {name}: " + code);
    }}
}}
""")


jcodec = ["// Generated by tools/generate_bindings.py; binding protocol v1.", "package nhcm.advmatch.internal;", "", "import nhcm.advmatch.model.*;", "", "public final class Wire", "{", "    private Wire()", "    {", "    }", ""]

cpp = ["// Generated by tools/generate_bindings.py; binding protocol v1.", "#pragma once", "", '#include "BindingTypes.h"', '#include "advmatch/persistence/codec/BinaryCodec.h"', "", "namespace advmatch::binding", "{"]
for name, fs in models.items():
    cpptype = cppnames[name]
    cpp += [f"    inline void write(BinaryWriter& writer, const {cpptype}& value);", f"    inline {cpptype} read{name}(BinaryReader& reader);"]


def cppwrite(t, expr):
    if t == "std::size_t":
        return "writer.number<std::uint64_t>(" + expr + ");"
    if t in numbers:
        return "writer.number(" + expr + ");"
    if t == "bool":
        return "writer.boolean(" + expr + ");"
    if t == "std::string":
        return "writer.string(" + expr + ");"
    if t == "std::filesystem::path":
        return "const auto path = " + expr + ".u8string();\n        writer.string(std::string(path.begin(), path.end()));"
    if t in enums:
        return "writer.enumeration(" + expr + ");"
    if t.startswith("std::optional<"):
        return "writer.boolean(" + expr + ".has_value());\n        if (" + expr + ")\n        {\n            " + cppwrite(inner(t), "*" + expr) + "\n        }"
    if t.startswith("std::vector<"):
        return "writer.number<std::uint64_t>(" + expr + ".size());\n        for (const auto& item : " + expr + ")\n        {\n            " + cppwrite(inner(t), "item") + "\n        }"
    return "write(writer, " + expr + ");"


def cppread(t):
    if t == "std::size_t":
        return "readSize(reader)"
    if t in numbers:
        return "reader.number<" + t + ">()"
    if t == "bool":
        return "reader.boolean()"
    if t == "std::string":
        return "reader.string()"
    if t == "std::filesystem::path":
        return "readPath(reader)"
    if t in enums:
        cppenum = "observe::" + t if t in ("ReadState", "ReadKind", "PositionFilter") else "audit::" + t if t == "AuditIssueType" else t
        return "reader.enumeration(static_cast<" + cppenum + ">(" + str(max(v for _, v in enums[t])) + "))"
    return "read" + t + "(reader)"


cpp += ["", "    inline std::size_t readSize(BinaryReader& reader)", "    {", "        const auto value = reader.number<std::uint64_t>();", '        persistenceCheck(value <= std::numeric_limits<std::size_t>::max(), "size exceeds platform range");', "        return static_cast<std::size_t>(value);", "    }"]
cpp += ["", "    inline std::filesystem::path readPath(BinaryReader& reader)", "    {", "        const auto value = reader.string();", "        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));", "    }"]
for name, fs in models.items():
    params = ", ".join(java_type(t) + " " + n for t, n, _ in fs)
    defaults = ", ".join(default(t, d) for t, _, d in fs)
    receipt_interface = " implements Receipt" if name == "CommandReceipt" else ""
    jlines = ["// Generated by tools/generate_bindings.py; binding protocol v1.", "package nhcm.advmatch.model;", "", "import java.util.List;", "", f"public record {name}({params}){receipt_interface}", "{", f"    public {name}()", "    {", f"        this({defaults});", "    }", ""]
    for t, n, _ in fs:
        if t.startswith("std::vector<"):
            jlines += [f"    public {name}", "    {", f"        {n} = List.copyOf({n});", "    }", ""]
    if name in ("ObservationQuery", "ExchangeConfig", "PersistenceConfig", "ObservationConfig", "ContractSpec", "ContractOrderOptions", "PriceSource", "ReferencePriceConfig", "ReferencePrices", "ContractSchedule", "ConditionalOrderRequest", "FeeSchedule", "RiskLimits", "TradingRules"):
        for t, n, _ in fs:
            args = ", ".join("value" if fn == n else "this." + fn for _, fn, _ in fs)
            jlines += [f"    public {name} with{cap(n)}({java_type(t)} value)", "    {", f"        return new {name}({args});", "    }", ""]
    if name == "CommandReceipt":
        jlines += ["    public CommandReceipt receipt()", "    {", "        return this;", "    }", "", "    public boolean durable()", "    {", "        return this.sequence != 0 && Long.compareUnsigned(this.sequence, this.durableThrough) <= 0;", "    }", ""]
    jlines += ["}"]
    # A record has at most one compact constructor, even with several lists.
    listnames = [n for t, n, _ in fs if t.startswith("std::vector<")]
    if len(listnames) > 1:
        for n in listnames:
            chunk = [f"    public {name}", "    {", f"        {n} = List.copyOf({n});", "    }", ""]
            start = next(i for i in range(len(jlines)) if jlines[i:i + len(chunk)] == chunk)
            del jlines[start:start + len(chunk)]
        jlines[11:11] = [f"    public {name}", "    {"] + [f"        {n} = List.copyOf({n});" for n in listnames] + ["    }", ""]
    emit(JAVA / "model" / (name + ".java"), "\n".join(jlines))


    jcodec += [f"    public static void write{name}(WireWriter writer, {name} value)", "    {"]
    jcodec += ["        " + jwrite(t, "value." + n + "()") for t, n, _ in fs]
    jcodec += ["    }", "", f"    public static {name} read{name}(WireReader reader)", "    {", "        return new " + name + "(" + ", ".join(jread(t) for t, _, _ in fs) + ");", "    }", ""]


    cpp += ["", f"    inline void write(BinaryWriter& writer, const {cppnames[name]}& value)", "    {"]
    cpp += ["        " + cppwrite(t, "value." + n) for t, n, _ in fs]
    cpp += ["    }", "", f"    inline {cppnames[name]} read{name}(BinaryReader& reader)", "    {", f"        {cppnames[name]} value{{}};"]
    for t, n, _ in fs:
        if t.startswith("std::optional<"):
            cpp += ["        if (reader.boolean())", "        {", f"            value.{n} = {cppread(inner(t))};", "        }"]
        elif t.startswith("std::vector<"):
            cpp += [f"        const auto {n}Count = reader.count();", f"        value.{n}.reserve({n}Count);", f"        for (std::size_t index = 0; index < {n}Count; ++index)", "        {", f"            value.{n}.push_back({cppread(inner(t))});", "        }"]
        else:
            cpp += [f"        value.{n} = {cppread(t)};"]
    cpp += ["        return value;", "    }"]
cpp += ["}"]
jcodec += ["}"]

emit(JAVA / "internal/Wire.java", "\n".join(jcodec))

emit(ROOT / "src/advmatch/api/binding/WireModels.h", "\n".join(cpp))

groups = {
    "CreateAsset": "management", "RemoveAsset": "management", "CreateUser": "management", "RemoveUser": "management", "CreateSymbol": "management", "RemoveSymbol": "management",
    "UpdateBalance": "account", "Transfer": "account", "AddSpotBook": "order", "PlaceOrder": "order", "ModifyPrice": "order", "ModifyQuantity": "order", "CancelOrder": "order", "ArchiveHistory": "maintenance",
    "PlaceConditionalOrder": "order", "ModifyConditionalOrder": "order", "CancelConditionalOrder": "order",
    "ConfigureTradingRules": "management", "WithdrawSpotFees": "account", "SetTradingHalt": "management",
}
for operation, (name, fs) in enumerate(commands.items()):
    group = groups.get(name, "contract")
    result_type = "CreationResult" if name in ("CreateAsset", "CreateUser", "CreateSymbol") else "CommandReceipt"
    jp = ", ".join(java_type(t) + " " + n for t, n, _ in fs)
    ja = ", ".join(n for _, n, _ in fs)
    params = jp + (", " if jp else "") + "long requestId"
    lines = ["// Generated by tools/generate_bindings.py; binding protocol v1.", f"package nhcm.advmatch.command.{group};", "", "import nhcm.advmatch.command.Command;", "import nhcm.advmatch.internal.*;", "import nhcm.advmatch.model.*;", "import java.util.List;", "", f"public record {name}({params}) implements Command<{result_type}>", "{", f"    public {name}({jp})", "    {", f"        this({ja + ', ' if ja else ''}0);", "    }", ""]
    trailing = 0
    for t, n, d in reversed(fs):
        if d is None:
            break
        trailing += 1
        kept = fs[:-trailing]
        moreparams = ", ".join(java_type(t) + " " + n for t, n, _ in kept)
        moreargs = ", ".join([n for _, n, _ in kept] + [default(t, d) for t, _, d in fs[-trailing:]])
        lines += [f"    public {name}({moreparams})", "    {", f"        this({moreargs + ', ' if moreargs else ''}0);", "    }", ""]
    arrays = [n for t, n, _ in fs if t.startswith("std::vector<")]
    if arrays:
        lines += [f"    public {name}", "    {"] + [f"        {n} = List.copyOf({n});" for n in arrays] + ["    }", ""]
    lines += ["    @Override", "    public byte[] encode()", "    {", "        var writer = WireWriter.request(0);", "        writer.u16(1);", "        writer.u64(this.requestId);", f"        writer.u16({operation});"]
    lines += ["        " + jwrite(t, "this." + n) for t, n, _ in fs]
    result_expr = "new CreationResult(value)" if result_type == "CreationResult" else "value"
    lines += ["        return writer.bytes();", "    }", "", "    @Override", f"    public {result_type} decode(byte[] response)", "    {", "        var reader = WireReader.response(response, 1);", "        var value = Wire.readCommandReceipt(reader);", "        reader.finish();", f"        return {result_expr};", "    }", "}"]
    emit(JAVA / "command" / group / (name + ".java"), "\n".join(lines))


for name, (kind, row) in reads.items():
    response = "OverviewResponse" if kind == "Overview" else "ReadResponse<" + row + ">"
    decoder = "ResponseCodec.overview(response)" if kind == "Overview" else "ResponseCodec.read(response).typed(" + row + ".class)"
    emit(JAVA / "command/read" / (name + ".java"), f"""// Generated by tools/generate_bindings.py; binding protocol v1.
package nhcm.advmatch.command.read;

import nhcm.advmatch.command.Command;
import nhcm.advmatch.internal.*;
import nhcm.advmatch.model.*;

public record {name}(ObservationQuery query) implements Command<{response}>
{{
    public {name}
    {{
        query = query.withKind(ReadKind.{kind});
    }}

    public {name}()
    {{
        this(new ObservationQuery());
    }}

    @Override
    public byte[] encode()
    {{
        var writer = WireWriter.request(1);
        Wire.writeObservationQuery(writer, this.query);
        return writer.bytes();
    }}

    @Override
    public {response} decode(byte[] response)
    {{
        return {decoder};
    }}
}}
""")


java_cases = ["            case 0 -> null;", "            case 1 -> Wire.readOverviewView(reader);"]

for index, row in enumerate(row_types, 2):
    java_cases.append(f"            case {index} -> reader.list(() -> Wire.read{row}(reader));")

emit(JAVA / "internal/ResponseCodec.java", """// Generated by tools/generate_bindings.py; binding protocol v1.
package nhcm.advmatch.internal;

import nhcm.advmatch.model.*;

public final class ResponseCodec
{
    private ResponseCodec()
    {
    }

    public static ObservationResult observation(WireReader reader)
    {
        var query = Wire.readObservationQuery(reader);
        var page = Wire.readReadPage(reader);
        Object data = switch (reader.u8())
        {
""" + "\n".join(java_cases) + """
            default -> throw new IllegalArgumentException("Unknown observation payload");
        };
        return new ObservationResult(query, page, data);
    }

    public static ObservationResult read(byte[] bytes)
    {
        var reader = WireReader.response(bytes, 2);
        var value = observation(reader);
        reader.finish();
        return value;
    }

    public static OverviewResponse overview(byte[] bytes)
    {
        var result = read(bytes);
        return new OverviewResponse(result.query(), result.page(), (OverviewView)result.data());
    }

    public static ObservationBatch batch(byte[] bytes)
    {
        var reader = WireReader.response(bytes, 3);
        var state = ReadState.fromCode(reader.u8());
        var stamp = Wire.readReadStamp(reader);
        var values = reader.list(() -> observation(reader));
        reader.finish();
        return new ObservationBatch(state, stamp, values);
    }
}
""")


for name, operation, response, type_id, kind in [
    ("Flush", 3, "PersistenceStatus", 4, "persistence"), ("Tick", 4, "PersistenceStatus", 4, "persistence"),
    ("Checkpoint", 5, "PersistenceStatus", 4, "persistence"), ("ReadPersistenceStatus", 9, "PersistenceStatus", 4, "persistence"),
    ("ReadCommitted", 10, "CommittedPage", 5, "persistence"), ("RebuildOrderIndex", 8, "Void", 0, "read"),
    ("ReadBatch", 2, "ObservationBatch", 3, "read"), ("Read", 1, "ObservationResult", 2, "read"),
    ("CreateObservationSlot", 6, "Long", 6, "read"), ("RefreshObservation", 7, "ObservationResult", 2, "read"),
]:
    param = {"ReadCommitted": ("CommittedQuery", "query"), "ReadBatch": ("List<ObservationQuery>", "queries"), "Read": ("ObservationQuery", "query"), "CreateObservationSlot": ("ObservationQuery", "query"), "RefreshObservation": ("long", "slotId")}.get(name)
    jp = param[0] + " " + param[1] if param else ""
    jwrites = []
    if name == "ReadBatch":
        jwrites = ["        writer.u64(this.queries.size());", "        for (var query : this.queries)", "        {", "            Wire.writeObservationQuery(writer, query);", "        }"]
    elif param:
        jwrites = ["        " + ("writer.u64(this.slotId);" if name == "RefreshObservation" else "Wire.write" + param[0] + "(writer, this.query);")]
    decode = ""
    if response in ("ObservationBatch", "ObservationResult"):
        decode = "        return ResponseCodec." + ("batch" if response == "ObservationBatch" else "read") + "(response);"
    else:
        value = "Wire.read" + response + "(reader)" if response not in ("Void", "Long") else ("null" if response == "Void" else "reader.u64()")
        decode = f"        var reader = WireReader.response(response, {type_id});\n        {response} value = {value};\n        reader.finish();\n        return value;"
    emit(JAVA / "command" / kind / (name + ".java"), f"""// Generated by tools/generate_bindings.py; binding protocol v1.
package nhcm.advmatch.command.{kind};

import nhcm.advmatch.command.Command;
import nhcm.advmatch.internal.*;
import nhcm.advmatch.model.*;
import java.util.List;

public record {name}({jp}) implements Command<{response}>
{{
    @Override
    public byte[] encode()
    {{
        var writer = WireWriter.request({operation});
""" + "\n".join(jwrites) + f"""
        return writer.bytes();
    }}

    @Override
    public {response} decode(byte[] response)
    {{
{decode}
    }}
}}
""")


changed = []
for path, text in OUTPUTS.items():
    if path.exists() and path.read_text(encoding="utf-8") == text:
        continue
    changed.append(path.relative_to(ROOT))
    if "--check" not in sys.argv:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
if "--check" in sys.argv and changed:
    print("Binding files need regeneration:", *changed, sep="\n")
    sys.exit(1)
print(f"Bindings: {len(OUTPUTS)} files, {len(changed)} changed")
