from pathlib import Path
import os
import re

Import("env")

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
ENV_FILE = PROJECT_DIR / ".env"
OUT_FILE = PROJECT_DIR / "include" / "generated_secrets.h"

REQUIRED_KEYS = ["WIFI_SSID", "WIFI_PASS", "TB_HOST", "TB_PORT", "FW_TITLE", "FW_VERSION"]

DEFAULTS = {
    "TB_PORT": "1883",
    "FW_TITLE": "esp32-mesh-node",
    "FW_VERSION": "1.0.0",
}


def parse_env_file(path: Path) -> dict:
    values = {}
    if not path.exists():
        return values

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue

        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()

        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]

        values[key] = value

    return values


def cpp_string(value: str) -> str:
    value = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{value}"'


def is_int(value: str) -> bool:
    return re.fullmatch(r"\d+", value or "") is not None


values = {}
values.update(DEFAULTS)
values.update(parse_env_file(ENV_FILE))

for key in REQUIRED_KEYS:
    if os.getenv(key):
        values[key] = os.getenv(key)

missing = [key for key in REQUIRED_KEYS if not values.get(key)]
if missing:
    raise RuntimeError("Variaveis ausentes no .env: " + ", ".join(missing))

OUT_FILE.parent.mkdir(parents=True, exist_ok=True)

lines = ["#pragma once", ""]

for key in REQUIRED_KEYS:
    value = str(values[key])
    if key == "TB_PORT":
        if not is_int(value):
            raise RuntimeError(f"TB_PORT precisa ser numero: {value}")
        lines.append(f"#define {key} {value}")
    else:
        lines.append(f"#define {key} {cpp_string(value)}")

lines.append("")

# NODE_<id>_TOKEN / NODE_<id>_NAME no .env -> tabela nodeId -> token/nome do device no TB.
# <id> = Node ID que aparece no boot ("[MESH] Node ID: ..."). Lista TODOS os nos da malha
# aqui, nao so o desse repo, pra qualquer um poder repassar telemetria de qualquer vizinho.
NODE_ENTRY_RE = re.compile(r"^NODE_(\d+)_(TOKEN|NAME)$")
node_fields = {}

for key, value in values.items():
    match = NODE_ENTRY_RE.match(key)
    if not match:
        continue
    node_id, field = match.group(1), match.group(2)
    node_fields.setdefault(node_id, {})[field] = value

node_table = []
for node_id, fields in node_fields.items():
    token = fields.get("TOKEN")
    name = fields.get("NAME")
    if token and name:
        node_table.append((node_id, token, name))

lines.append("struct NodeCredential { uint32_t nodeId; const char *token; const char *name; };")
lines.append("static const NodeCredential NODE_TABLE[] = {")
for node_id, token, name in node_table:
    lines.append(f"  {{ {node_id}UL, {cpp_string(token)}, {cpp_string(name)} }},")
lines.append("};")
lines.append(f"static const int NODE_TABLE_COUNT = {len(node_table)};")
lines.append("")

OUT_FILE.write_text("\n".join(lines), encoding="utf-8")

print(f"[ENV] Gerado: {OUT_FILE}")
print(f"[ENV] Nos na NODE_TABLE: {len(node_table)}")
