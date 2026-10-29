#!/usr/bin/env python3
"""Build a firmware-specific difference recipe from a user-supplied original.

Never connects to or installs on a Kindle. Original firmware and patched HBC
remain local; the distributable recipe needs the matching original to apply.
"""
from pathlib import Path
import argparse
import copy
import hashlib
import io
import json
import os
import struct
import subprocess
import tempfile

import hbc84 as hbc
from hbc84 import assemble, opcode_operand

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"RSSHBC1\n"
MAXIMUM = 16 * 1024 * 1024


def merge(source, helper_bytes, profile):
    if hashlib.sha256(source).hexdigest() != profile["original_sha256"]:
        raise ValueError("固件哈希不受支持；原文件未修改")
    original = hbc.load(io.BytesIO(source))
    output = hbc.load(io.BytesIO(source))
    helper = hbc.load(io.BytesIO(helper_bytes))
    obj, extra = output.getObj(), helper.getObj()
    if not (original.getVersion() == helper.getVersion() == 84):
        raise ValueError("Unsupported HBC layout")
    if not (helper.getFunction(1)[0] == "useRssCard"):
        raise ValueError("Unsupported HBC layout")
    if not (not any(extra[name] for name in ("arrayBuffer", "objKeyBuffer", "objValueBuffer", "regExpTable"))):
        raise ValueError("Unsupported HBC layout")
    if not (original.getHeader()["functionSourceCount"] == 0):
        raise ValueError("Unsupported HBC layout")


    def kinds_and_hashes(container):
        kinds, hashes, hash_index = [], {}, 0
        for run in container.getObj()["stringKinds"]:
            kind, count = run >> 31, run & 0x7fffffff
            for _ in range(count):
                if kind:
                    hashes[len(kinds)] = container.getObj()["identifierHashes"][hash_index]
                    hash_index += 1
                kinds.append(kind)
        if not (len(kinds) == container.getStringCount()):
            raise ValueError("Unsupported HBC layout")
        if not (hash_index == container.getHeader()["identifierCount"]):
            raise ValueError("Unsupported HBC layout")
        return kinds, hashes


    old_kinds, _ = kinds_and_hashes(original)
    helper_kinds, helper_hashes = kinds_and_hashes(helper)
    string_map = {}
    existing = {(old_kinds[sid], original.getString(sid)[1][0], original.getString(sid)[0]): sid
                for sid in range(original.getStringCount())}
    all_kinds = old_kinds[:]
    for sid in range(helper.getStringCount()):
        value, metadata = helper.getString(sid)
        key = (helper_kinds[sid], metadata[0], value)
        match = existing.get(key)
        if match is None and not helper_kinds[sid]:
            match = existing.get((1, metadata[0], value))
        if match is None:
            match = len(obj["stringTableEntries"])
            if metadata[0] and len(obj["stringStorage"]) % 2:
                obj["stringStorage"].append(0)
            data = bytes.fromhex(value) if metadata[0] else value.encode()
            length = len(data) // 2 if metadata[0] else len(data)
            if not (length < 255):
                raise ValueError("Unsupported HBC layout")
            obj["stringTableEntries"].append({"isUTF16": metadata[0], "offset": len(obj["stringStorage"]), "length": length})
            obj["stringStorage"].extend(data)
            all_kinds.append(helper_kinds[sid])
            if helper_kinds[sid]:
                obj["identifierHashes"].append(helper_hashes[sid])
            existing[key] = match
        string_map[sid] = match
    obj["stringKinds"] = []
    for kind in all_kinds:
        if obj["stringKinds"] and obj["stringKinds"][-1] >> 31 == kind:
            obj["stringKinds"][-1] += 1
        else:
            obj["stringKinds"].append((kind << 31) | 1)
    obj["header"].update(stringKindCount=len(obj["stringKinds"]), identifierCount=len(obj["identifierHashes"]),
                         stringCount=len(all_kinds), stringStorageSize=len(obj["stringStorage"]))

    first_new = original.getFunctionCount()
    function_map = {fid: first_new + fid - 1 for fid in range(1, helper.getFunctionCount())}


    def relocate_function(fid):
        instructions = helper.getFunction(fid)[4]
        old_pc, old_positions, transformed = 0, [], []
        for op, operands in instructions:
            old_positions.append(old_pc)
            old_pc += len(assemble([(op, operands)]))
            operands = list(operands)
            for index, (kind, is_string, value) in enumerate(operands):
                if is_string:
                    value = string_map[value]
                    if kind == "UInt8" and value > 255:
                        if not (op in ("GetByIdShort", "PutNewOwnByIdShort")):
                            raise ValueError("Unsupported HBC layout")
                        op = op.removesuffix("Short")
                        kind = "UInt16"
                    operands[index] = (kind, is_string, value)
            if op == "CreateClosure":
                kind, is_string, value = operands[-1]
                operands[-1] = (kind, is_string, function_map[value])
            if any(kind == "Addr8" for kind, _, _ in operands):
                op += "Long"
                if not (op in opcode_operand):
                    raise ValueError("Unsupported HBC layout")
                operands = [("Addr32" if kind == "Addr8" else kind, is_string, value)
                            for kind, is_string, value in operands]
            if not ([kind + (":S" if is_string else "") for kind, is_string, _ in operands] == opcode_operand[op]):
                raise ValueError("Unsupported HBC layout")
            transformed.append((op, operands))
        old_positions.append(old_pc)
        new_positions, new_pc = [], 0
        for op, operands in transformed:
            new_positions.append(new_pc)
            new_pc += len(assemble([(op, operands)]))
        new_positions.append(new_pc)
        pc_map = dict(zip(old_positions, new_positions))
        for index, (op, operands) in enumerate(transformed):
            for operand_index, (kind, is_string, value) in enumerate(operands):
                if kind == "Addr32":
                    operands[operand_index] = (kind, is_string, pc_map[old_positions[index] + value] - new_positions[index])
        bytecode = bytes(assemble(transformed))
        # Exception offsets are instruction offsets inside the function, not file offsets.
        handlers = []
        header = extra["functionHeaders"][fid]
        if header["flags"] & 8:
            offset = (header["infoOffset"] + 3) & ~3
            count = struct.unpack_from("<I", helper_bytes, offset)[0]
            for index in range(count):
                triplet = struct.unpack_from("<III", helper_bytes, offset + 4 + index * 12)
                handlers.append(tuple(pc_map[value] for value in triplet))
        return transformed, bytecode, handlers


    compiled = {fid: relocate_function(fid) for fid in function_map}
    for fid in function_map:
        header = copy.deepcopy(extra["functionHeaders"][fid])
        header["functionName"] = string_map[header["functionName"]]
        header["flags"] &= ~(16 | 32)
        obj["functionHeaders"].append(header)
    obj["header"]["functionCount"] = len(obj["functionHeaders"])
    # Determine the new metadata prefix size before updating absolute file offsets.
    temporary = io.BytesIO()
    hbc.dump(output, temporary)
    new_base = len(temporary.getvalue()) - len(obj["inst"])
    delta = new_base - obj["instOffset"]
    for header in obj["functionHeaders"][:first_new]:
        header["offset"] += delta
        if header["infoOffset"]:
            header["infoOffset"] += delta
        if "small" in header:
            small = header["small"]
            pointer = ((small["infoOffset"] << 16) | small["offset"]) + delta
            small["offset"], small["infoOffset"] = pointer & 65535, pointer >> 16
    obj["header"]["debugInfoOffset"] += delta
    obj["instOffset"] = new_base
    obj["inst"] = bytearray(obj["inst"][:-20])


    def append_code(header, bytecode, handlers):
        header["offset"] = new_base + len(obj["inst"])
        header["bytecodeSizeInBytes"] = len(bytecode)
        if not (len(bytecode) < 32768 and header["frameSize"] < 128):
            raise ValueError("Unsupported HBC layout")
        obj["inst"].extend(bytecode)
        header["infoOffset"] = 0
        if handlers:
            while len(obj["inst"]) % 4:
                obj["inst"].append(0)
            header["infoOffset"] = new_base + len(obj["inst"])
            obj["inst"].extend(struct.pack("<I", len(handlers)))
            for handler in handlers:
                obj["inst"].extend(struct.pack("<III", *handler))


    for fid, (_, bytecode, handlers) in compiled.items():
        append_code(obj["functionHeaders"][function_map[fid]], bytecode, handlers)
    # Insert before the original render's first call, using registers not live yet.
    template = copy.deepcopy(original.getFunction(profile["render_function"])[4])
    if not (template[4] == ("LoadFromEnvironment", [("Reg8", False, 21), ("Reg8", False, 20), ("UInt8", False, 0)])):
        raise ValueError("Unsupported HBC layout")
    def r(value): return ("Reg8", False, value)
    def u8(value): return ("UInt8", False, value)
    injection = [
        ("LoadFromEnvironment", [r(19), r(20), u8(1)]),
        ("CreateClosure", [r(18), r(20), ("UInt16", False, function_map[1])]),
        ("GetGlobalObject", [r(17)]),
        ("Call4", [r(0), r(18), r(17), r(21), r(19), r(0)]),
        ("StoreToEnvironment", [r(22), u8(0), r(0)]),
    ]
    # Keep every original hook call, then replace only the RSS card's returned element.
    render_key = next(sid for sid in range(helper.getStringCount()) if helper.getString(sid)[0] == "rssHomeRender")
    if template[173][0] != "GetByIdShort" or template[-1] != ("Ret", [r(0)]):
        raise ValueError("Unsupported Template13 return layout")
    template[173:173] = [("GetById", [r(28), r(0), u8(28), ("UInt16", True, string_map[render_key])])]
    finish = [
        ("JmpFalseLong", [("Addr32", False, 0), r(28)]),
        ("LoadConstUndefined", [r(1)]),
        ("Call2", [r(0), r(28), r(1), r(0)]),
        ("Ret", [r(0)]),
    ]
    finish[0][1][0] = ("Addr32", False, len(assemble(finish[:-1])))
    template[-1:] = finish
    template[5:5] = injection
    template_header = obj["functionHeaders"][profile["render_function"]]
    if not (not template_header["flags"] & 8 and "small" not in template_header):
        raise ValueError("Unsupported HBC layout")
    template_header["flags"] &= ~16
    template_header["highestReadCacheIndex"] = max(template_header["highestReadCacheIndex"], 28)
    append_code(template_header, bytes(assemble(template)), [])
    press_id = profile["press_function"]
    press = copy.deepcopy(original.getFunction(press_id)[4])
    callback = next(sid for sid in range(helper.getStringCount()) if helper.getString(sid)[0] == "rssHomeOpen")
    prefix = [
        ("GetEnvironment", [r(0), u8(0)]),
        ("LoadFromEnvironment", [r(1), r(0), u8(0)]),
        ("GetById", [r(2), r(1), u8(0), ("UInt16", True, string_map[callback])]),
        ("JmpFalseLong", [("Addr32", False, 0), r(2)]),
        ("LoadConstUndefined", [r(3)]),
        ("Call1", [r(3), r(2), r(3)]),
        ("Ret", [r(3)]),
    ]
    prefix[3][1][0] = ("Addr32", False, len(assemble(prefix[3:])))
    press = prefix + press
    press_header = obj["functionHeaders"][press_id]
    if not (not press_header["flags"] & 8 and "small" not in press_header):
        raise ValueError("Unsupported HBC layout")
    press_header["flags"] &= ~16
    append_code(press_header, bytes(assemble(press)), [])

    obj["inst"].extend(b"\0" * 20)
    obj["header"]["fileLength"] = new_base + len(obj["inst"])
    stream = io.BytesIO()
    hbc.dump(output, stream)
    data = stream.getvalue()
    data = data[:-20] + hashlib.sha1(data[:-20]).digest()
    if not (len(data) == obj["header"]["fileLength"]):
        raise ValueError("Unsupported HBC layout")
    check = hbc.load(io.BytesIO(data))
    if not (check.getObj()["instOffset"] == new_base):
        raise ValueError("Unsupported HBC layout")
    for sid in range(original.getStringCount()):
        if not (original.getString(sid)[0] == check.getString(sid)[0]):
            raise ValueError("Unsupported HBC layout")
    for fid in range(first_new):
        before, after = original.getBareFunction(fid), check.getBareFunction(fid)
        if fid not in (profile["render_function"], profile["press_function"]):
            if not (before[:5] == after[:5]):
                raise ValueError("Unsupported HBC layout")
            for key in ("highestReadCacheIndex", "highestWriteCacheIndex", "flags"):
                if not (before[5][key] == after[5][key]):
                    raise ValueError("Unsupported HBC layout")
    if not (check.getFunction(profile["render_function"])[4] == template):
        raise ValueError("Unsupported HBC layout")
    for fid, (instructions, _, _) in compiled.items():
        if not (check.getFunction(function_map[fid])[4] == instructions):
            raise ValueError("Unsupported HBC layout")

    if not (check.getFunction(profile["press_function"])[4] == press):
        raise ValueError("Unsupported HBC layout")
    return data


def make_recipe(original, patched):
    """Bounded block matching: copy original ranges, store only differing bytes."""
    if not 0 < len(original) <= MAXIMUM or not 0 < len(patched) <= MAXIMUM:
        raise ValueError("固件大小超出上限")
    block = 32
    index = {}
    for position in range(0, len(original) - block + 1, 8):
        index.setdefault(original[position:position + block], position)
    operations, literal, position = [], bytearray(), 0
    while position < len(patched):
        source = index.get(patched[position:position + block]) if position + block <= len(patched) else None
        if source is None:
            literal.append(patched[position]); position += 1
            continue
        if literal:
            operations.append((1, bytes(literal))); literal.clear()
        length = block
        maximum = min(len(original) - source, len(patched) - position)
        for step in (4096, 32, 1):
            while length + step <= maximum and original[source + length:source + length + step] == patched[position + length:position + length + step]:
                length += step
        operations.append((0, source, length))
        position += length
    if literal:
        operations.append((1, bytes(literal)))
    if len(operations) > 100000:
        raise ValueError("补丁操作过多")
    result = bytearray(MAGIC + hashlib.sha256(original).digest() + hashlib.sha256(patched).digest())
    result.extend(struct.pack("<III", len(original), len(patched), len(operations)))
    for op in operations:
        if op[0] == 0:
            result.extend(b"\0" + struct.pack("<II", op[1], op[2]))
        else:
            result.extend(b"\1" + struct.pack("<I", len(op[1])) + op[1])
    return bytes(result)


def apply_recipe(original, recipe):
    """Host cross-check of the same bounded recipe consumed by the device."""
    if len(recipe) < 84 or recipe[:8] != MAGIC or recipe[8:40] != hashlib.sha256(original).digest():
        raise ValueError("无效或不匹配的补丁")
    source_size, output_size, count = struct.unpack_from("<III", recipe, 72)
    if source_size != len(original) or not 0 < output_size <= MAXIMUM or count > 100000:
        raise ValueError("无效补丁边界")
    output, position = bytearray(), 84
    for _ in range(count):
        if position >= len(recipe): raise ValueError("补丁被截断")
        tag = recipe[position]; position += 1
        if tag == 0:
            source, length = struct.unpack_from("<II", recipe, position); position += 8
            if source + length > len(original): raise ValueError("复制越界")
            output.extend(original[source:source + length])
        elif tag == 1:
            length = struct.unpack_from("<I", recipe, position)[0]; position += 4
            if position + length > len(recipe): raise ValueError("补丁被截断")
            output.extend(recipe[position:position + length]); position += length
        else:
            raise ValueError("无效补丁操作")
        if len(output) > output_size: raise ValueError("输出越界")
    if position != len(recipe) or len(output) != output_size or hashlib.sha256(output).digest() != recipe[40:72]:
        raise ValueError("补丁输出校验失败")
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", type=Path, required=True)
    parser.add_argument("--hermesc", type=Path, required=True, help="HBC84 Hermes compiler")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    source = args.original.read_bytes()
    profiles = json.loads((ROOT / "home/firmwares.json").read_text())["profiles"]
    digest = hashlib.sha256(source).hexdigest()
    profile = next((item for item in profiles if item["original_sha256"] == digest), None)
    if profile is None:
        raise ValueError("固件哈希不受支持；未生成或安装补丁")
    with tempfile.TemporaryDirectory() as folder:
        compiled = Path(folder) / "card.hbc"
        subprocess.run([str(args.hermesc.resolve()), "-O", "-g0", "-emit-binary", "-out", str(compiled), str(ROOT / "home/card.js")], check=True)
        patched = merge(source, compiled.read_bytes(), profile)
    recipe = make_recipe(source, patched)
    if apply_recipe(source, recipe) != patched:
        raise ValueError("差分补丁重建不一致")
    manifest = dict(profile, format=1, patch_sha256=hashlib.sha256(recipe).hexdigest(),
                    patched_sha256=hashlib.sha256(patched).hexdigest(),
                    source_sha256=hashlib.sha256((ROOT / "home/card.js").read_bytes()).hexdigest(),
                    modified_original_functions=[profile["render_function"], profile["press_function"]],
                    contains_original_firmware=False, enabled_by_default=False)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in (("patch.rhp", recipe), ("patched.hbc", patched),
                       ("manifest.json", (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode())):
        temporary = args.output_dir / (name + ".tmp")
        temporary.write_bytes(data)
        temporary.replace(args.output_dir / name)
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    os.umask(0o077)
    main()
