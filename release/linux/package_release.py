#!/usr/bin/env python3
"""Packages the Linux platform release artifact: a merged static library
plus every header transitively needed to compile against it, laid out so
a plain -I<include dir> resolves the same #include paths Bazel would.

Usage: package_release.py <workspace_root> <output_base> <bazel_bin> \
           <headers_list_file> <static_lib_path> <staging_dir>
"""
import pathlib
import shutil
import sys


def resolve_source(short_path: str, workspace_root: pathlib.Path,
                    output_base: pathlib.Path, bazel_bin: pathlib.Path):
    """Returns (source_path, dest_relpath) for one CcInfo header short_path."""
    if short_path.startswith("../"):
        rest = short_path[len("../"):]
        repo, _, path_in_repo = rest.partition("/")
        candidates = [
            bazel_bin / "external" / repo / path_in_repo,
            output_base / "external" / repo / path_in_repo,
        ]
        dest = path_in_repo
    else:
        candidates = [
            workspace_root / short_path,
            bazel_bin / short_path,
        ]
        dest = short_path

    # Bazel's virtual-includes staging dirs (proto_library strip_import_prefix
    # etc.) duplicate the same header under _virtual_imports/<target>/<path>
    # and _virtual_includes/<target>/<path>. Re-root the destination at
    # whatever comes after that marker, since that's the logical include
    # path consumers resolve against (the physical staging dir is a Bazel
    # implementation detail, not part of the real #include spelling).
    for marker in ("_virtual_imports/", "_virtual_includes/"):
        if marker in dest:
            after_marker = dest.split(marker, 1)[1]
            # Drop the leading "<target_name>/" component of the staging dir.
            _, _, dest = after_marker.partition("/")
            break

    # Some external repos (boringssl, nlohmann_json) physically lay their
    # headers out under a top-level include/ dir and declare a plain
    # `includes = ["include"]` on their cc_library -- no virtual_includes
    # staging happens for that (only strip_include_prefix triggers it), so
    # the short_path's repo-relative component still literally starts with
    # "include/". Strip that one segment too, or these land doubled (e.g.
    # include/include/openssl/rand.h) instead of at the real #include
    # spelling (openssl/rand.h) consumers use.
    if dest.startswith("include/"):
        dest = dest[len("include/"):]

    for candidate in candidates:
        if candidate.is_file():
            return candidate, dest
    return None, dest


def main():
    workspace_root, output_base, bazel_bin, headers_list, static_lib, staging = (
        pathlib.Path(a) for a in sys.argv[1:7]
    )

    staging.mkdir(parents=True, exist_ok=True)
    include_dir = staging / "include"
    lib_dir = staging / "lib"
    include_dir.mkdir(exist_ok=True)
    lib_dir.mkdir(exist_ok=True)

    shutil.copy2(static_lib, lib_dir / static_lib.name)

    missing = []
    copied = 0
    dest_seen = {}
    for line in headers_list.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        src, dest = resolve_source(line, workspace_root, output_base, bazel_bin)
        if src is None:
            missing.append(line)
            continue
        if dest in dest_seen and dest_seen[dest] != src:
            # Same logical include path claimed by two different physical
            # files (can happen with virtual-includes re-rooting) -- only a
            # real problem if the contents actually differ.
            if src.read_bytes() != dest_seen[dest].read_bytes():
                missing.append(f"{line} (conflicts with {dest_seen[dest]} at {dest})")
            continue
        dest_seen[dest] = src
        dest_path = include_dir / dest
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest_path)
        copied += 1

    print(f"Copied {copied} headers into {include_dir}")
    if missing:
        print(f"ERROR: could not resolve {len(missing)} header(s):", file=sys.stderr)
        for m in missing:
            print(f"  {m}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
