from dataclasses import MISSING, fields, is_dataclass
import json
from typing import (
    Any,
    Dict,
    List,
    Generic,
    Literal,
    Optional,
    Type,
    TypeVar,
    Union,
    get_args,
    get_origin,
    get_type_hints,
)
from collections.abc import Mapping
import sys
from inspect import cleandoc
import warnings

class TypeMismatchWarning(Warning):
    def __init__(self, path: str, expected: str, got: str):
        self.path = path
        self.expected = expected
        self.got = got

    def __str__(self):
        return f"{self.path}: expected {self.expected}, got {self.got}"

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

class ClassMismatchWarning(Warning):
    def __init__(self, path: str, expected: type, got: type):
        self.path = path
        self.expected = expected
        self.got = got

    def __str__(self):
        return f"{self.path}: expected {self.expected.__name__}, got {self.got.__name__}"

JSONScalar = Union[bool, int, float, str] # int, float are different, nan, inf are allowed
JSON = Union[None, JSONScalar, List["JSON"], Dict[str, "JSON"]]

# usage:
# value_with_external_type: 'ExternalType[Literal["path/to/external.schema.json"]]' = ...
# you must quote it, even if forward reference is enabled
ExternalType = Any

L = TypeVar("L")

class _ExternalType(Generic[L]):
    ...

class _ExternalTypeProxy:
    def __class_getitem__(cls, item: Any):
        # ExternalType[Literal["foo.json"]]
        literal = get_args(item)
        if len(literal) != 1 or not isinstance(literal[0], str):
            raise TypeError("ExternalType[...] requires Literal[str]")
        return _ExternalType[Literal[literal[0]]]

def _get_type_hints(
    cls: Type[Any],
    *,
    globalns: Optional[Dict[str, Any]] = None,
    localns: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    module = sys.modules.get(cls.__module__)
    globalns = dict(vars(module)) if module is not None else {}
    globalns["ExternalType"] = _ExternalTypeProxy

    return get_type_hints(
        cls,
        globalns=globalns,
        localns=localns,
    )

def from_dict(cls: Union[type, Any], data: JSON, *, path: str = "$", check_external: bool = False):
    if cls is Any:
        return data

    if isinstance(cls, type) and is_dataclass(cls):
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, f"struct for {cls.__name__}", type(data).__name__))
            return cls()

        type_hints = _get_type_hints(cls)

        field_map = {f.name: f for f in fields(cls)}

        kwargs = {}

        for key in field_map.keys():
            field_path = f"{path}.{key}"

            if key not in data:
                warnings.warn(MissingWarning(field_path))
                continue
            
            kwargs[key] = from_dict(type_hints.get(key, field_map[key].type), data[key], path=field_path, check_external=check_external)

        for key in data.keys(): # type: ignore
            if key not in field_map:
                field_path = f"{path}.{key}"
                warnings.warn(UnknownWarning(field_path))

        return cls(**kwargs)

    origin = get_origin(cls)
    args = get_args(cls)

    if origin is _ExternalType:
        if check_external:
            path = get_args(get_args(cls)[0])[0]
            type_check_json({"$ref": path}, data, path=path)
        return data

    # List[T]
    if origin in (list, List):
        arr: List[Any] = []
        if not isinstance(data, list):
            warnings.warn(TypeMismatchWarning(path, "list", type(data).__name__))
            return arr

        element_type = args[0] if args else Any

        for i, v in enumerate(data): # type: ignore
            arr.append(from_dict(element_type, v, path=f"{path}[{i}]", check_external=check_external))
        return arr

    # Dict[K, V]
    if origin in (dict, Dict, Mapping):
        obj: Dict[str, Any] = {}
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, "dict", type(data).__name__))
            return obj

        assert len(args) == 0 or args[0] is str
        value_type = args[1] if len(args) > 1 else Any

        for k, v in data.items(): # type: ignore
            obj[k] = from_dict(value_type, v, path=f"{path}[{k!r}]", check_external=check_external)
        return obj

    # scalar
    if isinstance(cls, type) and cls in (type(None), bool, int, float, str):
        if type(data) != cls:
            warnings.warn(TypeMismatchWarning(path, cls.__name__, type(data).__name__))
            return cls()
        return data

    raise TypeError(f"unknown type: {cls} ({origin})")

def type_check(cls: Union[type, Any], data: Any, *, path: str = "$", check_external: bool = False) -> bool:
    if cls is Any:
        return True

    data_class: type = type(data) # type: ignore
    if isinstance(cls, type) and is_dataclass(cls):
        if not isinstance(data, cls):
            warnings.warn(ClassMismatchWarning(path, cls, data_class))
            return False

        type_hints = _get_type_hints(cls)
        field_map = {f.name: f for f in fields(cls)}
        ok = True

        for key in field_map.keys():
            field_path = f"{path}.{key}"
            if not type_check(type_hints.get(key, field_map[key].type), getattr(data, key), path=field_path, check_external=check_external):
                ok = False

        return ok

    origin = get_origin(cls)
    args = get_args(cls)

    if origin is _ExternalType:
        if check_external:
            path = get_args(get_args(cls)[0])[0]
            return type_check_json({"$ref": path}, data, path=path)
        else:
            return True

    if origin in (list, List):
        if not isinstance(data, list):
            warnings.warn(ClassMismatchWarning(path, list, data_class))
            return False
        element_type = args[0] if args else Any
        ok = True
        for i, v in enumerate(data): # type: ignore
            if not type_check(element_type, v, path=f"{path}[{i}]", check_external=check_external):
                ok = False
        return ok

    if origin in (dict, Dict, Mapping):
        if not isinstance(data, dict):
            warnings.warn(ClassMismatchWarning(path, dict, data_class))
            return False
        assert len(args) == 0 or args[0] is str
        value_type = args[1] if len(args) > 1 else Any
        ok = True
        for k, v in data.items(): # type: ignore
            if not type_check(value_type, v, path=f"{path}[{k!r}]", check_external=check_external):
                ok = False
        return ok

    if isinstance(cls, type) and cls in (type(None), bool, int, float, str):
        if type(data) != cls:
            warnings.warn(ClassMismatchWarning(path, cls, data_class))
            return False
        return True

    raise TypeError(f"unknown type: {cls} ({origin})")

def type_check_json(schema: Dict[Any, Any], data: JSON, *, path: str = "$") -> bool:
    """Validate `data` against a JSON schema dict, but only the shapes
    that `to_schema` actually produces - this is NOT a general JSON
    Schema validator. Recognized forms:

        {}                                                    -> Any
        {"type": "null"}                                      -> None
        {"type": "boolean" | "integer" | "number" | "string"} -> scalar
        {"type": "object", "properties": {...}}               -> dataclass
        {"type": "object", "additionalProperties": {...}}     -> Dict[str, V]
        {"type": "array", "items": {...}}                     -> List[T]
        {"$ref": "..."}                                       -> ExternalType[...], opaque
        {"anyOf": [{...}]}                                    -> wrapped type

    Uses the same warning classes and the same loose numeric
    convertibility as `type_check`, and returns True iff no mismatch
    was found anywhere in the structure.
    """

    # {} == Any
    if not schema:
        return True

    # ExternalType[...] - validated against a schema defined elsewhere
    if isinstance(ref := schema.get("$ref"), str):
        with open(ref, "r") as fp:
            inner_schema = json.load(fp)
            return type_check_json(inner_schema, data, path=path)

    if isinstance(anyOf := schema.get("anyOf"), list) and anyOf and isinstance(inner_schema := anyOf[0], dict): # type: ignore
        return type_check_json(inner_schema, data, path=path) # type: ignore

    stype = schema.get("type")

    if stype == "object" and isinstance(properties := schema.get("properties"), dict):
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, "struct", type(data).__name__))
            return False

        ok = True
        for key, field_schema in properties.items(): # type: ignore
            if not isinstance(key, str): continue
            if not isinstance(field_schema, dict): continue
            field_path = f"{path}.{key}"
            if key not in data:
                warnings.warn(MissingWarning(field_path))
                ok = False
                continue
            if not type_check_json(field_schema, data[key], path=field_path): # type: ignore
                ok = False

        for key in data.keys(): # type: ignore
            if key not in properties:
                warnings.warn(UnknownWarning(f"{path}.{key}"))
                ok = False

        return ok

    if stype == "object" and isinstance(value_schema := schema.get("additionalProperties"), dict):
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, "dict", type(data).__name__))
            return False

        ok = True
        for k, v in data.items(): # type: ignore
            if not type_check_json(value_schema, v, path=f"{path}[{k!r}]"): # type: ignore
                ok = False
        return ok

    if stype == "array" and isinstance(item_schema := schema.get("items", {}), dict):
        if not isinstance(data, list):
            warnings.warn(TypeMismatchWarning(path, "list", type(data).__name__))
            return False

        ok = True
        for i, v in enumerate(data): # type: ignore
            if not type_check_json(item_schema, v, path=f"{path}[{i}]"): # type: ignore
                ok = False
        return ok

    SCALAR_TYPES: Dict[str, type] = {
        "null": type(None),
        "boolean": bool,
        "integer": int,
        "number": float,
        "string": str,
    }
    if stype in SCALAR_TYPES:
        if type(data) != SCALAR_TYPES[stype]:
            warnings.warn(TypeMismatchWarning(path, stype, type(data).__name__))
            return False
        return True

    raise TypeError(f"unrecognized schema: {schema!r}")

def to_schema(cls: Union[type, Any]) -> Dict[str, JSON]:
    origin = get_origin(cls)

    schema: Dict[str, Any] = {}

    if cls is Any:
        return schema

    if isinstance(cls, type) and is_dataclass(cls):
        hints = _get_type_hints(cls)
        properties = {}
        for f in fields(cls):
            field_schema = to_schema(hints.get(f.name, f.type))
            if f.default is not MISSING:
                field_schema = {**field_schema, "default": f.default} # type: ignore
            properties[f.name] = field_schema
        desc = {"description": cleandoc(cls.__doc__)} if cls.__doc__ else {}
        schema = {
            "type": "object",
            "properties": properties,
            **desc,
        }
        return schema

    if origin is _ExternalType:
        path = get_args(get_args(cls)[0])[0]
        return {"$ref": path}

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
