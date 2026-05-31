#!/usr/bin/env python3
# tests/agent-eval/validate_schema.py — minimal JSON Schema (draft-07 subset)
# validator, pure stdlib. Lets the bats suites conformance-check eval cases +
# runner results against docs/agent-eval/*-schema.json WITHOUT a third-party
# validator (plan docs/plans/active/subagent-eval-harness.md § Verification).
#
# Supports only the keywords those schemas actually use:
#   $ref (local #/definitions/...), type (string or list), required,
#   properties, additionalProperties:false, items, enum, const, minItems,
#   minLength, minimum, exclusiveMinimum, allOf, if/then.
#
# Usage:   python tests/agent-eval/validate_schema.py <schema.json> <doc.json>
# Exit:    0 valid · 1 schema violation · 2 usage / IO error.

from __future__ import annotations

import json
import sys
from typing import Any, List


class SchemaViolation(ValueError):
    pass


TYPEMAP = {
    "object": dict,
    "array": list,
    "string": str,
    "integer": int,
    "number": (int, float),
    "boolean": bool,
    "null": type(None),
}


def fail(msg: str, path: List[str]) -> None:
    raise SchemaViolation(f"{'/'.join(path) or '<root>'}: {msg}")


def resolve(root: dict, schema: Any) -> Any:
    seen = 0
    while isinstance(schema, dict) and "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/"):
            fail(f"unsupported $ref {ref}", [])
        node: Any = root
        for part in ref[2:].split("/"):
            node = node[part]
        schema = node
        seen += 1
        if seen > 50:
            fail("$ref cycle", [])
    return schema


def check_type(t: Any, val: Any, path: List[str]) -> None:
    types = t if isinstance(t, list) else [t]
    for tt in types:
        py = TYPEMAP[tt]
        # bool is a subclass of int — never let it satisfy integer/number.
        if isinstance(val, bool):
            if tt == "boolean":
                return
            continue
        if isinstance(val, py):
            return
    fail(f"expected type {t}, got {type(val).__name__}", path)


def validate(root: dict, schema: Any, val: Any, path: List[str]) -> None:
    schema = resolve(root, schema)
    if "type" in schema:
        check_type(schema["type"], val, path)
    if "const" in schema and val != schema["const"]:
        fail(f"const mismatch: expected {schema['const']!r}", path)
    if "enum" in schema and val not in schema["enum"]:
        fail(f"{val!r} not in enum {schema['enum']}", path)
    if isinstance(val, str) and "minLength" in schema and len(val) < schema["minLength"]:
        fail("string shorter than minLength", path)
    if isinstance(val, (int, float)) and not isinstance(val, bool):
        if "minimum" in schema and val < schema["minimum"]:
            fail("below minimum", path)
        if "exclusiveMinimum" in schema and val <= schema["exclusiveMinimum"]:
            fail("not above exclusiveMinimum", path)
    if isinstance(val, list):
        if "minItems" in schema and len(val) < schema["minItems"]:
            fail("fewer than minItems", path)
        if "items" in schema:
            for i, item in enumerate(val):
                validate(root, schema["items"], item, path + [str(i)])
    if isinstance(val, dict):
        for req in schema.get("required", []):
            if req not in val:
                fail(f"missing required '{req}'", path)
        props = schema.get("properties", {})
        for k, v in val.items():
            if k in props:
                validate(root, props[k], v, path + [k])
            elif schema.get("additionalProperties") is False:
                fail(f"additional property '{k}' not allowed", path)
    for sub in schema.get("allOf", []):
        sub = resolve(root, sub)
        if "if" in sub:
            try:
                validate(root, sub["if"], val, path)
                matched = True
            except SchemaViolation:
                matched = False
            if matched and "then" in sub:
                validate(root, sub["then"], val, path)
        else:
            validate(root, sub, val, path)


def main(argv: List[str]) -> int:
    if len(argv) != 2:
        print("usage: validate_schema.py <schema.json> <doc.json>", file=sys.stderr)
        return 2
    try:
        with open(argv[0], encoding="utf-8") as f:
            schema = json.load(f)
        with open(argv[1], encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    try:
        validate(schema, schema, doc, [])
    except SchemaViolation as exc:
        print(f"SCHEMA VIOLATION: {exc}", file=sys.stderr)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
