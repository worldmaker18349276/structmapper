from contextlib import contextmanager
from dataclasses import MISSING, fields, is_dataclass
import json
from pathlib import Path
from types import ModuleType
from typing import (
    Any,
    Dict,
    List,
    Generic,
    Literal,
    Tuple,
    Type,
    TypeVar,
    Union,
    cast,
    get_args,
    get_origin,
    get_type_hints,
)
from collections.abc import Mapping
import sys
from inspect import cleandoc
import warnings

__all__ = [
    "TypeMismatchWarning", "MissingWarning", "UnknownWarning", "ClassMismatchWarning",
    "JSONScalar", "JSON", "ExternalType",
    "from_json", "FromJson",
    "type_check", "type_check_json", "to_schema",
]

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
P = TypeVar("P")
class _ExternalType(Generic[L, P]):
    ...

class _ExternalTypeProxy:
    def __class_getitem__(cls, item: Any):
        # ExternalType[Literal["xxx"]] -> _ExternalType[Literal["xxx"], Literal["mymodule.py"]]
        literal = get_args(item)
        if len(literal) != 1 or not isinstance(literal[0], str):
            raise TypeError("ExternalType[...] requires Literal[str]")
        caller_frame = sys._getframe(1) # pyright: ignore[reportPrivateUsage]
        literal_filepath = str(caller_frame.f_globals.get("__file__") or "")
        return _ExternalType[Literal[literal[0]], Literal[literal_filepath]]

@contextmanager
def _inject_into_mro_modules(cls: Type[Any], name: str, value: Any):
    """
    Temporarily inject `name = value` into the real __dict__ of every
    module referenced by cls.__mro__, so native get_type_hints (which
    uses sys.modules[base.__module__].__dict__ per base when globalns=None)
    can resolve `name` during eval - then restore each module exactly
    as it was.
    """
    touched: List[Tuple[ModuleType, bool, Any]] = []  # (module, name, had_key, old_value)
    try:
        for base in cls.__mro__:
            module = sys.modules.get(base.__module__)
            if module is None:
                continue
            had_key = name in module.__dict__
            old_value = module.__dict__.get(name)
            touched.append((module, had_key, old_value))
            module.__dict__[name] = value
        yield
    finally:
        for module, had_key, old_value in touched:
            if had_key:
                module.__dict__[name] = old_value
            else:
                module.__dict__.pop(name, None)

def _get_type_hints(cls: Type[Any]) -> Dict[str, Any]:
    with _inject_into_mro_modules(cls, "ExternalType", _ExternalTypeProxy):
        return get_type_hints(cls)


def from_json(cls: Union[type, Any], data: JSON, *, path: str = "$", check_external: bool = False):
    """
    convert json to object in depth, use default value if fails.
    `cls` can be:
    - dataclass with valid type hints for from_json, it must be default constructable.
    - JSON type: NoneType, bool, int, float, str, List[T], Dict[str, T]
    - immutable tuple: Tuple[T, ...]
    - string enum: Literal['option1', 'option2', 'option3']
    - schema type: ExternalType[Literal['path/to/your.schema.json']]
    - Any
    where T is valid type hint for `from_json`.
    
    type will be checked and warnings will be issued if mismatch.
    scalar type must match exactly, so False is not a int.
    if `check_external` is true, the external type will also be checked.
    """
    if cls is Any:
        return data

    if isinstance(cls, type) and is_dataclass(cls):
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, f"struct for {cls.__name__}", type(data).__name__))
            return cls()

        type_hints = _get_type_hints(cls)

        field_map = {f.name: f for f in fields(cls)}

        kwargs: Dict[str, Any] = {}

        for key in field_map.keys():
            field_path = f"{path}.{key}"

            if key not in data:
                warnings.warn(MissingWarning(field_path))
                continue
            
            kwargs[key] = from_json(type_hints.get(key, field_map[key].type), data[key], path=field_path, check_external=check_external)

        for key in data.keys():
            if key not in field_map:
                field_path = f"{path}.{key}"
                warnings.warn(UnknownWarning(field_path))

        return cls(**kwargs)

    origin = get_origin(cls)
    args = get_args(cls)

    if origin is _ExternalType:
        if check_external:
            schema_filepath = get_args(get_args(cls)[0])[0]
            literal_filepath = get_args(get_args(cls)[1])[0]
            type_check_json({"$ref": schema_filepath}, Path(literal_filepath), data, path=path)
        return data

    if origin is Literal:
        if data not in args:
            warnings.warn(TypeMismatchWarning(path, " | ".join(repr(value) for value in args), repr(data)))
            return args[0]
        return data

    # List[T] or Tuple[T, ...]
    if origin in (list, List) or origin in (tuple, Tuple) and len(args) == 0 or origin in (tuple, Tuple) and len(args) == 2 and args[-1] is Ellipsis:
        arr: List[Any] = []
        if not isinstance(data, list):
            warnings.warn(TypeMismatchWarning(path, "list", type(data).__name__))
            return arr

        element_type = args[0] if args else Any

        for i, v in enumerate(data):
            arr.append(from_json(element_type, v, path=f"{path}[{i}]", check_external=check_external))
        return tuple(arr) if origin in (tuple, Tuple) else arr

    # Dict[K, V]
    if origin in (dict, Dict, Mapping):
        obj: Dict[str, Any] = {}
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, "dict", type(data).__name__))
            return obj

        assert len(args) == 0 or args[0] is str
        value_type = args[1] if len(args) > 1 else Any

        for k, v in data.items():
            obj[k] = from_json(value_type, v, path=f"{path}[{k!r}]", check_external=check_external)
        return obj

    # scalar
    if isinstance(cls, type) and cls in (type(None), bool, int, float, str):
        if type(data) != cls:
            warnings.warn(TypeMismatchWarning(path, cls.__name__, type(data).__name__))
            return cls()
        return data

    raise TypeError(f"unknown type: {cls} ({origin})")

DataclassT = TypeVar("DataclassT")
class FromJson:
    """
    make dataclass convertible from json in depth.

    usage:
    @dataclass
    class MyDataclass(FromJson):
        a: int
        b: AnotherDataclass
    
    obj = MyDataclass.from_json(data)
    schema = MyDataclass.to_schema()
    """
    @classmethod
    def from_json(cls: Type[DataclassT], data: JSON) -> DataclassT:
        """
        construct dataclass from json in depth, use default value if fails.
        """
        return cast(DataclassT, from_json(cls, data))
    @classmethod
    def to_schema(cls) -> JSON:
        return to_schema(cls, {})

def type_check(cls: Union[type, Any], data: Any, *, path: str = "$", check_external: bool = False) -> bool:
    """
    Validate `data` against a type hint `cls` like `from_json`, without construct instance.
    """
    if cls is Any:
        return True

    data_class = cast(Type[Any], type(data))
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
            schema_filepath = get_args(get_args(cls)[0])[0]
            literal_filepath = get_args(get_args(cls)[1])[0]
            return type_check_json({"$ref": schema_filepath}, Path(literal_filepath), data, path=path)
        else:
            return True

    if origin is Literal:
        if data not in args:
            warnings.warn(TypeMismatchWarning(path, " | ".join(repr(value) for value in args), repr(data)))
            return False
        return True

    if origin in (list, List) or origin in (tuple, Tuple) and len(args) == 0 or origin in (tuple, Tuple) and len(args) == 2 and args[-1] is Ellipsis:
        if not isinstance(data, list):
            warnings.warn(ClassMismatchWarning(path, list, data_class))
            return False
        element_type = args[0] if args else Any
        ok = True
        for i, v in enumerate(cast(List[Any], data)):
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
        for k, v in cast(Dict[str, Any], data).items():
            if not type_check(value_type, v, path=f"{path}[{k!r}]", check_external=check_external):
                ok = False
        return ok

    if isinstance(cls, type) and cls in (type(None), bool, int, float, str):
        if type(data) != cls:
            warnings.warn(ClassMismatchWarning(path, cls, data_class))
            return False
        return True

    raise TypeError(f"unknown type: {cls} ({origin})")

def type_check_json(schema: Dict[Any, Any], schema_path: Path, data: JSON, *, path: str = "$") -> bool:
    """Validate `data` against a JSON schema dict, but only the shapes
    that `to_schema` actually produces.
    """

    # {} == Any
    if schema.get("type") is None:
        return True

    # ExternalType[...] - validated against a schema defined elsewhere
    if isinstance(ref := schema.get("$ref"), str):
        inner_schema_path = schema_path.parent / ref
        with open(inner_schema_path, "r") as fp:
            inner_schema = json.load(fp)
            return type_check_json(inner_schema, inner_schema_path, data, path=path)

    if isinstance(anyOf := schema.get("anyOf"), list) and anyOf and isinstance(inner_schema := cast(Any, anyOf[0]), dict):
        return type_check_json(cast(Dict[Any, Any], inner_schema), schema_path, data, path=path)

    if "const" in schema:
        return schema["const"] == data

    if isinstance(enum := schema.get("enum"), list):
        return data in enum

    stype = schema.get("type")

    if stype == "object" and isinstance(properties := schema.get("properties"), dict):
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, "struct", type(data).__name__))
            return False

        ok = True
        for key, field_schema in cast(Dict[Any, Any], properties).items():
            if not isinstance(key, str): continue
            if not isinstance(field_schema, dict): continue
            field_path = f"{path}.{key}"
            if key not in data:
                warnings.warn(MissingWarning(field_path))
                ok = False
                continue
            if not type_check_json(cast(Dict[Any, Any], field_schema), schema_path, data[key], path=field_path):
                ok = False

        for key in data.keys():
            if key not in properties:
                warnings.warn(UnknownWarning(f"{path}.{key}"))
                ok = False

        return ok

    if stype == "object" and isinstance(value_schema := schema.get("additionalProperties"), dict):
        if not isinstance(data, dict):
            warnings.warn(TypeMismatchWarning(path, "dict", type(data).__name__))
            return False

        ok = True
        for k, v in data.items():
            if not type_check_json(cast(Dict[Any, Any], value_schema), schema_path, v, path=f"{path}[{k!r}]"):
                ok = False
        return ok

    if stype == "array" and isinstance(item_schema := schema.get("items", {}), dict):
        if not isinstance(data, list):
            warnings.warn(TypeMismatchWarning(path, "list", type(data).__name__))
            return False

        ok = True
        for i, v in enumerate(data):
            if not type_check_json(cast(Dict[Any, Any], item_schema), schema_path, v, path=f"{path}[{i}]"):
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

def _count_type(cls: Any, counts: Dict[type, int], refs: Dict[type, str]) -> None:
    origin = get_origin(cls)
    args = get_args(cls)

    if isinstance(cls, type) and is_dataclass(cls):
        if cls in refs: return
        counts[cls] = counts.get(cls, 0) + 1
        if counts[cls] >= 2: return # already visited

        # Still traverse it for counting nested types.
        hints = _get_type_hints(cls)
        for f in fields(cls):
            _count_type(hints.get(f.name, f.type), counts, refs)
        return

    if origin in (list, List) or origin in (tuple, Tuple) and len(args) == 0 or origin in (tuple, Tuple) and len(args) == 2 and args[1] is Ellipsis:
        _count_type(args[0] if len(args) == 1 else Any, counts, refs)
        return

    if origin in (dict, Dict, Mapping):
        assert len(args) == 0 or len(args) == 2 and args[0] is str
        _count_type(args[1] if len(args) == 2 else Any, counts, refs)
        return

    # Any, scalars, Literals, External types, etc.
    return

def _make_schema(cls: Any, defs_root: bool, refs: Dict[type, str]) -> Dict[str, JSON]:
    origin = get_origin(cls)
    args = get_args(cls)

    schema: Dict[str, JSON] = {}

    if cls is Any:
        return schema

    if isinstance(cls, type) and is_dataclass(cls):
        if not defs_root and cls in refs:
            return {"$ref": refs[cls]}

        hints = _get_type_hints(cls)
        properties: Dict[str, JSON] = {}
        for f in fields(cls):
            field_schema = _make_schema(hints.get(f.name, f.type), False, refs)
            if f.default is not MISSING:
                field_schema = cast(Dict[str, JSON], {**field_schema, "default": f.default})
            properties[f.name] = field_schema
        desc = {"description": cleandoc(cls.__doc__)} if cls.__doc__ else {}
        schema = {
            "type": "object",
            "properties": properties,
            **desc,
        }
        return schema

    if origin is _ExternalType:
        schema_filepath = get_args(args[0])[0]
        literal_filepath = get_args(args[1])[0]
        path = Path(literal_filepath).parent / Path(schema_filepath)
        return {"$ref": str(path)}

    if origin is Literal:
        if len(args) == 1:
            return {"const": args[0]}
        else:
            return {"enum": [value for value in args]}

    if origin in (list, List) or origin in (tuple, Tuple) and len(args) == 0 or origin in (tuple, Tuple) and len(args) == 2 and args[-1] is Ellipsis:
        schema = {
            "type": "array",
            "items": _make_schema(args[0] if len(args) == 1 else Any, False, refs)
        }
        return schema

    if origin in (dict, Dict, Mapping):
        assert len(args) == 0 or len(args) == 2 and args[0] is str
        schema = {
            "type": "object",
            "additionalProperties": _make_schema(args[1] if len(args) == 2 else Any, False, refs)
        }
        return schema

    SCALAR_SCHEMA: Dict[type, Dict[str, JSON]] = {
        type(None): {"type": "null"},
        bool: {"type": "boolean"},
        int: {"type": "integer"},
        float: {"type": "number"},
        str: {"type": "string"},
    }
    if cls in SCALAR_SCHEMA:
        return dict(SCALAR_SCHEMA[cls])

    raise TypeError(f"unknown type {cls} ({origin})")

def _choose_unique_defs_name(cls: type, defs_: Dict[str, type]) -> str:
    name = cls.__name__
    if name in defs_.keys():
        i = 2
        while (candidate := f"{name}_{i}") in defs_.keys():
            i += 1
        name = candidate
    return name

def to_schema(cls: Union[type, Any], refs: Dict[type, str]) -> Dict[str, JSON]:
    """
    convert type hints (valid for `from_json`) to JSON schema.
    
        Any                       -> {}
        NoneType                  -> {"type": "null"}
        scalar                    -> {"type": "boolean" | "integer" | "number" | "string"}
        dataclass                 -> {"type": "object", "properties": {...}}
        Dict[str, V]              -> {"type": "object", "additionalProperties": {...}}
        List[T], Tuple[T, ...]    -> {"type": "array", "items": {...}}
        ExternalType[...]         -> {"$ref": "..."}
        Literal["...", ...]       -> {"enum": [...]}
        Literal["..."]            -> {"const": ...}
    
    description of dataclass will be appended if exists.

    """
    counts: Dict[type, int] = {}
    _count_type(cls, counts, refs)
    refs = dict(refs)
    defs_: Dict[str, type] = {}
    for typ, count in counts.items():
        if count >= 2 and typ not in refs:
            name = _choose_unique_defs_name(typ, defs_)
            defs_[name] = typ
            refs[typ] = f"#/$defs/{name}"
    defs: Dict[str, JSON] = {}
    for name, typ in defs_.items():
        defs[name] = _make_schema(typ, True, refs)
    root = _make_schema(cls, False, refs)
    root["$defs"] = defs
    return root
