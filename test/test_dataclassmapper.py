import unittest
from dataclasses import dataclass, field
from typing import Any, Dict, List, Literal, Mapping
from unittest.mock import patch, mock_open
import json

from structmapper.dataclassmapper import ExternalType, from_dict, to_schema, TypeMismatchWarning, UnknownWarning, MissingWarning, type_check


@dataclass
class Pose:
    x: float
    y: float


@dataclass
class Config:
    """
    config
    """
    name: str
    pose: Pose
    poses: List[Pose]
    values: Dict[str, float]
    enabled: bool


@dataclass
class Defaults:
    name: str = "default"
    count: int = 42
    tags: List[str] = field(default_factory=lambda: [])


TEST_SCHEMA = { # type: ignore
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "type": "object",
    "properties": {
        "my_integer": {"type": "integer"},
        "my_float": {"type": "number"},
        "my_nan": {"type": "number"},
        "my_inf": {"type": "number"},
        "my_path": {"type": "string"},
        "my_sub": {
            "type": "object",
            "properties": {
                "my_str": {"type": "string"},
                "my_vector": {
                    "type": "object",
                    "properties": {
                        "x": {"type": "number"},
                        "y": {"type": "number"},
                        "z": {"type": "number"},
                    },
                },
            },
        },
        "my_arr": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "a": {"type": "number"},
                    "b": {"type": "number"},
                    "c": {"type": "number"},
                },
            },
        },
    },
}

@dataclass
class WithExternal:
    i: int
    x: bool
    y: 'ExternalType[Literal["test_schema.schema.json"]]' = field(default_factory=lambda: None)


class TestFromDict(unittest.TestCase):

    def test_any(self):
        self.assertEqual(from_dict(Any, 123), 123)
        self.assertEqual(from_dict(Any, {"a": 1}), {"a": 1})

    def test_scalar(self):
        self.assertEqual(from_dict(int, 123), 123)
        self.assertEqual(from_dict(float, 1.5), 1.5)
        self.assertEqual(from_dict(str, "hello"), "hello")
        self.assertEqual(from_dict(bool, True), True)
        self.assertIsNone(from_dict(type(None), None))

    def test_scalar_type_mismatch(self):
        with self.assertWarnsRegex(
            TypeMismatchWarning,
            r"^\$: expected int, got str$",
        ):
            result = from_dict(int, "123")

        self.assertEqual(result, 0)

    def test_dataclass(self):
        result = from_dict(
            Pose,
            {
                "x": 1.0,
                "y": 2.0,
            },
        )

        self.assertEqual(result, Pose(1.0, 2.0))

    def test_nested_dataclass(self):
        result = from_dict(
            Config,
            {
                "name": "test",
                "pose": {
                    "x": 1.0,
                    "y": 2.0,
                },
                "poses": [
                    {"x": 3.0, "y": 4.0},
                    {"x": 5.0, "y": 6.0},
                ],
                "values": {
                    "a": 1.0,
                    "b": 2.0,
                },
                "enabled": True,
            },
        )

        self.assertEqual(
            result,
            Config(
                name="test",
                pose=Pose(1.0, 2.0),
                poses=[
                    Pose(3.0, 4.0),
                    Pose(5.0, 6.0),
                ],
                values={
                    "a": 1.0,
                    "b": 2.0,
                },
                enabled=True,
            ),
        )

    def test_nested_scalar_type_mismatch(self):
        with self.assertWarnsRegex(
            TypeMismatchWarning,
            r"^\$\.x: expected float, got str$",
        ):
            result = from_dict(
                Pose,
                {
                    "x": "bad",
                    "y": 2.0,
                },
            )

        self.assertEqual(result, Pose(0.0, 2.0))

    def test_unknown_field(self):
        with self.assertWarnsRegex(
            UnknownWarning,
            r"^\$\.unknown: unknown field$",
        ):
            result = from_dict(
                Pose,
                {
                    "x": 1.0,
                    "y": 2.0,
                    "unknown": 123,
                },
            )

        self.assertEqual(result, Pose(1.0, 2.0))

    def test_missing_required_field(self):
        with self.assertWarnsRegex(
            MissingWarning,
            r"^\$\.y: missing field$",
        ):
            with self.assertRaises(TypeError):
                from_dict(
                    Pose,
                    {
                        "x": 1.0,
                    },
                )

    def test_missing_field_with_default(self):
        with self.assertWarnsRegex(
            MissingWarning,
            r"^\$\.name: missing field$",
        ):
            result = from_dict(
                Defaults,
                {
                    "count": 10,
                },
            )

        self.assertTrue(isinstance(result, Defaults))
        assert isinstance(result, Defaults)
        self.assertEqual(result.name, "default")
        self.assertEqual(result.count, 10)
        self.assertEqual(result.tags, [])

    def test_list(self):
        self.assertEqual(
            from_dict(List[int], [1, 2, 3]),
            [1, 2, 3],
        )

    def test_nested_list(self):
        self.assertEqual(
            from_dict(
                List[Pose],
                [
                    {"x": 1.0, "y": 2.0},
                    {"x": 3.0, "y": 4.0},
                ],
            ),
            [
                Pose(1.0, 2.0),
                Pose(3.0, 4.0),
            ],
        )

    def test_list_type_mismatch(self):
        with self.assertWarnsRegex(
            TypeMismatchWarning,
            r"^\$: expected list, got dict$",
        ):
            result = from_dict(List[int], {})

        self.assertEqual(result, [])

    def test_list_element_type_mismatch(self):
        with self.assertWarnsRegex(
            TypeMismatchWarning,
            r"^\$\[1\]: expected int, got str$",
        ):
            result = from_dict(
                List[int],
                [1, "bad", 3],
            )

        self.assertEqual(result, [1, 0, 3])

    def test_dict(self):
        self.assertEqual(
            from_dict(
                Dict[str, int],
                {"a": 1, "b": 2},
            ),
            {"a": 1, "b": 2},
        )

    def test_nested_dict(self):
        self.assertEqual(
            from_dict(
                Dict[str, Pose],
                {
                    "first": {"x": 1.0, "y": 2.0},
                    "second": {"x": 3.0, "y": 4.0},
                },
            ),
            {
                "first": Pose(1.0, 2.0),
                "second": Pose(3.0, 4.0),
            },
        )

    def test_dict_value_type_mismatch(self):
        with self.assertWarnsRegex(
            TypeMismatchWarning,
            r"^\$\['a'\]: expected float, got str$",
        ):
            result = from_dict(
                Dict[str, float],
                {
                    "a": "bad",
                    "b": 2.0,
                },
            )

        self.assertEqual(
            result,
            {
                "a": 0.0,
                "b": 2.0,
            },
        )

    def test_dict_structure_mismatch(self):
        with self.assertWarnsRegex(
            TypeMismatchWarning,
            r"^\$: expected dict, got list$",
        ):
            result = from_dict(Dict[str, int], [])

        self.assertEqual(result, {})

    def test_mapping(self):
        self.assertEqual(
            from_dict(
                Mapping[str, int],
                {"a": 1, "b": 2},
            ),
            {"a": 1, "b": 2},
        )

    def test_complex_structure(self):
        with patch("warnings.warn") as warn:
            result = from_dict(
                Config,
                {
                    "name": "test",
                    "pose": {
                        "x": 1.0,
                        "y": "bad",
                        "extra": True,
                    },
                    "poses": [
                        {"x": 2.0, "y": 3.0},
                        {"x": "bad", "y": 5.0},
                    ],
                    "values": {
                        "a": 1.0,
                        "b": "bad",
                    },
                    "enabled": True,
                    "unknown": 123,
                },
            )

        messages = [str(call.args[0]) for call in warn.call_args_list]

        self.assertEqual(
            messages,
            [
                "$.pose.y: expected float, got str",
                "$.pose.extra: unknown field",
                "$.poses[1].x: expected float, got str",
                "$.values['b']: expected float, got str",
                "$.unknown: unknown field",
            ],
        )

        self.assertTrue(isinstance(result, Config))
        assert isinstance(result, Config)
        self.assertEqual(
            result,
            Config(
                name="test",
                pose=Pose(1.0, 0.0),
                poses=[
                    Pose(2.0, 3.0),
                    Pose(0.0, 5.0),
                ],
                enabled=True,
                values={"a": 1.0, "b": 0.0},
            ),
        )


class TestToSchema(unittest.TestCase):

    def test_any(self):
        self.assertEqual(to_schema(Any), {})

    def test_scalar(self):
        self.assertEqual(
            to_schema(int),
            {"type": "integer"},
        )
        self.assertEqual(
            to_schema(float),
            {"type": "number"},
        )
        self.assertEqual(
            to_schema(str),
            {"type": "string"},
        )
        self.assertEqual(
            to_schema(bool),
            {"type": "boolean"},
        )
        self.assertEqual(
            to_schema(type(None)),
            {"type": "null"},
        )

    def test_list(self):
        self.assertEqual(
            to_schema(List[int]),
            {
                "type": "array",
                "items": {
                    "type": "integer",
                },
            },
        )

    def test_dict(self):
        self.assertEqual(
            to_schema(Dict[str, float]),
            {
                "type": "object",
                "additionalProperties": {
                    "type": "number",
                },
            },
        )

    def test_dataclass(self):
        self.assertEqual(
            to_schema(Pose),
            {
                "description": "Pose(x: float, y: float)",
                "type": "object",
                "properties": {
                    "x": {"type": "number"},
                    "y": {"type": "number"},
                },
            },
        )

    def test_nested_dataclass(self):
        schema = to_schema(Config)

        self.maxDiff = None
        self.assertEqual(
            schema,
            {
                "description": "config",
                "type": "object",
                "properties": {
                    "pose": {
                        "description": "Pose(x: float, y: float)",
                        "type": "object",
                        "properties": {
                            "x": {"type": "number"},
                            "y": {"type": "number"},
                        },
                    },
                    "poses": {
                        "type": "array",
                        "items": {
                            "description": "Pose(x: float, y: float)",
                            "type": "object",
                            "properties": {
                                "x": {"type": "number"},
                                "y": {"type": "number"},
                            },
                        },
                    },
                    "values": {
                        "type": "object",
                        "additionalProperties": {
                            "type": "number",
                        },
                    },
                   "enabled": {"type": "boolean"},
                   "name": {"type": "string"},
                },
            },
        )

    def test_defaults(self):
        schema = to_schema(Defaults)

        self.assertEqual(
            schema,
            {
                "description": "Defaults(name: str = 'default', count: int = 42, tags: List[str] = <factory>)",
                "type": "object",
                "properties": {
                    "name": {
                        "type": "string",
                        "default": "default",
                    },
                    "count": {
                        "type": "integer",
                        "default": 42,
                    },
                    "tags": {
                        "type": "array",
                        "items": {"type": "string"},
                    },
                }
            },
        )


class TestTypeCheck(unittest.TestCase):

    # ------------------------------------------------------------------
    # Any
    # ------------------------------------------------------------------

    def test_any(self):
        self.assertTrue(type_check(Any, None))
        self.assertTrue(type_check(Any, 123))
        self.assertTrue(type_check(Any, "hello"))
        self.assertTrue(type_check(Any, []))
        self.assertTrue(type_check(Any, {}))

    # ------------------------------------------------------------------
    # Scalar
    # ------------------------------------------------------------------

    def test_scalar(self):
        self.assertTrue(type_check(int, 1))
        self.assertTrue(type_check(float, 1.0))
        self.assertTrue(type_check(str, "hello"))
        self.assertTrue(type_check(bool, True))
        self.assertTrue(type_check(type(None), None))

    def test_scalar_type_mismatch(self):
        self.assertFalse(type_check(int, "1"))
        self.assertFalse(type_check(float, "1.0"))
        self.assertFalse(type_check(str, 123))
        self.assertFalse(type_check(bool, 1))
        self.assertFalse(type_check(type(None), 0))

    def test_bool_is_not_int(self):
        # Important because isinstance(True, int) is True in Python.
        self.assertFalse(type_check(int, True))

    def test_int_is_not_float(self):
        self.assertFalse(type_check(float, 1))

    # ------------------------------------------------------------------
    # List
    # ------------------------------------------------------------------

    def test_list(self):
        self.assertTrue(type_check(List[int], []))
        self.assertTrue(type_check(List[int], [1, 2, 3]))

        self.assertTrue(
            type_check(
                List[Pose],
                [
                    Pose(1.0, 2.0),
                    Pose(3.0, 4.0),
                ],
            )
        )

    def test_list_type_mismatch(self):
        self.assertFalse(type_check(List[int], {}))
        self.assertFalse(type_check(List[int], "hello"))
        self.assertFalse(type_check(List[int], None))

    def test_list_element_type_mismatch(self):
        self.assertFalse(
            type_check(
                List[int],
                [1, 2, "3"],
            )
        )

    def test_list_nested_type_mismatch(self):
        self.assertFalse(
            type_check(
                List[List[int]],
                [
                    [1, 2],
                    [3, "4"],
                ],
            )
        )

    def test_list_of_dataclass_type_mismatch(self):
        self.assertFalse(
            type_check(
                List[Pose],
                [
                    Pose(1.0, 2.0),
                    {"x": 3.0, "y": 4.0},
                ],
            )
        )

    # ------------------------------------------------------------------
    # Dict
    # ------------------------------------------------------------------

    def test_dict(self):
        self.assertTrue(type_check(Dict[str, int], {}))
        self.assertTrue(
            type_check(
                Dict[str, int],
                {
                    "a": 1,
                    "b": 2,
                },
            )
        )

    def test_dict_type_mismatch(self):
        self.assertFalse(type_check(Dict[str, int], []))
        self.assertFalse(type_check(Dict[str, int], "hello"))
        self.assertFalse(type_check(Dict[str, int], None))

    def test_dict_value_type_mismatch(self):
        self.assertFalse(
            type_check(
                Dict[str, int],
                {
                    "a": 1,
                    "b": "2",
                },
            )
        )

    def test_dict_nested_type(self):
        self.assertTrue(
            type_check(
                Dict[str, Pose],
                {
                    "a": Pose(1.0, 2.0),
                    "b": Pose(3.0, 4.0),
                },
            )
        )

        self.assertFalse(
            type_check(
                Dict[str, Pose],
                {
                    "a": Pose(1.0, 2.0),
                    "b": {"x": 3.0, "y": 4.0},
                },
            )
        )

    # ------------------------------------------------------------------
    # Dataclass
    # ------------------------------------------------------------------

    def test_dataclass(self):
        self.assertTrue(
            type_check(
                Pose,
                Pose(1.0, 2.0),
            )
        )

    def test_dataclass_type_mismatch(self):
        self.assertFalse(
            type_check(
                Pose,
                {
                    "x": 1.0,
                    "y": 2.0,
                },
            )
        )

        self.assertFalse(
            type_check(
                Pose,
                None,
            )
        )

    def test_dataclass_field_type_mismatch(self):
        self.assertFalse(
            type_check(
                Pose,
                Pose(
                    x="bad", # type: ignore
                    y=2.0,
                ),
            )
        )

    def test_nested_dataclass(self):
        value = Config(
            name="test",
            pose=Pose(1.0, 2.0),
            poses=[
                Pose(3.0, 4.0),
                Pose(5.0, 6.0),
            ],
            values={
                "a": 1.0,
                "b": 2.0,
            },
            enabled=True,
        )

        self.assertTrue(type_check(Config, value))

    def test_nested_dataclass_field_mismatch(self):
        value = Config(
            name="test",
            pose=Pose(1.0, 2.0),
            poses=[
                Pose(3.0, 4.0),
                Pose(5.0, 6.0),
            ],
            values={
                "a": 1.0,
                "b": 2.0,
            },
            enabled=True,
        )

        # Deliberately corrupt the runtime value.
        value.poses[1].x = "bad" # type: ignore

        self.assertFalse(type_check(Config, value))

    # ------------------------------------------------------------------
    # Complex recursive structures
    # ------------------------------------------------------------------

    def test_complex_nested_structure(self):
        typ = Dict[str, List[Dict[str, Pose]]]

        value = {
            "group1": [
                {
                    "a": Pose(1.0, 2.0),
                    "b": Pose(3.0, 4.0),
                },
            ],
            "group2": [
                {
                    "c": Pose(5.0, 6.0),
                },
            ],
        }

        self.assertTrue(type_check(typ, value))

    def test_complex_nested_structure_type_mismatch(self):
        typ = Dict[str, List[Dict[str, Pose]]]

        value = { # type: ignore
            "group1": [
                {
                    "a": Pose(1.0, 2.0),
                    "b": Pose(3.0, 4.0),
                },
            ],
            "group2": [
                {
                    "c": {
                        "x": 5.0,
                        "y": 6.0,
                    },
                },
            ],
        }

        self.assertFalse(type_check(typ, value))


class TestTypeCheckExternal(unittest.TestCase):

    def setUp(self):
        self.schema_file = mock_open(
            read_data=json.dumps(TEST_SCHEMA)
        )

        self.open_patch = patch(
            "builtins.open",
            self.schema_file,
        )
        self.open_patch.start()

        self.addCleanup(self.open_patch.stop)

    def test_valid(self):
        value = WithExternal(
            i=123,
            x=True,
            y={
                "my_integer": 123,
                "my_float": 1.25,
                "my_nan": float("nan"),
                "my_inf": float("inf"),
                "my_path": "/tmp/test.txt",
                "my_sub": {
                    "my_str": "hello",
                    "my_vector": {
                        "x": 1.0,
                        "y": 2.0,
                        "z": 3.0,
                    },
                },
                "my_arr": [
                    {
                        "a": 1.0,
                        "b": 2.0,
                        "c": 3.0,
                    },
                ],
            },
        )

        self.assertTrue(type_check(WithExternal, value, check_external=True))

    def test_integer_type_mismatch(self):
        value = WithExternal(
            i=123,
            x=True,
            y={
                "my_integer": "123",
            },
        )

        self.assertFalse(type_check(WithExternal, value, check_external=True))

    def test_nested_type_mismatch(self):
        value = WithExternal(
            i=123,
            x=True,
            y={
                "my_sub": {
                    "my_vector": {
                        "x": 1.0,
                        "y": "bad",
                        "z": 3.0,
                    },
                },
            },
        )

        self.assertFalse(type_check(WithExternal, value, check_external=True))

    def test_array_type_mismatch(self):
        value = WithExternal(
            i=123,
            x=True,
            y={
                "my_arr": [
                    {
                        "a": 1.0,
                        "b": "bad",
                        "c": 3.0,
                    },
                ],
            },
        )

        self.assertFalse(type_check(WithExternal, value, check_external=True))


if __name__ == "__main__":
    unittest.main()