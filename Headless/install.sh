#!/bin/sh
set -eu

repository="${MESEN_AGENT_REPOSITORY:-atonamy/mesen-agent}"
install_dir="${MESEN_AGENT_INSTALL_DIR:-$HOME/.local/bin}"
version="${MESEN_AGENT_VERSION:-}"

case "$(uname -s):$(uname -m)" in
	Darwin:arm64) platform="macos-arm64" ;;
	Darwin:x86_64) platform="macos-x64" ;;
	Linux:x86_64) platform="linux-x64" ;;
	Linux:aarch64|Linux:arm64) platform="linux-arm64" ;;
	*)
		echo "mesen-agent: unsupported platform $(uname -s) $(uname -m)" >&2
		exit 2
		;;
esac

if [ -z "$version" ]; then
	latest_url="$(curl -fsSL -o /dev/null -w '%{url_effective}' "https://github.com/$repository/releases/latest")"
	version="${latest_url##*/}"
fi

case "$version" in
	v*) ;;
	*) version="v$version" ;;
esac

asset="mesen-agent-$platform.tar.gz"
release_url="${MESEN_AGENT_RELEASE_URL:-https://github.com/$repository/releases/download/$version}"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/mesen-agent-install.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

echo "Downloading mesen-agent $version for $platform..."
curl -fL "$release_url/$asset" -o "$work_dir/$asset"
curl -fL "$release_url/SHA256SUMS" -o "$work_dir/SHA256SUMS"

expected="$(awk -v file="$asset" '$2 == file || $2 == "*" file { print $1; exit }' "$work_dir/SHA256SUMS")"
if [ -z "$expected" ]; then
	echo "mesen-agent: checksum for $asset is missing" >&2
	exit 3
fi

if command -v sha256sum >/dev/null 2>&1; then
	actual="$(sha256sum "$work_dir/$asset" | awk '{print $1}')"
else
	actual="$(shasum -a 256 "$work_dir/$asset" | awk '{print $1}')"
fi

if [ "$actual" != "$expected" ]; then
	echo "mesen-agent: checksum verification failed for $asset" >&2
	exit 3
fi

mkdir -p "$work_dir/unpacked"
tar -xzf "$work_dir/$asset" -C "$work_dir/unpacked"

if [ ! -x "$work_dir/unpacked/mesen-agent" ]; then
	echo "mesen-agent: release archive does not contain the CLI" >&2
	exit 3
fi

mkdir -p "$install_dir"
install -m 0755 "$work_dir/unpacked/mesen-agent" "$install_dir/mesen-agent"

for runtime in MesenCore.dylib MesenCore.so; do
	if [ -f "$work_dir/unpacked/$runtime" ]; then
		install -m 0644 "$work_dir/unpacked/$runtime" "$install_dir/$runtime"
	fi
done

"$install_dir/mesen-agent" --help >/dev/null
echo "Installed mesen-agent $version to $install_dir/mesen-agent"
