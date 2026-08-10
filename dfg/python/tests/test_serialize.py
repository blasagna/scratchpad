"""Blueprint round-tripping through JSON, and what refuses to be written."""

import json
import unittest

import helpers
from dfg.blueprint import GraphBuilder, GraphSpec, NodeSpec, ParamRef
from dfg.errors import (
    SchemaVersionError,
    SerializationError,
    UnknownNodeTypeError,
    UnknownReadinessError,
    ValidationError,
)
from dfg.graph import Graph
from dfg.readiness import AnyInput, CountAtLeast, PredicateRule
from dfg.registry import Registry
from dfg.serialize import (
    SCHEMA_VERSION,
    dumps,
    from_dict,
    loads,
    to_dict,
)


class TestRoundTrip(unittest.TestCase):
    def test_the_readme_example_round_trips(self):
        spec = helpers.readme_example_spec()
        self.assertEqual(loads(dumps(spec)), spec)

    def test_a_second_dump_is_byte_identical(self):
        # Key order is declaration order, never sorted, so a diff shows only what
        # actually changed.
        text = dumps(helpers.readme_example_spec())
        self.assertEqual(dumps(loads(text)), text)

    def test_nested_subgraphs_round_trip(self):
        inner = GraphBuilder("inner")
        inner.add("double", helpers.Double)
        inner.add_input("x", "double.in")
        inner.add_output("y", "double.out")
        middle = GraphBuilder("middle")
        middle.add_subgraph("deep", inner.build())
        middle.add_input("x", "deep.x")
        middle.add_output("y", "deep.y")
        root = GraphBuilder("root")
        root.add_subgraph("mid", middle.build())
        root.add_input("source", "mid.x")
        root.add_output("result", "mid.y")
        spec = root.build()
        self.assertEqual(loads(dumps(spec)), spec)

    def test_param_refs_round_trip(self):
        builder = GraphBuilder("g", params={"n": 4})
        builder.add("emit", helpers.EmitN, params={"n": ParamRef("n")})
        builder.add_input("source", "emit.in")
        builder.add_output("out", "emit.out")
        spec = builder.build()
        restored = loads(dumps(spec))
        self.assertEqual(restored, spec)
        self.assertEqual(restored.nodes[0].params["n"], ParamRef("n"))

    def test_policies_and_capacities_round_trip(self):
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double, on_error="drop", priority=3)
        builder.add("sink", helpers.Passthrough, readiness=AnyInput())
        builder.connect("head.out", "sink.in", capacity=8, on_overflow="drop_oldest")
        builder.add_input("source", "head.in")
        builder.add_output("out", "sink.out")
        spec = builder.build()
        self.assertEqual(loads(dumps(spec)), spec)

    def test_a_readiness_rule_with_arguments_round_trips(self):
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double, readiness=CountAtLeast(512, port="in"))
        builder.add_input("source", "head.in")
        builder.add_output("out", "head.out")
        restored = loads(dumps(builder.build()))
        rule = restored.nodes[0].readiness
        self.assertEqual(rule, CountAtLeast(512, port="in"))

    def test_a_type_tag_round_trips_including_none(self):
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double)
        builder.add_input("source", "head.in", type_tag="number")
        builder.add_output("out", "head.out")
        restored = loads(dumps(builder.build()))
        self.assertEqual(restored.inputs[0].type_tag, "number")
        self.assertIsNone(restored.outputs[0].type_tag)

    def test_container_params_round_trip(self):
        # Note tuples come back as lists: JSON has one sequence type.
        builder = GraphBuilder("g")
        builder.add("emit", helpers.EmitN, params={"n": 2})
        builder.add_input("source", "emit.in")
        spec = builder.build()
        self.assertEqual(loads(dumps(spec)).nodes[0].params, {"n": 2})


class TestSerializedShape(unittest.TestCase):
    def test_the_document_shape(self):
        data = to_dict(helpers.readme_example_spec())
        self.assertEqual(data["schema_version"], SCHEMA_VERSION)
        graph = data["graph"]
        self.assertEqual(graph["name"], "tracker")
        self.assertEqual(graph["params"], {"imu_rate_hz": 200.0})
        self.assertEqual(
            graph["inputs"][0],
            {
                "name": "imu_raw",
                "type_tag": "ImuSample",
                "targets": [{"node": "calib", "port": "raw"}],
            },
        )
        self.assertEqual(
            graph["outputs"],
            [
                {
                    "name": "pose",
                    "type_tag": None,
                    "source": {"node": "overlay", "port": "composited"},
                }
            ],
        )
        self.assertEqual(
            graph["nodes"][0],
            {
                "kind": "node",
                "id": "calib",
                "type": "helpers.Calib",
                "params": {},
                "readiness": {"kind": "all"},
                "on_error": "stop",
                "priority": 0,
            },
        )
        self.assertEqual(graph["nodes"][1]["kind"], "subgraph")
        self.assertEqual(graph["nodes"][1]["graph"]["name"], "fusion")
        self.assertEqual(
            graph["edges"][0],
            {
                "src": {"node": "calib", "port": "corrected"},
                "dst": {"node": "fusion", "port": "imu"},
                "capacity": None,
                "on_overflow": "error",
                "transport": "memory",
            },
        )

    def test_a_subgraph_input_fan_out_is_one_declaration_with_two_targets(self):
        data = to_dict(helpers.readme_example_spec())
        fusion = data["graph"]["nodes"][1]["graph"]
        self.assertEqual(
            fusion["inputs"][0]["targets"],
            [
                {"node": "predict", "port": "imu"},
                {"node": "update", "port": "imu"},
            ],
        )

    def test_output_is_valid_json(self):
        json.loads(dumps(helpers.readme_example_spec()))


class TestRegistryIndependence(unittest.TestCase):
    def test_loading_needs_no_registry_and_instantiating_does(self):
        # Deserialization without a registry produces a description nothing can
        # instantiate. That is the whole reason the registry is a concept.
        text = dumps(helpers.readme_example_spec())
        spec = loads(text)  # no registry in sight
        self.assertEqual(spec.nodes[0].type_name, "helpers.Calib")

        # Instantiating validates first, so every unresolvable type is reported at
        # once rather than one per attempt.
        with self.assertRaises(ValidationError) as caught:
            Graph.instantiate(spec, Registry())
        self.assertEqual({p.code for p in caught.exception.problems}, {"unknown_type"})

    def test_creating_an_unregistered_type_directly_is_its_own_error(self):
        with self.assertRaises(UnknownNodeTypeError):
            Registry().create("t.calib", {})

    def test_an_unknown_type_survives_a_round_trip_intact(self):
        spec = GraphSpec(
            name="g", nodes=(NodeSpec(node_id="a", type_name="never.registered"),)
        )
        self.assertEqual(loads(dumps(spec)).nodes[0].type_name, "never.registered")


class TestRejections(unittest.TestCase):
    def test_a_predicate_rule_cannot_be_written_and_says_which_node(self):
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double, readiness=PredicateRule(lambda q: True))
        builder.add_input("source", "head.in")
        with self.assertRaises(SerializationError) as caught:
            dumps(builder.build())
        self.assertIn("'head'", str(caught.exception))
        self.assertIn("cannot be serialized", str(caught.exception))

    def test_a_non_json_param_names_the_node_and_the_param(self):
        builder = GraphBuilder("g")
        builder.add("head", helpers.Double, params={})
        spec = builder.build()
        spec = GraphSpec(
            name=spec.name,
            nodes=(
                NodeSpec(
                    node_id="head", type_name="t.double", params={"clock": object()}
                ),
            ),
        )
        with self.assertRaises(SerializationError) as caught:
            dumps(spec)
        self.assertIn("'head'", str(caught.exception))
        self.assertIn("'clock'", str(caught.exception))

    def test_a_missing_schema_version_is_rejected(self):
        with self.assertRaises(SchemaVersionError):
            from_dict({"graph": {"name": "g"}})

    def test_a_future_schema_version_is_rejected(self):
        data = to_dict(helpers.readme_example_spec())
        data["schema_version"] = SCHEMA_VERSION + 1
        with self.assertRaises(SchemaVersionError) as caught:
            from_dict(data)
        self.assertIn(str(SCHEMA_VERSION), str(caught.exception))

    def test_a_missing_graph_object_is_rejected(self):
        with self.assertRaises(SerializationError):
            from_dict({"schema_version": SCHEMA_VERSION})

    def test_invalid_json_is_rejected(self):
        with self.assertRaises(SerializationError):
            loads("{not json")

    def test_a_json_array_is_rejected(self):
        with self.assertRaises(SerializationError):
            loads("[]")

    def test_an_unknown_readiness_kind_is_rejected_on_load(self):
        data = to_dict(helpers.readme_example_spec())
        data["graph"]["nodes"][0]["readiness"] = {"kind": "telepathy"}
        with self.assertRaises(UnknownReadinessError) as caught:
            from_dict(data)
        self.assertIn("telepathy", str(caught.exception))

    def test_an_unknown_node_kind_is_rejected(self):
        data = to_dict(helpers.readme_example_spec())
        data["graph"]["nodes"][0]["kind"] = "wormhole"
        with self.assertRaises(SerializationError):
            from_dict(data)

    def test_a_malformed_port_ref_is_rejected(self):
        data = to_dict(helpers.readme_example_spec())
        data["graph"]["edges"][0]["src"] = "calib.corrected"
        with self.assertRaises(SerializationError) as caught:
            from_dict(data)
        self.assertIn("'node' and 'port'", str(caught.exception))

    def test_a_missing_node_id_is_rejected(self):
        data = to_dict(helpers.readme_example_spec())
        del data["graph"]["nodes"][0]["id"]
        with self.assertRaises(SerializationError):
            from_dict(data)


class TestDefaultsOnLoad(unittest.TestCase):
    def test_omitted_optional_keys_take_their_defaults(self):
        spec = from_dict(
            {
                "schema_version": SCHEMA_VERSION,
                "graph": {
                    "name": "minimal",
                    "nodes": [{"id": "a", "type": "t.double"}],
                },
            }
        )
        node = spec.nodes[0]
        self.assertEqual(node.params, {})
        self.assertEqual(node.on_error, "stop")
        self.assertEqual(node.priority, 0)
        self.assertEqual(node.readiness.KIND, "all")
        self.assertEqual(spec.edges, ())
        self.assertEqual(spec.params, {})


if __name__ == "__main__":
    unittest.main()
