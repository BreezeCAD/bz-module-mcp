#!/usr/bin/env python3
"""The .bzmod as BreezeCAD unpacks it: one folder "mcp" with module.json,
README, the module library and the bridge — and on Linux/macOS the bridge
stays runnable (BreezeCAD keeps the exec bit from the ZIP; Claude starts it).

    check_package.py <package.bzmod> <library file name> <bridge file name> <system>
"""
import json
import sys
import zipfile

package, lib, bridge, system = sys.argv[1:5]
with zipfile.ZipFile(package) as z:
    names = {i.filename: i for i in z.infolist()}
    need = ["mcp/module.json", "mcp/README.md", f"mcp/{lib}", f"mcp/{bridge}"]
    missing = [n for n in need if n not in names]
    if missing:
        sys.exit(f"нет в пакете: {missing}; есть: {sorted(names)}")
    manifest = json.loads(z.read("mcp/module.json").decode("utf-8"))
    if manifest["id"] != "mcp":
        sys.exit(f"id в module.json: {manifest['id']}")
    if system != "windows" and not (names[f"mcp/{bridge}"].external_attr >> 16) & 0o111:
        sys.exit("мост в пакете без права на запуск")
print(f"OK: {package}")
