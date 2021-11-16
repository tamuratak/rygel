@echo off

setlocal enableDelayedExpansion
cd %~dp0

set VERSION=0.13.14

curl -O https://registry.npmjs.org/esbuild-windows-64/-/esbuild-windows-64-%VERSION%.tgz
tar zx --strip-components=1 -f esbuild-windows-64-%VERSION%.tgz package/esbuild.exe
move /Y esbuild.exe esbuild_windows_amd64.exe

curl -O https://registry.npmjs.org/esbuild-windows-32/-/esbuild-windows-32-%VERSION%.tgz
tar zx --strip-components=1 -f esbuild-windows-32-%VERSION%.tgz package/esbuild.exe
move /Y esbuild.exe esbuild_windows_x86.exe

curl -O https://registry.npmjs.org/esbuild-darwin-64/-/esbuild-darwin-64-%VERSION%.tgz
tar zx --strip-components=2 -f esbuild-darwin-64-%VERSION%.tgz package/bin/esbuild
move /Y esbuild esbuild_darwin_amd64

curl -O https://registry.npmjs.org/esbuild-linux-64/-/esbuild-linux-64-%VERSION%.tgz
tar zx --strip-components=2 -f esbuild-linux-64-%VERSION%.tgz package/bin/esbuild
move /Y esbuild esbuild_linux_amd64

curl -O https://registry.npmjs.org/esbuild-linux-arm64/-/esbuild-linux-arm64-%VERSION%.tgz
tar zx --strip-components=2 -f esbuild-linux-arm64-%VERSION%.tgz package/bin/esbuild
move /Y esbuild esbuild_linux_arm64

del *.tgz