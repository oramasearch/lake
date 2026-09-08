"""Validate captured HTTP payloads with an independent JSON Schema implementation."""

import json
from pathlib import Path
import sys

from jsonschema import Draft202012Validator


def validation_errors(label, schema, body):
    return [
        f"{label} {list(error.absolute_path)}: {error.message}"
        for error in Draft202012Validator(schema).iter_errors(body)
    ]


def validate_mcp(evidence, tools, error_schema):
    errors = []
    successful = set()
    called = set()
    for call in evidence["calls"]:
        name = call["name"]
        called.add(name)
        body = call["result"]["structuredContent"]
        if body["ok"]:
            successful.add(name)
            schema = tools[name]["outputSchema"]
        else:
            schema = error_schema
        errors.extend(validation_errors(name, schema, body))
    assert called == tools.keys(), f"Tools without an HTTP call: {tools.keys() - called}"
    required_outputs = {
        name
        for name, tool in tools.items()
        if tool["outputSchema"].get("properties", {}).get("value", {}).get("required")
    }
    assert required_outputs <= successful, (
        f"Required outputs without success: {required_outputs - successful}"
    )
    print(
        f"MCP HTTP conformance: {len(called)} tools called; "
        f"{len(successful)} successful tool outputs validated; "
        f"{len(required_outputs)} tools with required value fields succeeded"
    )
    return errors


def validate_rest(evidence, error_schema):
    errors = []
    for response in evidence["rest"]:
        name = response["name"]
        if response["status"] == 200:
            operation = next(iter(evidence["openapi"]["paths"]["/v1/" + name].values()))
            schema = operation["responses"]["200"]["content"]["application/json"]["schema"]
        else:
            schema = error_schema
        errors.extend(validation_errors("REST " + name, schema, response["body"]))
    return errors


def check_upload_schema(evidence, tools):
    upload_input = tools["upload_drive_files"]["inputSchema"]
    item_schema = upload_input["properties"]["files"]["items"]
    assert item_schema["type"] == "object"
    assert set(item_schema["required"]) == {"name", "content"}
    assert item_schema["properties"]["content"]["contentEncoding"] == "base64"
    for call in evidence["calls"]:
        if call["name"] != "upload_drive_files":
            continue
        if not call["result"]["isError"]:
            Draft202012Validator(upload_input).validate(call["arguments"])
        elif call["arguments"].get("files") == [1]:
            assert not Draft202012Validator(upload_input).is_valid(call["arguments"])
    upload = next(
        response for response in evidence["rest"]
        if response["name"] == "upload_drive_files"
    )
    print(
        f"upload_drive_files files=[1]: HTTP {upload['status']} "
        f"{upload['body']['error']['code']} path={upload['body']['error']['path']}"
    )


def check_metering_schema(evidence, tools):
    schema = tools["metering_usage"]["outputSchema"]
    metering = next(
        call["result"]["structuredContent"] for call in evidence["calls"]
        if call["name"] == "metering_usage" and not call["result"]["isError"]
    )
    assert not Draft202012Validator(schema).is_valid(metering["value"])
    missing_currency = json.loads(json.dumps(metering))
    del missing_currency["value"]["currency"]
    assert not Draft202012Validator(schema).is_valid(missing_currency)
    print(
        "metering_usage: successful HTTP structuredContent validates; "
        "unwrapped value and missing currency rejected"
    )


def main():
    evidence = json.loads(Path(sys.argv[1]).read_text())
    tools = {tool["name"]: tool for tool in evidence["tools"]}
    error_schema = evidence["openapi"]["components"]["schemas"]["error_envelope"]
    for tool in tools.values():
        Draft202012Validator.check_schema(tool["outputSchema"])
        Draft202012Validator.check_schema(tool["inputSchema"])
        operation = next(iter(evidence["openapi"]["paths"]["/v1/" + tool["name"]].values()))
        rest_schema = operation["responses"]["200"]["content"]["application/json"]["schema"]
        assert tool["outputSchema"] == rest_schema, tool["name"]
    errors = validate_mcp(evidence, tools, error_schema)
    errors.extend(validate_rest(evidence, error_schema))
    print(f"Schema validation errors: {len(errors)}")
    for error in errors:
        print(error)
    assert not errors, "HTTP payload did not conform to advertised schema"
    check_metering_schema(evidence, tools)
    check_upload_schema(evidence, tools)


if __name__ == "__main__":
    main()
