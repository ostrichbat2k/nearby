#!/usr/bin/env bash
# Builds the Linux platform release artifact: a merged static library plus
# every header transitively needed to compile against it. Produces
# release/linux/dist/nearby-linux-platform-<version>.tar.gz containing
# lib/libnearby_linux.a and include/**.
#
# Usage: release/linux/build_release.sh <version>
# Run from the repo root. Requires bazel with --experimental_cc_static_library
# support (the flag the cc_static_library rule needs at this rules_cc version).
set -euo pipefail

VERSION="${1:?usage: build_release.sh <version>}"
TARGET="//release/linux:nearby_linux"
QUERY_TARGETS="//internal/platform/implementation/linux:sharing_context + //sharing:nearby_connections_manager"

# The exact CcInfo provider key is namespaced by rules_cc's bzlmod canonical
# repo name. If this breaks after a rules_cc bump, re-derive it with:
#   bazel cquery --output=starlark --starlark:expr='"\n".join(providers(target).keys())' <any cc_library target>
CC_INFO_KEY='@@rules_cc+//cc/private:cc_info.bzl%CcInfo'

BAZEL_FLAGS=(
  --experimental_cc_static_library
  --@com_google_protobuf//bazel/toolchains:prefer_prebuilt_protoc=true
  --copt='-DGITHUB_BUILD'
)

echo "==> Building ${TARGET}"
bazel build "${BAZEL_FLAGS[@]}" -- "${TARGET}"

WORKSPACE_ROOT="$(pwd)"
OUTPUT_BASE="$(bazel info output_base)"
BAZEL_BIN="$(bazel info bazel-bin)"
STATIC_LIB="${BAZEL_BIN}/release/linux/libnearby_linux.a"

echo "==> Collecting transitive headers"
HEADERS_LIST="$(mktemp)"
trap 'rm -f "${HEADERS_LIST}"' EXIT
bazel cquery "${BAZEL_FLAGS[@]}" \
  --output=starlark \
  --starlark:expr="'\n'.join([f.short_path for f in providers(target)['${CC_INFO_KEY}'].compilation_context.headers.to_list()])" \
  -- ${QUERY_TARGETS} 2>/dev/null | sort -u > "${HEADERS_LIST}"

STAGING="$(mktemp -d)"
trap 'rm -rf "${STAGING}"; rm -f "${HEADERS_LIST}"' EXIT

echo "==> Packaging into ${STAGING}"
python3 release/linux/package_release.py \
  "${WORKSPACE_ROOT}" "${OUTPUT_BASE}" "${BAZEL_BIN}" \
  "${HEADERS_LIST}" "${STATIC_LIB}" "${STAGING}"

DIST_DIR="release/linux/dist"
mkdir -p "${DIST_DIR}"
ARCHIVE="${DIST_DIR}/nearby-linux-platform-${VERSION}.tar.gz"
tar -czf "${ARCHIVE}" -C "${STAGING}" lib include

echo "==> Wrote ${ARCHIVE}"
sha256sum "${ARCHIVE}"
