#!/usr/bin/env python3
"""Write Wi-Fi credentials and the provider key onto the panel's NVS.

    python3 firmware/panel/tools/provision_nvs.py /dev/cu.usbmodem101 [path/to/.env]

Reads the repo's gitignored .env (default: the repo root's), builds an NVS
image with namespace `r2cfg`, and flashes it over the `nvs` partition. That
REPLACES the partition: anything else in NVS -- PHY calibration, which the
radio regenerates on the next boot -- goes with it.

Secrets never touch a log, a commit or a lasting file: the CSV and the image
live in a private temp directory that is removed on the way out, and the only
thing printed is which keys were written. CLAUDE.md: "Never commit API keys
or Wi-Fi credentials. Use .env.example and NVS."

Needs ESP-IDF's environment (IDF_PATH) for the partition generator and esptool.
"""
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PARTITIONS = HERE.parent / "partitions.csv"
NAMESPACE = "r2cfg"

# .env name -> NVS key (NVS keys are at most 15 characters). Wi-Fi SSID is the
# only one required; a board with Wi-Fi and no key still proves the network.
KEYS = {
    "WIFI_SSID": "wifi_ssid",
    "WIFI_PASSWORD": "wifi_pass",
    "LLM_API_KEY": "llm_key",
    "LLM_MODEL": "llm_model",
}


def read_env(path: Path) -> dict:
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)\s*$", line)
        if not m:
            continue
        v = m.group(2)
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        out[m.group(1)] = v
    return out


def nvs_partition() -> tuple[str, str]:
    for row in csv.reader(l for l in PARTITIONS.read_text().splitlines()
                          if l.strip() and not l.lstrip().startswith("#")):
        cells = [c.strip() for c in row]
        if cells and cells[0] == "nvs":
            return cells[3], cells[4]
    sys.exit("no `nvs` partition in partitions.csv")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = sys.argv[1]
    env_path = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / ".env"
    idf = os.environ.get("IDF_PATH")
    if not idf:
        sys.exit("IDF_PATH is not set: source ESP-IDF's export.sh first")
    env = read_env(env_path)
    values = {nvs: env.get(name, "") for name, nvs in KEYS.items()}
    if not values["wifi_ssid"]:
        sys.exit(f"WIFI_SSID is empty in {env_path}; nothing written")
    for nvs, v in values.items():
        if len(v.encode()) > 4000:
            sys.exit(f"{nvs} is implausibly long; refusing")

    offset, size = nvs_partition()
    gen = Path(idf) / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    tmp = Path(tempfile.mkdtemp(prefix="r2nvs-"))
    os.chmod(tmp, 0o700)
    try:
        csv_path, bin_path = tmp / "nvs.csv", tmp / "nvs.bin"
        with open(csv_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["key", "type", "encoding", "value"])
            w.writerow([NAMESPACE, "namespace", "", ""])
            for nvs, v in values.items():
                if v:
                    w.writerow([nvs, "data", "string", v])
        subprocess.run([sys.executable, str(gen), "generate", str(csv_path),
                        str(bin_path), size],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3",
                        "-p", port, "write_flash", offset, str(bin_path)],
                       check=True, stdout=subprocess.DEVNULL)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    written = [n for n, v in values.items() if v]
    missing = [n for n, v in values.items() if not v]
    print(f"nvs @ {offset}: wrote {', '.join(written)}"
          + (f"; not set: {', '.join(missing)}" if missing else ""))


if __name__ == "__main__":
    main()
