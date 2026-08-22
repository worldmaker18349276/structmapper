import unittest
from dataclasses import dataclass, field
from typing import Any, Dict, List, Literal, Mapping
from unittest.mock import patch

from structmapper.dataclassmapper import ExternalType, from_dict, to_schema, TypeMismatchWarning, UnknownWarning, MissingWarning


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
    additional: 'ExternalType[Literal["path/to/another.schema.json"]]' = field(default_factory=lambda: None)


@dataclass
class Defaults:
    name: str = "default"
    count: int = 42
    tags: List[str] = field(default_factory=lambda: [])


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
                '$.additional: missing field',
                "$.unknown: unknown field",
            ],
        )

        self.assertTrue(isinstance(result, Config))
        assert isinstance(result, Config)
        self.assertEqual(result.name, "test")
        self.assertEqual(result.pose, Pose(1.0, 0.0))
        self.assertEqual(result.poses[0], Pose(2.0, 3.0))
        self.assertEqual(result.poses[1], Pose(0.0, 5.0))
        self.assertEqual(
            result.values,
            {"a": 1.0, "b": 0.0},
        )
        self.assertTrue(result.enabled)


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
                    "additional": {
                        "$ref": "path/to/another.schema.json",
                    },
                },
            },
        )

    def test_defaults(self):
        schema = to_schema(Defaults)

        self.assertEqual(
            schema["properties"]["name"],
            {
                "type": "string",
                "default": "default",
            },
        )

        self.assertEqual(
            schema["properties"]["count"],
            {
                "type": "integer",
                "default": 42,
            },
        )

        self.assertEqual(
            schema["properties"]["tags"],
            {
                "type": "array",
                "items": {"type": "string"},
            },
        )


if __name__ == "__main__":
    unittest.main()