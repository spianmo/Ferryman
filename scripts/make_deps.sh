#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
VCPKG_BIN="${VCPKG:-$VCPKG_ROOT/vcpkg}"
TRIPLET="${VCPKG_TRIPLET:-arm64-osx}"

DOWNLOADS_DIR="${VCPKG_DOWNLOADS:-$ROOT_DIR/.vcpkg-downloads}"
BINARY_CACHE_DIR="${VCPKG_DEFAULT_BINARY_CACHE:-$ROOT_DIR/.vcpkg-binary-cache}"

NLOHMANN_VERSION="3.11.3"
NLOHMANN_ARCHIVE_NAME="nlohmann-json-v${NLOHMANN_VERSION}.tar.gz"
NLOHMANN_SHA512="7df19b621de34f08d5d5c0a25e8225975980841ef2e48536abcf22526ed7fb99f88ad954a2cb823115db59ccc88d1dbe74fe6c281b5644b976b33fb78db9d717"

MESON_VERSION="1.5.2"
MESON_ARCHIVE_NAME="meson-${MESON_VERSION}.tar.gz"
MESON_SHA512="54c6611dd95caaffa216f03d0b96b44c86d5452f54e1282234a4646f8e50f75ea0185a1611a4c078c888154bd4e2d917c4d075de3e7577440a925f72f6152a4f"

FFMPEG_VERSION="7.0.2"
FFMPEG_ARCHIVE_NAME="ffmpeg-ffmpeg-n${FFMPEG_VERSION}.tar.gz"
FFMPEG_SHA512="3ba02e8b979c80bf61d55f414bdac2c756578bb36498ed7486151755c6ccf8bd8ff2b8c7afa3c5d1acd862ce48314886a86a105613c05e36601984c334f8f6bf"

if [[ ! -x "$VCPKG_BIN" ]]; then
  echo "[deps] vcpkg not found at $VCPKG_BIN" >&2
  exit 1
fi

mkdir -p "$DOWNLOADS_DIR" "$BINARY_CACHE_DIR"
export VCPKG_DOWNLOADS="$DOWNLOADS_DIR"
export VCPKG_DEFAULT_BINARY_CACHE="$BINARY_CACHE_DIR"

if [[ -n "${VCPKG_ASSET_SOURCES:-}" ]]; then
  export X_VCPKG_ASSET_SOURCES="$VCPKG_ASSET_SOURCES"
  echo "[deps] using custom asset sources from VCPKG_ASSET_SOURCES"
fi

if [[ "${FERRYMAN_USE_PROXY:-0}" == "1" ]]; then
  if command -v useProxy >/dev/null 2>&1; then
    # shellcheck disable=SC1090
    source "$(command -v useProxy)"
    echo "[deps] proxy enabled via useProxy"
  else
    echo "[deps] FERRYMAN_USE_PROXY=1 but useProxy command not found" >&2
  fi
fi

sha512_file() {
  local target="$1"
  shasum -a 512 "$target" | awk '{print $1}'
}

ensure_nlohmann_archive() {
  local archive_path="$DOWNLOADS_DIR/$NLOHMANN_ARCHIVE_NAME"

  if [[ -s "$archive_path" ]]; then
    local existing_sha
    existing_sha="$(sha512_file "$archive_path")"
    if [[ "$existing_sha" == "$NLOHMANN_SHA512" ]]; then
      echo "[deps] found cached $NLOHMANN_ARCHIVE_NAME"
      return 0
    fi
    echo "[deps] cached $NLOHMANN_ARCHIVE_NAME hash mismatch, re-downloading"
    rm -f "$archive_path"
  fi

  local -a urls=()
  if [[ -n "${NLOHMANN_JSON_URL:-}" ]]; then
    urls+=("$NLOHMANN_JSON_URL")
  fi

  urls+=(
    "https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v${NLOHMANN_VERSION}"
    "https://github.com/nlohmann/json/archive/refs/tags/v${NLOHMANN_VERSION}.tar.gz"
  )

  if [[ -n "${GITHUB_MIRROR_PREFIX:-}" ]]; then
    local -a mirrored=()
    for src in "${urls[@]}"; do
      mirrored+=("${GITHUB_MIRROR_PREFIX}${src}")
    done
    urls=("${mirrored[@]}" "${urls[@]}")
  fi

  local tmp_path="$archive_path.tmp"
  rm -f "$tmp_path"

  for url in "${urls[@]}"; do
    echo "[deps] prefetching $NLOHMANN_ARCHIVE_NAME from $url"
    if curl --fail --location --http1.1 --retry 6 --retry-all-errors --retry-delay 2 --connect-timeout 10 --max-time 60 "$url" -o "$tmp_path"; then
      local fetched_sha
      fetched_sha="$(sha512_file "$tmp_path")"
      if [[ "$fetched_sha" == "$NLOHMANN_SHA512" ]]; then
        mv "$tmp_path" "$archive_path"
        echo "[deps] cached $NLOHMANN_ARCHIVE_NAME in $DOWNLOADS_DIR"
        return 0
      fi
      echo "[deps] downloaded archive hash mismatch from $url"
      rm -f "$tmp_path"
    fi
  done

  echo "[deps] failed to prefetch $NLOHMANN_ARCHIVE_NAME; continuing with vcpkg builtin download" >&2
  return 0
}

ensure_meson_archive() {
  local archive_path="$DOWNLOADS_DIR/$MESON_ARCHIVE_NAME"

  if [[ -s "$archive_path" ]]; then
    local existing_sha
    existing_sha="$(sha512_file "$archive_path")"
    if [[ "$existing_sha" == "$MESON_SHA512" ]]; then
      echo "[deps] found cached $MESON_ARCHIVE_NAME"
      return 0
    fi
    echo "[deps] cached $MESON_ARCHIVE_NAME hash mismatch, re-downloading"
    rm -f "$archive_path"
  fi

  local -a urls=()
  if [[ -n "${MESON_URL:-}" ]]; then
    urls+=("$MESON_URL")
  fi

  urls+=(
    "https://github.com/mesonbuild/meson/archive/${MESON_VERSION}.tar.gz"
    "https://codeload.github.com/mesonbuild/meson/tar.gz/refs/tags/${MESON_VERSION}"
  )

  if [[ -n "${GITHUB_MIRROR_PREFIX:-}" ]]; then
    local -a mirrored=()
    for src in "${urls[@]}"; do
      mirrored+=("${GITHUB_MIRROR_PREFIX}${src}")
    done
    urls=("${mirrored[@]}" "${urls[@]}")
  fi

  local tmp_path="$archive_path.tmp"
  rm -f "$tmp_path"

  for url in "${urls[@]}"; do
    echo "[deps] prefetching $MESON_ARCHIVE_NAME from $url"
    if curl --fail --location --http1.1 --retry 6 --retry-all-errors --retry-delay 2 --connect-timeout 10 --max-time 60 "$url" -o "$tmp_path"; then
      local fetched_sha
      fetched_sha="$(sha512_file "$tmp_path")"
      if [[ "$fetched_sha" == "$MESON_SHA512" ]]; then
        mv "$tmp_path" "$archive_path"
        echo "[deps] cached $MESON_ARCHIVE_NAME in $DOWNLOADS_DIR"
        return 0
      fi
      echo "[deps] downloaded archive hash mismatch from $url"
      rm -f "$tmp_path"
    fi
  done

  echo "[deps] failed to prefetch $MESON_ARCHIVE_NAME; continuing with vcpkg builtin download" >&2
  return 0
}

ensure_ffmpeg_archive() {
  local archive_path="$DOWNLOADS_DIR/$FFMPEG_ARCHIVE_NAME"

  if [[ -s "$archive_path" ]]; then
    local existing_sha
    existing_sha="$(sha512_file "$archive_path")"
    if [[ "$existing_sha" == "$FFMPEG_SHA512" ]]; then
      echo "[deps] found cached $FFMPEG_ARCHIVE_NAME"
      return 0
    fi
    echo "[deps] cached $FFMPEG_ARCHIVE_NAME hash mismatch, re-downloading"
    rm -f "$archive_path"
  fi

  local -a urls=()
  if [[ -n "${FFMPEG_URL:-}" ]]; then
    urls+=("$FFMPEG_URL")
  fi

  urls+=(
    "https://codeload.github.com/ffmpeg/ffmpeg/tar.gz/refs/tags/n${FFMPEG_VERSION}"
    "https://github.com/ffmpeg/ffmpeg/archive/n${FFMPEG_VERSION}.tar.gz"
  )

  if [[ -n "${GITHUB_MIRROR_PREFIX:-}" ]]; then
    local -a mirrored=()
    for src in "${urls[@]}"; do
      mirrored+=("${GITHUB_MIRROR_PREFIX}${src}")
    done
    urls=("${mirrored[@]}" "${urls[@]}")
  fi

  local tmp_path="$archive_path.tmp"
  rm -f "$tmp_path"

  for url in "${urls[@]}"; do
    echo "[deps] prefetching $FFMPEG_ARCHIVE_NAME from $url"
    if curl --fail --location --http1.1 --retry 6 --retry-all-errors --retry-delay 2 --connect-timeout 10 --max-time 60 "$url" -o "$tmp_path"; then
      local fetched_sha
      fetched_sha="$(sha512_file "$tmp_path")"
      if [[ "$fetched_sha" == "$FFMPEG_SHA512" ]]; then
        mv "$tmp_path" "$archive_path"
        echo "[deps] cached $FFMPEG_ARCHIVE_NAME in $DOWNLOADS_DIR"
        return 0
      fi
      echo "[deps] downloaded archive hash mismatch from $url"
      rm -f "$tmp_path"
    fi
  done

  echo "[deps] failed to prefetch $FFMPEG_ARCHIVE_NAME; continuing with vcpkg builtin download" >&2
  return 0
}

ensure_nlohmann_archive
ensure_meson_archive
ensure_ffmpeg_archive

cd "$ROOT_DIR"
"$VCPKG_BIN" install --triplet "$TRIPLET"
