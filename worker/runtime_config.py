"""Read the same scalar deployment config used by the C++ server and client."""
import os
from pathlib import Path


def load_config(role="server"):
    if role not in ("server", "worker"):
        raise ValueError("invalid config role")
    explicit = os.environ.get("GCRP_CONFIG")
    if explicit is not None:
        if not explicit:
            raise ValueError("GCRP_CONFIG is empty")
        path = Path(explicit)
    else:
        path = next((directory / "config" / name
                     for directory in (Path.cwd(),) + tuple(Path.cwd().parents)
                     for name in (("worker.config", "worker.conf", "gcrp.conf") if role == "worker" else ("server.conf", "gcrp.conf"))
                     if (directory / "config" / name).is_file()), None)
        if path is None:
            raise ValueError("missing config; set GCRP_CONFIG")
    values = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError("malformed config line {}".format(number))
        key, value = (part.strip() for part in line.split("=", 1))
        if not key:
            raise ValueError("empty config key on line {}".format(number))
        values[key] = value
    return values
