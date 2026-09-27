#!/usr/bin/env python3
"""
schema_generator.py -- driver invoked by structmapper_generate_python_schemas().

Resolves each --type spec ("module.path:QualName"), builds a shared `refs`
map so cross-references between the given types come out consistent, calls
`to_schema()` on each, and writes one JSON Schema file per type plus a
manifest.json into --output.
"""
import argparse
import importlib
import json
import re
import sys
from pathlib import Path
from typing import Dict, List

from structmapper.dataclassmapper import to_schema


def _resolve(spec: str) -> type:
    """Resolve "module.path:QualName" to a class object, importing its module."""
    if ":" not in spec:
        raise ValueError(f"Invalid type spec {spec!r}: expected 'module.path:QualName'")
    module_name, qualname = spec.split(":", 1)
    obj: object = importlib.import_module(module_name)
    for part in qualname.split("."):
        obj = getattr(obj, part)
    if not isinstance(obj, type):
        raise TypeError(f"{spec!r} resolved to {obj!r}, which is not a type")
    return obj


def sanitize_identifier(s: str) -> str:
    s = re.sub(r'\W', '_', s, flags=re.ASCII)
    if not s:
        return 'T'
    if s[0].isdigit():
        s = 'T_' + s
    return s

def generate(type_specs: List[str], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)

    classes: Dict[type, str] = {_resolve(spec): spec for spec in type_specs}
    refs: Dict[type, str] = {cls: sanitize_identifier(spec) + ".schema.json" for cls, spec in classes.items()}
    manifest = {"types": {classes[cls]: schema_name for cls, schema_name in refs.items()}}

    for cls, schema_name in refs.items():
        refs_ = dict(refs)
        del refs_[cls]
        schema = to_schema(cls, refs_)
        (output_dir / schema_name).write_text(json.dumps(schema, indent=2, sort_keys=False))

    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=False))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--type",
        dest="types",
        action="append",
        required=True,
        metavar="module.path:QualName",
        help="A type to generate a schema for. Repeatable.",
    )
    parser.add_argument("--output", required=True, type=Path, help="Output directory")
    args = parser.parse_args()

    try:
        generate(args.types, args.output)
    except Exception as exc:
        print(f"schema_generator: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    exit(main())
