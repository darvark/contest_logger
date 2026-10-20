#!/usr/bin/env bash
set -euo pipefail
umask 022

usage() {
    cat <<'EOF'
Usage: bash tools/package-linux.sh [options]

Build a native package on the target Linux distribution (no cross-packaging).
  --format auto|deb|rpm|slackware  Default: detect from /etc/os-release
  --install-deps                  Install build dependencies using APT or DNF
  --no-hamlib                     Build without hardware CAT/CW support
  --skip-tests                    Do not build or run the test suite
  --build-dir PATH               Default: build-package-<format>
  --output-dir PATH              Default: packages/<format>
  --jobs N                       Parallel build jobs (default: 2)
  --maintainer 'Name <email>'     DEB/RPM package contact
  -h, --help                     Show this help

Paths are relative to the current working directory. Only dependency
installation uses root/sudo; building and staging do not require root.
EOF
}

die() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 ||
        die "Missing tool: $1. Install the build dependencies (see --install-deps and docs/BUILD_AND_RELEASE.md)."
}

format=auto
install_deps=0
hamlib=ON
testing=ON
build_dir=
output_dir=
jobs=2
maintainer='Contest Logger contributors'
while (($#)); do
    case "$1" in
        --format|--build-dir|--output-dir|--jobs|--maintainer)
            (($# >= 2)) && [[ -n $2 && $2 != --* ]] ||
                die "Missing value for $1"
            case "$1" in
                --format) format=$2 ;;
                --build-dir) build_dir=$2 ;;
                --output-dir) output_dir=$2 ;;
                --jobs) jobs=$2 ;;
                --maintainer) maintainer=$2 ;;
            esac
            shift 2
            ;;
        --install-deps) install_deps=1; shift ;;
        --no-hamlib) hamlib=OFF; shift ;;
        --skip-tests) testing=OFF; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown option: $1 (see --help)" ;;
    esac
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"
[[ $(uname -s) == Linux ]] || die "Run this script on Linux."
[[ -r /etc/os-release ]] || die "/etc/os-release is required."
. /etc/os-release
case " ${ID:-} ${ID_LIKE:-} " in
    *" debian "*|*" ubuntu "*) native_format=deb ;;
    *" fedora "*) native_format=rpm ;;
    *" slackware "*) native_format=slackware ;;
    *) die "Unsupported distribution: ${ID:-unknown}. Use Debian/Ubuntu, Fedora or Slackware." ;;
esac
[[ $format != auto ]] || format=$native_format
case "$format" in
    deb|rpm|slackware) ;;
    *) die "Unknown package format: $format" ;;
esac
[[ $format == "$native_format" ]] ||
    die "Build $format packages on the matching distribution, not ${ID}."
if ((install_deps)) && [[ $format == slackware ]]; then
    die "Slackware pkgtools do not resolve dependencies. Install the dependencies listed in docs/BUILD_AND_RELEASE.md, then rerun without --install-deps."
fi

if ((install_deps)); then
    root=()
    if ((EUID != 0)); then
        require_command sudo
        root=(sudo)
    fi
    if [[ $format == deb ]]; then
        require_command apt-get
        deps=(build-essential cmake ninja-build dpkg-dev qt6-base-dev
            libqt6sql6-sqlite qt6-qpa-plugins libyaml-cpp-dev)
        [[ $hamlib == OFF ]] || deps+=(libhamlib-dev)
        "${root[@]}" apt-get update
        "${root[@]}" apt-get install -y "${deps[@]}"
    else
        require_command dnf
        deps=(gcc-c++ cmake ninja-build rpm-build qt6-qtbase-devel
            qt6-qtbase-sqlite qt6-qtbase-gui yaml-cpp-devel)
        [[ $hamlib == OFF ]] || deps+=(hamlib-devel)
        "${root[@]}" dnf install -y "${deps[@]}"
    fi
fi

for tool in cmake cpack c++; do
    require_command "$tool"
done
case "$format" in
    deb) require_command dpkg-deb; require_command dpkg-shlibdeps ;;
    rpm) require_command rpmbuild ;;
    slackware)
        for tool in tar xz mktemp; do require_command "$tool"; done
        ;;
esac

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${build_dir:-"$PWD/build-package-$format"}
output_dir=${output_dir:-"$PWD/packages/$format"}
mkdir -p -- "$build_dir" "$output_dir"
build_dir=$(cd -- "$build_dir" && pwd)
output_dir=$(cd -- "$output_dir" && pwd)
cmake -S "$source_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCPACK_PACKAGING_INSTALL_PREFIX=/usr \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING="$testing" \
    -DCONTEST_LOGGER_BUILD_DOC_TOOLS=OFF \
    -DLOGGER_ENABLE_HAMLIB="$hamlib" \
    -DLOGGER_REQUIRE_HAMLIB="$hamlib" \
    "-DCPACK_PACKAGE_CONTACT=$maintainer"
cmake --build "$build_dir" --parallel "$jobs"
if [[ $testing == ON ]]; then
    require_command ctest
    ctest --test-dir "$build_dir" --output-on-failure
fi

if [[ $format == deb || $format == rpm ]]; then
    generator=DEB
    [[ $format != rpm ]] || generator=RPM
    cpack --config "$build_dir/CPackConfig.cmake" -G "$generator" -B "$output_dir"
else
    # Slackware packages are root-relative tar archives, not CPack TGZ bundles.
    stage=$(mktemp -d "$build_dir/slackware-stage.XXXXXXXX")
    trap 'rm -rf -- "$stage"' EXIT
    chmod 755 "$stage"
    DESTDIR="$stage" cmake --install "$build_dir"
    mkdir -p "$stage/install"
    cat > "$stage/install/slack-desc" <<'EOF'
contest-logger: contest-logger (amateur radio contest logger)
contest-logger:
contest-logger: C++20 / Qt 6 contest logging application and sync server.
contest-logger: Includes YAML contest definitions and documentation.
contest-logger: Supports SQLite storage, Cabrillo/ADIF export and DX clusters.
contest-logger:
contest-logger: Requires Qt 6 (including QSQLITE and platform plugins),
contest-logger: yaml-cpp, and the C++ runtime.
contest-logger: Hardware CAT/CW support requires Hamlib unless disabled.
contest-logger:
contest-logger: License: GPL version 3.
EOF
    printf '%s\n' gcc-g++ qt6 yaml-cpp > "$stage/install/slack-required"
    [[ $hamlib == OFF ]] || printf '%s\n' hamlib >> "$stage/install/slack-required"
    version=$(sed -n 's/^set(CPACK_PACKAGE_VERSION "\(.*\)")$/\1/p' "$build_dir/CPackConfig.cmake")
    [[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "Cannot determine package version."
    arch=$(uname -m)
    case "$arch" in i?86) arch=i586 ;; esac
    archive="$output_dir/contest-logger-${version}-${arch}-1.txz"
    tar --sort=name --owner=0 --group=0 --numeric-owner \
        -cJf "$archive" -C "$stage" .
    printf 'Created: %s\n' "$archive"
fi
printf 'Packages saved in: %s\n' "$output_dir"
