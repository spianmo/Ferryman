def PLATFORM_MATRIX = [
  [
    name: "linux",
    label: "linux && x64",
    triplet: "x64-linux",
    packageSuffix: "linux-x64",
    packageExt: "tar.gz",
    isWindows: false,
    isMac: false
  ],
  [
    name: "macos",
    label: "macos && arm64",
    triplet: "arm64-osx",
    packageSuffix: "macos-arm64",
    packageExt: "tar.gz",
    isWindows: false,
    isMac: true
  ],
  [
    name: "windows",
    label: "windows && x64",
    triplet: "x64-windows-static",
    packageSuffix: "windows-x64",
    packageExt: "zip",
    isWindows: true,
    isMac: false
  ]
]

def runUnixBuild(Map cfg) {
  if (!cfg.isMac) {
    sh """#!/usr/bin/env bash
set -euo pipefail
sudo apt-get update
sudo apt-get install -y build-essential pkg-config nasm yasm libx11-dev libxext-dev libxtst-dev libxi-dev
"""
  }
  if (cfg.isMac) {
    sh """#!/usr/bin/env bash
set -euo pipefail
brew install nasm yasm
"""
  }

  sh """#!/usr/bin/env bash
set -euo pipefail
node --version
npm --version
cd frontend
npm ci
npm run build
"""

  if (cfg.isMac) {
    sh """#!/usr/bin/env bash
set -euo pipefail
node -e '
  const fs = require("fs");
  const file = "vcpkg.json";
  const json = JSON.parse(fs.readFileSync(file, "utf8"));
  if (Array.isArray(json.dependencies)) {
    for (const dep of json.dependencies) {
      if (dep && typeof dep === "object" && dep.name === "ffmpeg" && Array.isArray(dep.features)) {
        dep.features = dep.features.filter((feature) => feature !== "x265");
      }
    }
  }
  fs.writeFileSync(file, JSON.stringify(json, null, 2) + "\\n");
'
cat vcpkg.json
"""
  }

  sh """#!/usr/bin/env bash
set -euo pipefail
mkdir -p "\$VCPKG_DOWNLOADS" .vcpkg-binary-cache
if [[ ! -d "\$VCPKG_ROOT/.git" ]]; then
  git clone https://github.com/microsoft/vcpkg.git "\$VCPKG_ROOT"
fi
"\$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
"\$VCPKG_ROOT/vcpkg" install --triplet "${cfg.triplet}"
"""

  sh """#!/usr/bin/env bash
set -euo pipefail
HCONFIG="vcpkg_installed/${cfg.triplet}/include/hv/hconfig.h"
if [[ ! -f "\$HCONFIG" ]]; then
  echo "hconfig.h not found: \$HCONFIG"
  exit 1
fi
if ! grep -Eq '^[[:space:]]*#define[[:space:]]+WITH_OPENSSL[[:space:]]+1([[:space:]]|\$)' "\$HCONFIG"; then
  echo "WITH_OPENSSL is not enabled in \$HCONFIG"
  echo "---- \$HCONFIG ----"
  sed -n '1,200p' "\$HCONFIG"
  exit 1
fi
echo "Verified WITH_OPENSSL=1 in \$HCONFIG"
"""

  sh """#!/usr/bin/env bash
set -euo pipefail
cmake -S . -B "\$BUILD_DIR" \\
  -DCMAKE_BUILD_TYPE=Release \\
  -DFERRYMAN_BUILD_FRONTEND=OFF \\
  -DCMAKE_TOOLCHAIN_FILE="\$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \\
  -DVCPKG_TARGET_TRIPLET="${cfg.triplet}"
cmake --build "\$BUILD_DIR" --parallel
"""

  sh """#!/usr/bin/env bash
set -euo pipefail
BIN_PATH="\$BUILD_DIR/Ferryman"
if [[ ! -f "\$BIN_PATH" ]]; then
  BIN_PATH="\$BUILD_DIR/Release/Ferryman"
fi
if [[ ! -f "\$BIN_PATH" ]]; then
  echo "Ferryman binary not found."
  exit 1
fi
PKG_ROOT="package/ferryman-${cfg.packageSuffix}"
mkdir -p "\$PKG_ROOT"
cp "\$BIN_PATH" "\$PKG_ROOT/"
cp README.md README_CN.md LICENSE "\$PKG_ROOT/"
tar -czf "ferryman-${cfg.packageSuffix}.tar.gz" -C package "ferryman-${cfg.packageSuffix}"
"""
}

def runWindowsBuild(Map cfg) {
  powershell '''
$ErrorActionPreference = "Stop"
choco install nasm -y
'''

  powershell '''
$ErrorActionPreference = "Stop"
node --version
npm --version
Push-Location frontend
npm ci
npm run build
Pop-Location
'''

  powershell '''
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Path "$env:VCPKG_DOWNLOADS" -Force | Out-Null
New-Item -ItemType Directory -Path ".vcpkg-binary-cache" -Force | Out-Null
if (-not (Test-Path "$env:VCPKG_ROOT\\.git")) {
  git clone https://github.com/microsoft/vcpkg.git "$env:VCPKG_ROOT"
}
& "$env:VCPKG_ROOT\\bootstrap-vcpkg.bat" -disableMetrics
& "$env:VCPKG_ROOT\\vcpkg.exe" install --triplet "$env:VCPKG_TRIPLET"
'''

  powershell '''
$ErrorActionPreference = "Stop"
$hconfig = "vcpkg_installed/$env:VCPKG_TRIPLET/include/hv/hconfig.h"
if (-not (Test-Path $hconfig)) {
  throw "hconfig.h not found: $hconfig"
}
$content = Get-Content $hconfig -Raw
if ($content -notmatch "(?m)^\\s*#define\\s+WITH_OPENSSL\\s+1(\\s|$)") {
  Write-Host "WITH_OPENSSL is not enabled in $hconfig"
  Write-Host "---- $hconfig ----"
  Get-Content $hconfig
  throw "libhv OpenSSL backend verification failed"
}
Write-Host "Verified WITH_OPENSSL=1 in $hconfig"
'''

  powershell '''
$ErrorActionPreference = "Stop"
cmake -S . -B "$env:BUILD_DIR" `
  -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DFERRYMAN_BUILD_FRONTEND=OFF `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET="$env:VCPKG_TRIPLET"
cmake --build "$env:BUILD_DIR" --config Release --parallel
'''

  powershell '''
$ErrorActionPreference = "Stop"
$binCandidates = @(
  "$env:BUILD_DIR/Ferryman.exe",
  "$env:BUILD_DIR/Release/Ferryman.exe"
)
$binPath = $binCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $binPath) {
  throw "Ferryman.exe not found."
}
$pkgRoot = "package/ferryman-$env:PACKAGE_SUFFIX"
New-Item -ItemType Directory -Path $pkgRoot -Force | Out-Null
Copy-Item $binPath "$pkgRoot/Ferryman.exe" -Force
Copy-Item README.md, README_CN.md, LICENSE $pkgRoot -Force
$zipPath = "ferryman-$env:PACKAGE_SUFFIX.zip"
if (Test-Path $zipPath) {
  Remove-Item $zipPath -Force
}
Compress-Archive -Path "$pkgRoot/*" -DestinationPath $zipPath
'''
}

pipeline {
  agent none
  options {
    timestamps()
    disableConcurrentBuilds()
  }
  stages {
    stage("Build Matrix") {
      steps {
        script {
          def branches = [:]
          for (cfg in PLATFORM_MATRIX) {
            def localCfg = cfg
            branches["${localCfg.name} / ${localCfg.triplet}"] = {
              node(localCfg.label) {
                ws("${env.WORKSPACE}@${localCfg.name}") {
                  deleteDir()
                  checkout scm
                  def root = pwd()
                  withEnv([
                    "BUILD_DIR=build",
                    "VCPKG_ROOT=${root}/vcpkg",
                    "VCPKG_TRIPLET=${localCfg.triplet}",
                    "PACKAGE_SUFFIX=${localCfg.packageSuffix}",
                    "VCPKG_FEATURE_FLAGS=manifests,binarycaching",
                    "VCPKG_BINARY_SOURCES=clear;files,${root}/.vcpkg-binary-cache,readwrite",
                    "VCPKG_DOWNLOADS=${root}/.vcpkg-downloads",
                    "VCPKG_DEFAULT_BINARY_CACHE=${root}/.vcpkg-binary-cache"
                  ]) {
                    try {
                      if (localCfg.isWindows) {
                        runWindowsBuild(localCfg)
                      } else {
                        runUnixBuild(localCfg)
                      }
                    } catch (err) {
                      if (localCfg.isMac) {
                        sh """#!/usr/bin/env bash
set +e
echo "==== vcpkg x265 logs ===="
ls -la "\$VCPKG_ROOT/buildtrees/x265" || true
for f in \\
  "\$VCPKG_ROOT/buildtrees/x265/config-arm64-osx-dbg-CMakeCache.txt.log" \\
  "\$VCPKG_ROOT/buildtrees/x265/config-arm64-osx-rel-CMakeCache.txt.log" \\
  "\$VCPKG_ROOT/buildtrees/x265/config-arm64-osx-dbg-CMakeConfigureLog.yaml.log" \\
  "\$VCPKG_ROOT/buildtrees/x265/config-arm64-osx-rel-CMakeConfigureLog.yaml.log" \\
  "\$VCPKG_ROOT/buildtrees/x265/config-arm64-osx-out.log"; do
  if [[ -f "\$f" ]]; then
    echo "---- \$f ----"
    cat "\$f"
  fi
done
"""
                      }
                      throw err
                    } finally {
                      archiveArtifacts artifacts: "ferryman-${localCfg.packageSuffix}.${localCfg.packageExt}", fingerprint: true, onlyIfSuccessful: true
                    }
                  }
                }
              }
            }
          }
          branches.failFast = false
          parallel branches
        }
      }
    }
  }
}
