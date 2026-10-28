#!/usr/bin/env python3
"""Publish a manifest only after the build producer has verified the revisions.

This does not infer a binary's SVN revision from its filename. The publisher is
the trust boundary; test2_sha256 identifies the exact input tree, not a label.
"""
import argparse
import json
import os
from pathlib import Path
import tempfile
from pjtest_adapter import file_digest, tree_digest, _SAFE_REVISION


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--test2-root", type=Path, required=True)
    parser.add_argument("--test2-revision", required=True)
    args = parser.parse_args()
    if not _SAFE_REVISION.fullmatch(args.revision) or not args.test2_revision:
        parser.error("invalid revision")
    archive = args.archive.resolve(strict=True)
    manifest = Path(str(archive) + ".manifest.json")
    if manifest.exists() or manifest.is_symlink():
        parser.error("manifest already exists; publish a new immutable artifact")
    value = dict(
        spec_version=1,
        revision=args.revision,
        test2_revision=args.test2_revision,
        archive_sha256=file_digest(archive),
        test2_sha256=tree_digest(args.test2_root.resolve(strict=True)),
    )
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", dir=archive.parent, prefix=".manifest-", delete=False
        ) as out:
            temporary = Path(out.name)
            json.dump(value, out, indent=2)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        # Atomic publish without replacing an existing manifest.
        os.link(temporary, manifest)
    finally:
        if temporary is not None:
            temporary.unlink()
    print(manifest)


if __name__ == "__main__":
    main()
