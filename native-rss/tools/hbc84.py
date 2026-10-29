"""Small offline HBC84 reader/writer for the explicitly supported home adapter.

Format tables are from the MIT-licensed hbctool; see third_party/hbc84/SOURCE.json.
No firmware content is bundled. Unsupported layouts fail before producing a patch.
"""
import hashlib
import json
from pathlib import Path
import struct

TABLES = Path(__file__).resolve().parents[1] / "third_party/hbc84"
STRUCTURE = json.loads((TABLES / "structure.json").read_text())
opcode_operand = json.loads((TABLES / "opcode.json").read_text())
OPCODES = list(opcode_operand)
FORMATS = {"Reg8": "B", "Reg32": "I", "UInt8": "B", "UInt16": "H", "UInt32": "I",
           "Addr8": "b", "Addr32": "i", "Imm32": "I", "Double": "d"}


def read_record(data, offset, schema):
    position, result = offset * 8, {}
    for name, (_, width, count) in schema.items():
        values = []
        for _ in range(count):
            end = (position + width + 7) // 8
            if end > len(data):
                raise ValueError("Truncated HBC record")
            raw = int.from_bytes(data[position // 8:end], "little")
            values.append((raw >> (position % 8)) & ((1 << width) - 1))
            position += width
        result[name] = values[0] if count == 1 else values
    return result, (position + 7) // 8


def write_record(value, schema):
    bits, position = 0, 0
    for name, (_, width, count) in schema.items():
        values = [value[name]] if count == 1 else value[name]
        if len(values) != count:
            raise ValueError("Invalid HBC field count")
        for number in values:
            if not 0 <= number < 1 << width:
                raise ValueError("HBC field overflow: " + name)
            bits |= number << position
            position += width
    return bits.to_bytes((position + 7) // 8, "little")


def assemble(instructions):
    result = bytearray()
    for op, operands in instructions:
        expected = opcode_operand[op]
        actual = [kind + (":S" if string else "") for kind, string, _ in operands]
        if actual != expected:
            raise ValueError("Invalid operands for " + op)
        result.append(OPCODES.index(op))
        for kind, _, value in operands:
            result.extend(struct.pack("<" + FORMATS[kind], value))
    return bytes(result)


def disassemble(data):
    position, result = 0, []
    while position < len(data):
        if data[position] >= len(OPCODES):
            raise ValueError("Unknown HBC instruction")
        op = OPCODES[data[position]]
        position += 1
        operands = []
        for encoded in opcode_operand[op]:
            kind = encoded.removesuffix(":S")
            size = struct.calcsize(FORMATS[kind])
            if position + size > len(data):
                raise ValueError("Truncated HBC instruction")
            operands.append((kind, encoded.endswith(":S"), struct.unpack_from("<" + FORMATS[kind], data, position)[0]))
            position += size
        result.append((op, operands))
    return result


class HBC84:
    def __init__(self, data):
        if not 148 <= len(data) <= 16 * 1024 * 1024:
            raise ValueError("Invalid HBC size")
        header, position = read_record(data, 0, STRUCTURE["header"])
        if header["magic"] != 0x1F1903C103BC1FC6 or header["version"] != 84:
            raise ValueError("Only HBC84 is supported")
        if header["fileLength"] != len(data) or hashlib.sha1(data[:-20]).digest() != data[-20:]:
            raise ValueError("Invalid HBC length or footer digest")
        if header["functionCount"] > 65535 or header["stringCount"] > 65535 or header["functionSourceCount"]:
            raise ValueError("Unsupported HBC tables")
        self.obj = {"header": header}

        def records(name, count, schema):
            nonlocal position
            position = (position + 3) & ~3
            values = []
            for _ in range(count):
                item, position = read_record(data, position, STRUCTURE[schema])
                values.append(item)
            self.obj[name] = values

        def integers(name, count):
            nonlocal position
            position = (position + 3) & ~3
            if count > (len(data) - position) // 4:
                raise ValueError("Invalid HBC table size")
            self.obj[name] = list(struct.unpack_from("<" + "I" * count, data, position))
            position += count * 4

        def blob(name, size):
            nonlocal position
            position = (position + 3) & ~3
            if position + size > len(data):
                raise ValueError("Truncated HBC blob")
            self.obj[name] = bytearray(data[position:position + size])
            position += size

        records("functionHeaders", header["functionCount"], "SmallFuncHeader")
        for item in self.obj["functionHeaders"]:
            if item["flags"] & 32:
                small = item.copy()
                pointer = (item["infoOffset"] << 16) | item["offset"]
                item.update(read_record(data, pointer, STRUCTURE["FuncHeader"])[0])
                item["small"] = small
        integers("stringKinds", header["stringKindCount"])
        integers("identifierHashes", header["identifierCount"])
        records("stringTableEntries", header["stringCount"], "SmallStringTableEntry")
        records("stringTableOverflowEntries", header["overflowStringCount"], "OverflowStringTableEntry")
        for name in ("stringStorage", "arrayBuffer", "objKeyBuffer", "objValueBuffer"):
            blob(name, header[name + "Size"])
        records("regExpTable", header["regExpCount"], "RegExpTableEntry")
        blob("regExpStorage", header["regExpStorageSize"])
        records("cjsModuleTable", header["cjsModuleCount"], "CJSModuleTable")
        self.obj["instOffset"] = (position + 3) & ~3
        self.obj["inst"] = bytearray(data[self.obj["instOffset"]:])

    def getObj(self): return self.obj
    def getHeader(self): return self.obj["header"]
    def getVersion(self): return 84
    def getStringCount(self): return self.getHeader()["stringCount"]
    def getFunctionCount(self): return self.getHeader()["functionCount"]

    def getString(self, sid):
        item = self.obj["stringTableEntries"][sid]
        offset, length = item["offset"], item["length"]
        if length == 255:
            item_overflow = self.obj["stringTableOverflowEntries"][offset]
            offset, length = item_overflow["offset"], item_overflow["length"]
        size = length * (2 if item["isUTF16"] else 1)
        raw = bytes(self.obj["stringStorage"][offset:offset + size])
        if len(raw) != size:
            raise ValueError("Truncated HBC string")
        return (raw.hex() if item["isUTF16"] else raw.decode("utf8"), (item["isUTF16"], offset, size))

    def getFunction(self, fid, disasm=True):
        item = self.obj["functionHeaders"][fid]
        offset = item["offset"] - self.obj["instOffset"]
        code = bytes(self.obj["inst"][offset:offset + item["bytecodeSizeInBytes"]])
        if offset < 0 or len(code) != item["bytecodeSizeInBytes"]:
            raise ValueError("Invalid HBC function range")
        return (self.getString(item["functionName"])[0], item["paramCount"], item["frameSize"],
                item["environmentSize"], disassemble(code) if disasm else code, item)

    def getBareFunction(self, fid): return self.getFunction(fid, False)

    def export(self):
        result = bytearray(write_record(self.getHeader(), STRUCTURE["header"]))
        def add(values, schema=None):
            while len(result) % 4:
                result.append(0)
            for value in values:
                result.extend(write_record(value, STRUCTURE[schema]) if schema else struct.pack("<I", value))
        add([item.get("small", item) for item in self.obj["functionHeaders"]], "SmallFuncHeader")
        add(self.obj["stringKinds"])
        add(self.obj["identifierHashes"])
        add(self.obj["stringTableEntries"], "SmallStringTableEntry")
        add(self.obj["stringTableOverflowEntries"], "OverflowStringTableEntry")
        for name in ("stringStorage", "arrayBuffer", "objKeyBuffer", "objValueBuffer"):
            add([])
            result.extend(self.obj[name])
        add(self.obj["regExpTable"], "RegExpTableEntry")
        add([])
        result.extend(self.obj["regExpStorage"])
        add(self.obj["cjsModuleTable"], "CJSModuleTable")
        add([])
        result.extend(self.obj["inst"])
        for item in self.obj["functionHeaders"]:
            if "small" in item:
                pointer = (item["small"]["infoOffset"] << 16) | item["small"]["offset"]
                raw = write_record(item, STRUCTURE["FuncHeader"])
                result[pointer:pointer + len(raw)] = raw
        return bytes(result)


def load(stream): return HBC84(stream.read())
def dump(container, stream): stream.write(container.export())
