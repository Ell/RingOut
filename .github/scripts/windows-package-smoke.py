#!/usr/bin/env python3
"""Windows-side helpers for the packaged module-toolchain smoke test."""

from __future__ import annotations

import ctypes
import struct
import sys
from pathlib import Path


def make_dol(destination: Path) -> None:
    # Project-authored two-instruction GameCube DOL: li r3,1; blr.  This is the
    # minimal legal shape covered by DolRecomp's own frontend test and contains
    # no Nintendo or other third-party game material.
    image = bytearray(0x108)
    struct.pack_into(">I", image, 0x00, 0x100)
    struct.pack_into(">I", image, 0x48, 0x80003100)
    struct.pack_into(">I", image, 0x90, 8)
    struct.pack_into(">I", image, 0xE0, 0x80003100)
    struct.pack_into(">II", image, 0x100, 0x38600001, 0x4E800020)
    destination.write_bytes(image)
    if destination.stat().st_size != 0x108:
        raise AssertionError("synthetic DOL has the wrong size")


class DescriptorPrefix(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("cpu_abi_version", ctypes.c_uint32),
        ("cpu_state_size", ctypes.c_uint32),
        ("game_id", ctypes.c_char * 8),
        ("entry_point", ctypes.c_uint32),
    ]


def check_module(module_path: Path) -> None:
    # Loading through Windows, rather than only inspecting the PE table from
    # Linux, also catches unresolved imports in the generated DLL.
    module = ctypes.CDLL(str(module_path))
    get_module = module.staticrecomp_get_module
    get_module.restype = ctypes.POINTER(DescriptorPrefix)
    getattr(module, "ppc_set_gather_pipe")

    descriptor = get_module().contents
    game_id = descriptor.game_id.rstrip(b"\0")
    assert descriptor.abi_version == 3, descriptor.abi_version
    assert descriptor.cpu_abi_version > 0, descriptor.cpu_abi_version
    assert descriptor.cpu_state_size > 0, descriptor.cpu_state_size
    assert game_id == b"TST001", game_id
    assert descriptor.entry_point == 0x80003100, hex(descriptor.entry_point)
    print(
        "module ABI OK: "
        f"game_id={game_id.decode()} abi={descriptor.abi_version} "
        f"cpu_abi={descriptor.cpu_abi_version} "
        f"entry=0x{descriptor.entry_point:08X}"
    )


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] not in {"make-dol", "check-module"}:
        print(
            "usage: windows-package-smoke.py "
            "<make-dol|check-module> <path>",
            file=sys.stderr,
        )
        return 2

    path = Path(sys.argv[2])
    if sys.argv[1] == "make-dol":
        make_dol(path)
    else:
        check_module(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
