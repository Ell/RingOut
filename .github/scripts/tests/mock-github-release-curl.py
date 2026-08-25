#!/usr/bin/env python3
"""Small stateful curl stand-in for publish-tag-draft-assets regressions."""

import hashlib
import json
import os
import pathlib
import sys
import urllib.parse


def parse_curl_args(argv):
    output = None
    method = "GET"
    data_path = None
    url = None
    options_with_values = {
        "-H",
        "-o",
        "-w",
        "-X",
        "--connect-timeout",
        "--data-binary",
        "--retry",
        "--retry-delay",
        "--retry-max-time",
    }
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument in options_with_values:
            if index + 1 >= len(argv):
                raise SystemExit(f"mock curl: {argument} has no value")
            value = argv[index + 1]
            if argument == "-o":
                output = value
            elif argument == "-X":
                method = value
            elif argument == "--data-binary":
                data_path = value[1:] if value.startswith("@") else None
            index += 2
        elif argument.startswith("-"):
            index += 1
        else:
            url = argument
            index += 1
    if output is None or url is None:
        raise SystemExit("mock curl: output path and URL are required")
    retry_enabled = "--retry" in argv or any(arg.startswith("--retry=") for arg in argv)
    return pathlib.Path(output), method, data_path, url, retry_enabled


def load_state(path):
    if path.exists():
        return json.loads(path.read_text())
    return {"log": [], "list_calls": 0, "post_releases": 0, "uploaded_assets": []}


def release_record(release_id, marker, *, mismatch=False, wrong_target=False):
    body_marker = marker + "-mismatch" if mismatch else marker
    return {
        "id": release_id,
        "tag_name": os.environ["MOCK_TAG"],
        "draft": True,
        "prerelease": True,
        "body": "Experimental release assets.\n\n" + body_marker,
        "target_commitish": (
            "0000000000000000000000000000000000000000"
            if wrong_target
            else os.environ["MOCK_COMMIT"]
        ),
        "upload_url": (
            f"https://uploads.mock.invalid/repos/Ell/RingOut/releases/{release_id}/assets"
            "{?name,label}"
        ),
    }


def validate_create_payload(data_path):
    if not data_path:
        return "release creation omitted its JSON body"
    payload = json.loads(pathlib.Path(data_path).read_text())
    expected = {
        "tag_name": os.environ["MOCK_TAG"],
        "target_commitish": os.environ["MOCK_COMMIT"],
        "draft": True,
        "prerelease": True,
    }
    for key, value in expected.items():
        if payload.get(key) != value:
            return f"release creation has wrong {key}"
    if os.environ["MOCK_MARKER"] not in payload.get("body", ""):
        return "release creation omitted the source marker"
    return None


def main():
    output, method, data_path, url, retry_enabled = parse_curl_args(sys.argv[1:])
    state_path = pathlib.Path(os.environ["MOCK_STATE"])
    state = load_state(state_path)
    parsed = urllib.parse.urlparse(url)
    path = parsed.path
    query = urllib.parse.parse_qs(parsed.query)
    state["log"].append({"method": method, "url": url, "retry": retry_enabled})

    status = 500
    exit_code = 0
    raw_body = None
    body = {"message": f"unhandled mock request: {method} {url}"}
    tag_object = os.environ["MOCK_TAG_OBJECT"]
    commit = os.environ["MOCK_COMMIT"]
    marker = os.environ["MOCK_MARKER"]
    scenario = os.environ["MOCK_SCENARIO"]

    if method == "GET" and "/git/ref/tags/" in path:
        status = 200
        body = {"object": {"type": "tag", "sha": tag_object}}
    elif method == "GET" and path.endswith("/git/tags/" + tag_object):
        status = 200
        body = {"object": {"type": "commit", "sha": commit}}
    elif method == "GET" and "/releases/tags/" in path:
        # This faithfully models the endpoint that hid the real draft release.
        status = 404
        body = {"message": "Not Found"}
    elif method == "GET" and path.endswith("/releases"):
        state["list_calls"] += 1
        page = int(query.get("page", ["1"])[0])
        release = release_record(1234, marker)
        if scenario == "pagination":
            if page == 1:
                body = [{"tag_name": f"v0-unrelated-{number}"} for number in range(100)]
            elif page == 2:
                body = [release]
            else:
                body = []
        elif scenario == "pagination-dedup":
            if page == 1:
                body = [release] + [
                    {"tag_name": f"v0-unrelated-{number}"} for number in range(99)
                ]
            elif page == 2:
                body = [release]
            else:
                body = []
        elif scenario == "join-delayed":
            body = [release] if state["list_calls"] >= 4 else []
        elif scenario == "join-delayed-mismatch":
            body = (
                [release_record(1234, marker, mismatch=True)]
                if state["list_calls"] >= 4
                else []
            )
        elif scenario == "malformed-list":
            body = {"message": "not an array"}
        elif scenario == "truncated-list":
            raw_body = "["
        elif scenario in {
            "create201",
            "race422",
            "create-zero",
            "duplicate201",
            "transport-created",
            "server500-created",
            "fresh-upload",
        }:
            if state["post_releases"] == 0 or scenario == "create-zero":
                body = []
            elif scenario == "duplicate201":
                body = [release, release_record(5678, marker)]
            else:
                body = [release]
        elif scenario == "zero":
            body = []
        elif scenario == "duplicate":
            body = [release, release_record(5678, marker)]
        elif scenario == "source-mismatch":
            body = [release_record(1234, marker, mismatch=True)]
        elif scenario == "wrong-target":
            body = [release_record(1234, marker, wrong_target=True)]
        else:
            body = [release]
        status = 200
    elif method == "POST" and path.endswith("/releases"):
        state["post_releases"] += 1
        error = validate_create_payload(data_path)
        if retry_enabled:
            status = 400
            body = {"message": "non-idempotent release creation enabled curl retries"}
        elif error:
            status = 400
            body = {"message": error}
        elif scenario in {"create201", "duplicate201", "fresh-upload"}:
            status = 201
            body = release_record(1234, marker)
        elif scenario in {"race422", "create-zero"}:
            status = 422
            body = {"message": "Validation Failed"}
        elif scenario == "transport-created":
            status = "000"
            body = {"message": "connection closed after commit"}
            exit_code = 7
        elif scenario == "server500-created":
            status = 500
            body = {"message": "ambiguous server failure after commit"}
    elif method == "POST" and path.endswith("/assets"):
        expected_assets = json.loads(
            pathlib.Path(os.environ["MOCK_ASSETS_JSON"]).read_text()
        )
        name = query.get("name", [""])[0]
        matches = [asset for asset in expected_assets if asset.get("name") == name]
        if len(matches) != 1 or not data_path:
            status = 400
            body = {"message": "unexpected upload fixture"}
        elif name in state["uploaded_assets"]:
            status = 422
            body = {"message": "already_exists"}
        else:
            data = pathlib.Path(data_path).read_bytes()
            expected = matches[0]
            digest = "sha256:" + hashlib.sha256(data).hexdigest()
            if digest != expected["digest"] or len(data) != expected["size"]:
                status = 400
                body = {"message": "uploaded bytes do not match fixture digest"}
            else:
                state["uploaded_assets"].append(name)
                status = 201
                body = expected
    elif method == "GET" and path.endswith("/assets"):
        status = 200
        expected_assets = json.loads(
            pathlib.Path(os.environ["MOCK_ASSETS_JSON"]).read_text()
        )
        if scenario == "fresh-upload":
            body = [
                asset
                for asset in expected_assets
                if asset["name"] in state["uploaded_assets"]
            ]
        else:
            body = expected_assets

    state_path.write_text(json.dumps(state) + "\n")
    output.write_text(raw_body if raw_body is not None else json.dumps(body) + "\n")
    sys.stdout.write(str(status))
    raise SystemExit(exit_code)


if __name__ == "__main__":
    main()
