#!/usr/bin/env python3
"""Independent restore-codec format vectors; no Plinth imports or file writes.

This deliberately small reference emits exact canonical JSON and complete
domain-separated preimage hex alongside SHA256. It is not a package walker,
policy validator, trust decision or production encoding implementation.
"""

import hashlib
import json
import struct


def be32(number):
    return struct.pack(">I", number)


def be64(number):
    return struct.pack(">Q", number)


def artifact_preimage(records):
    encoded = b"plinth.restore.artifact\0" + be32(1) + be32(len(records))
    for kind, name, mode, content in sorted(
            records, key=lambda record: record[1].encode("utf-8")):
        path = name.encode("utf-8")
        encoded += bytes([kind]) + be32(len(path)) + path + be32(mode)
        if kind == 2:
            encoded += be64(len(content)) + content
    return encoded


def schema_preimage(schema):
    # Valid scalar Unicode's code-point order equals unsigned UTF-8 byte order.
    # Python's ensure_ascii=False keeps UTF-8/U+2028/U+2029; control/quote/solidus
    # escaping below follows json.dumps' specified compact JSON representation.
    normalized = {
        **schema,
        "persist_keys": sorted(schema["persist_keys"],
                               key=lambda key: key.encode("utf-8")),
    }
    encoded = json.dumps(normalized, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return encoded, b"plinth.restore.schema\0" + be32(1) + encoded


def main():
    vectors = {
        "empty_artifact": artifact_preimage([]),
        "binary_artifact": artifact_preimage([
            (2, "hello.txt", 0o644, b"A\0B\n"),
        ]),
        "complete_inventory": artifact_preimage([
            (2, "é/δ", 0o600, b"z\n"),
            (1, "z-empty", 0o555, b""),
            (2, "app/main.js", 0o644, b"x\0\xff"),
            (1, ".hidden", 0o700, b""),
            (1, "é", 0o750, b""),
            (2, ".hidden/empty", 0, b""),
            (1, "app", 0o755, b""),
        ]),
    }
    schemas = {
        "empty_schema": {"fields": {}, "persist_keys": [], "version": 1},
        "unicode_schema": {
            "fields": {
                "z": {"required": False, "max_bytes": 0, "meaning": "a\0b"},
                "é": {"required": True, "max_bytes": 512,
                      "meaning": "café/\"\\\b\t\n\f\r\x01\x1f\u2028\u2029"},
                "😀": {"required": True, "max_bytes": 1, "meaning": "emoji"},
            },
            "persist_keys": ["z", "😀"],
            "version": 1,
        },
    }
    output = {}
    for name, schema in schemas.items():
        canonical, vectors[name] = schema_preimage(schema)
        output[name] = {"canonical_json_hex": canonical.hex()}
    for name, preimage in vectors.items():
        output.setdefault(name, {}).update(
            preimage_hex=preimage.hex(), sha256=hashlib.sha256(preimage).hexdigest())
    print(json.dumps(output, ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
