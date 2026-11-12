#!/usr/bin/env bash
# Prebuilt ARC images already contain these packages; retain a fallback for
# other runners without spending time on apt indexes when nothing is missing.
set -euo pipefail

if (( $# == 0 )); then
  echo "Usage: $0 PACKAGE [PACKAGE ...]" >&2
  exit 2
fi

missing=()
for package in "$@"; do
  if [[ "$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)" != 'install ok installed' ]]; then
    missing+=("$package")
  fi
done

if (( ${#missing[@]} == 0 )); then
  echo 'All dependencies are installed; skipping apt-get update/install.'
  exit 0
fi

printf 'Installing missing dependencies: %s\n' "${missing[*]}"
sudo apt-get update
sudo apt-get install -y "${missing[@]}"
