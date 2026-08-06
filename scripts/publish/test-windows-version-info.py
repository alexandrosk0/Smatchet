#!/usr/bin/env python3
"""Validate the Win32 version resource embedded in a PE file.

Python port of the retired test_windows_version_info.ps1. Python rather than
bash because reading a PE version resource needs the Win32 version.dll API:
PowerShell reached it through `Add-Type` + FileVersionInfo, and bash has no
analogue, but ctypes calls the same three exports directly.

Prints the parsed fields as JSON on success; exits non-zero with a message on
any mismatch or on a file with no version resource.

Usage:
  python scripts/publish/test-windows-version-info.py <path> [--expected-version X.Y.Z]
"""

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import os
import sys

# Fields the installer smoke test asserts on. Every expectation is optional:
# an empty expected value skips that comparison (matching the PowerShell
# Assert-Equal contract).
STRING_FIELDS = (
    ("FileVersion", "FileVersion"),
    ("ProductVersion", "ProductVersion"),
    ("CompanyName", "CompanyName"),
    ("ProductName", "ProductName"),
    ("FileDescription", "FileDescription"),
)


class Translation(ctypes.Structure):
    _fields_ = [("lang", wintypes.WORD), ("codepage", wintypes.WORD)]


def die(message):
    sys.stderr.write("test-windows-version-info: %s\n" % message)
    raise SystemExit(1)


def load_version_dll():
    if os.name != "nt":
        die("Windows-only: this validates a Win32 version resource.")
    version = ctypes.WinDLL("version.dll")
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version.GetFileVersionInfoW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID
    ]
    version.GetFileVersionInfoW.restype = wintypes.BOOL
    version.VerQueryValueW.argtypes = [
        wintypes.LPCVOID, wintypes.LPCWSTR,
        ctypes.POINTER(wintypes.LPVOID), ctypes.POINTER(wintypes.UINT),
    ]
    version.VerQueryValueW.restype = wintypes.BOOL
    version.VerLanguageNameW.argtypes = [wintypes.DWORD, wintypes.LPWSTR, wintypes.DWORD]
    version.VerLanguageNameW.restype = wintypes.DWORD
    return version


def query_string(version, block, lang, codepage, name):
    sub_block = u"\\StringFileInfo\\%04x%04x\\%s" % (lang, codepage, name)
    value = wintypes.LPVOID()
    size = wintypes.UINT()
    if not version.VerQueryValueW(block, sub_block, ctypes.byref(value), ctypes.byref(size)):
        return ""
    if not size.value:
        return ""
    return ctypes.wstring_at(value.value, size.value - 1)


def read_version_info(path):
    version = load_version_dll()

    handle = wintypes.DWORD(0)
    native_size = version.GetFileVersionInfoSizeW(path, ctypes.byref(handle))
    if native_size <= 0:
        die("GetFileVersionInfoSizeW returned %d for %s" % (native_size, path))

    block = ctypes.create_string_buffer(native_size)
    if not version.GetFileVersionInfoW(path, 0, native_size, block):
        die("GetFileVersionInfoW failed for %s" % path)

    value = wintypes.LPVOID()
    size = wintypes.UINT()
    if not version.VerQueryValueW(block, u"\\VarFileInfo\\Translation",
                                  ctypes.byref(value), ctypes.byref(size)) or not size.value:
        die("No translation table in the version resource of %s" % path)
    translation = ctypes.cast(value, ctypes.POINTER(Translation))[0]

    info = {
        "Path": path,
        "NativeVersionInfoSize": int(native_size),
    }
    for key, name in STRING_FIELDS:
        info[key] = query_string(version, block, translation.lang, translation.codepage, name)

    buf = ctypes.create_unicode_buffer(256)
    version.VerLanguageNameW(translation.lang, buf, len(buf))
    info["Language"] = buf.value
    return info


def main(argv):
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("path")
    parser.add_argument("--expected-version", default="")
    parser.add_argument("--expected-company-name", default="Smatchet")
    parser.add_argument("--expected-product-name", default="Smatchet")
    parser.add_argument("--expected-file-description", default="Smatchet Standalone")
    args = parser.parse_args(argv)

    resolved = os.path.abspath(args.path)
    if not os.path.isfile(resolved):
        die("File not found: %s" % resolved)

    info = read_version_info(resolved)

    expectations = (
        ("FileVersion", args.expected_version),
        ("ProductVersion", args.expected_version),
        ("CompanyName", args.expected_company_name),
        ("ProductName", args.expected_product_name),
        ("FileDescription", args.expected_file_description),
    )
    for key, expected in expectations:
        if not expected.strip():
            continue
        if info[key] != expected:
            die("%s mismatch. Expected '%s' but found '%s'." % (key, expected, info[key]))

    print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
