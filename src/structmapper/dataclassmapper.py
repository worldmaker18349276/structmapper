from dataclasses import MISSING, fields, is_dataclass
from typing import (
    Any,
    Dict,
    List,
    Union,
    get_args,
    get_origin,
    get_type_hints,
)
from collections.abc import Mapping
from inspect import cleandoc
import warnings

class TypeMismatchWarning(Warning):
    def __init__(self, path: str, expected: str, data: Any):
        self.path = path
        self.expected = expected
        self.data = data

    def __str__(self):
        return f"{self.path}: expected {self.expected}, got {type(self.data).__name__}"

class MissingWarning(Warning):
    def __init__(self, path: str):
        self.path = path

    def __str__(self):
        return f"{self.path}: missing field"

class UnknownWarning(Warning):
    def __init__(self, path: str):
        self.path = path

    def __str__(self):
        return f"{self.path}: unknown field"

def from_dict(cls: Union[type, Any], data: Any, *, path: str = "$"):
    if cls is Any:
        return data

    if isinstance(cls, type) and is_dataclass(cls):
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, f"dict for {cls.__name__}", data))
            return cls()

        type_hints = get_type_hints(cls)

        field_map = {f.name: f for f in fields(cls)}

        kwargs = {}

        for key in field_map.keys():
            field_path = f"{path}.{key}"

            if key not in data:
                warnings.warn(MissingWarning(field_path))
                continue
            
            kwargs[key] = from_dict(type_hints.get(key, field_map[key].type), data[key], path=field_path)

        for key in data.keys(): # type: ignore
            if isinstance(key, str) and key not in field_map:
                field_path = f"{path}.{key}"
                warnings.warn(UnknownWarning(field_path))

        return cls(**kwargs)

    origin = get_origin(cls)
    args = get_args(cls)

    # List[T]
    if origin in (list, List):
        arr: List[Any] = []
        if not isinstance(data, list):
            warnings.warn(TypeMismatchWarning(path, "list", data))
            return arr

        element_type = args[0] if args else Any

        for i, v in enumerate(data): # type: ignore
            arr.append(from_dict(element_type, v, path=f"{path}[{i}]"))
        return arr

    # Dict[K, V]
    if origin in (dict, Dict, Mapping):
        obj: Dict[str, Any] = {}
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, "dict", data))
            return obj

        assert len(args) == 0 or args[0] is str
        value_type = args[1] if len(args) > 1 else Any

        for k, v in data.items(): # type: ignore
            obj[k] = from_dict(value_type, v, path=f"{path}[{k!r}]")
        return obj

    # scalar
    if isinstance(cls, type) and cls in (type(None), bool, int, float, str):
        if not isinstance(data, cls):
            warnings.warn(TypeMismatchWarning(path, cls.__name__, data))
            return cls()
        return data

    raise TypeError(f"unknown type: {cls} ({origin})")


def to_schema(cls: Union[type, Any]) -> Dict[str, Any]:
    origin = get_origin(cls)

    schema: Dict[str, Any] = {}

    if cls is Any:
        return schema

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
        schema = {
            "type": "object",
            "properties": properties,
            **desc,
        }
        return schema

    if origin in (list, List):
        args = get_args(cls)
        schema = {
            "type": "array",
            "items": to_schema(args[0] if len(args) == 1 else Any)
        }
        return schema

    if origin in (dict, Dict, Mapping):
        args = get_args(cls)
        assert len(args) == 0 or args[0] is str
        schema = {
            "type": "object",
            "additionalProperties": to_schema(args[1] if len(args) == 2 else Any)
        }
        return schema

    return dict({
        type(None): {"type": "null"},
        bool: {"type": "boolean"},
        int: {"type": "integer"},
        float: {"type": "number"},
        str: {"type": "string"},
    }[cls])
