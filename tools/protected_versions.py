#!/usr/bin/env python3
"""Python 3.6+ integration helper for the external ZIP cleaner.

Hold this context across BOTH checking a candidate and deleting its ZIP.
An unreadable/missing/malformed list raises; the caller must skip deletion.
This helper itself does not delete files.
"""
from contextlib import contextmanager
import fcntl
import os
import re


@contextmanager
def locked_protected_versions(path):
    path = os.path.abspath(os.fspath(path))
    # The lock inode is permanent: never delete or replace path + '.lock'.
    fd = os.open(path + ".lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o664)
    try:
        if os.fstat(fd).st_uid == os.geteuid():
            os.fchmod(fd, 0o664)
        fcntl.flock(fd, fcntl.LOCK_EX)
        with open(path, "r", encoding="ascii") as stream:
            text = stream.read(1024 * 1024 + 1)
        if len(text) > 1024 * 1024:
            raise ValueError("protection list exceeds 1 MiB; skip ZIP cleanup")
        protected = set()
        for number, line in enumerate(text.splitlines(), 1):
            revision = line.strip()
            if not revision:
                continue
            if not re.match(r"^[0-9]{1,32}\Z", revision):
                raise ValueError("invalid protection revision at line {}".format(number))
            protected.add(revision)
        yield protected
    finally:
        os.close(fd)
