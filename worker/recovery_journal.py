"""Durable local evidence; Python 3.6 compatible."""
import json
import os
from pathlib import Path
import tempfile


def sync_directory(path):
    fd = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def write_json(path, value):
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix=".recovery-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, str(path))
        sync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def remove(path):
    if path.exists():
        path.unlink()
        sync_directory(path.parent)
