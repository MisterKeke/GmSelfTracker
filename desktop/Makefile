SHELL := cmd.exe
.SHELLFLAGS := /D /C
.DEFAULT_GOAL := help
.NOTPARALLEL:

export QT_ROOT_DIR COMPILER_BIN CMAKE_PATH JOBS ZIP_NAME VCPKG_ROOT VCPKG_INSTALLED_DIR

.PHONY: help build test portable

help:
	@powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "tools/make.ps1" -Task help

build:
	@powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "tools/make.ps1" -Task build

test:
	@powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "tools/make.ps1" -Task test

portable:
	@powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "tools/make.ps1" -Task portable
