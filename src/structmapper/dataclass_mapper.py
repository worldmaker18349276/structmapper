from dataclasses import MISSING, fields, is_dataclass
from typing import (
    Any,
    Dict,
    List,
    get_args,
    get_origin,
    get_type_hints,
)
from collections.abc import Mapping
from inspect import cleandoc
import warnings


def _warn(path, message):
    warnings.warn(f"{path}: {message}", UserWarning)

def _warn_type_mismatch(path, expected, data):
    _warn(path, f"expected {expected}, got {type(data).__name__}")

def from_dict(cls, data, *, path="$"):
    if cls is Any:
        return data

    if is_dataclass(cls):
        if not isinstance(data, dict):
            _warn_type_mismatch(path, f"dict for {cls.__name__}", data)
            return cls()

        type_hints = get_type_hints(cls)

        field_map = {f.name: f for f in fields(cls)}

        kwargs = {}

        for key in data:
            if key not in field_map:
                _warn(f"{path}.{key}", "unknown field")
                continue
            
            field = field_map[key]
            field_path = f"{path}.{key}"
            kwargs[key] = from_dict(type_hints.get(key, field.type), data[key], path=field_path)

        for key in field_map.keys():
            if key not in data:
                _warn(f"{path}.{key}", "missing field")

        return cls(**kwargs)

    origin = get_origin(cls)
    args = get_args(cls)

    # List[T]
    if origin in (list, List):
        if not isinstance(data, list):
            _warn_type_mismatch(path, "list", data)
            return []

        element_type = args[0] if args else Any

        return [
            from_dict(element_type, v, path=f"{path}[{i}]")
            for i, v in enumerate(data)
        ]

    # Dict[K, V]
    if origin in (dict, Dict, Mapping):
        if not isinstance(data, dict):
            _warn_type_mismatch(path, "dict", data)
            return {}

        assert len(args) == 0 or args[0] is str
        value_type = args[1] if len(args) > 1 else Any

        return {
            k: from_dict(value_type, v, path=f"{path}[{k!r}]")
            for k, v in data.items()
        }

    # scalar
    if cls in (type(None), bool, int, float, str):
        if not isinstance(data, cls):
            _warn_type_mismatch(path, cls.__name__, data)
            return cls()
        return data

    raise TypeError(f"unknown type: {cls} ({origin})")


def to_schema(cls):
    origin = get_origin(cls)

    if cls is Any:
        return {}

    if is_dataclass(cls):
        hints = get_type_hints(cls)
        properties = {}
        for f in fields(cls):
            field_schema = to_schema(hints.get(f.name, f.type))
            if f.default is not MISSING:
                field_schema = {**field_schema, "default": f.default}
            elif f.default_factory is not MISSING:
                try:
                    field_schema = {**field_schema, "default": f.default_factory()}
                except Exception:
                    pass
            properties[f.name] = field_schema
        desc = {"description": cleandoc(cls.__doc__)} if cls.__doc__ else {}
        return {
            "type": "object",
            "properties": properties,
            **desc,
        }

    if origin in (list, List):
        args = get_args(cls)
        return {
            "type": "array",
            "items": to_schema(args[0] if len(args) == 1 else Any)
        }

    if origin in (dict, Dict, Mapping):
        args = get_args(cls)
        assert len(args) == 0 or args[0] is str
        return {
            "type": "object",
            "additionalProperties": to_schema(args[1] if len(args) == 2 else Any)
        }

    return dict({
        type(None): {"type": "null"},
        bool: {"type": "boolean"},
        int: {"type": "integer"},
        float: {"type": "number"},
        str: {"type": "string"},
    }[cls])
