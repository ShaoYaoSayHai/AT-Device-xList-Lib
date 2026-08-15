# ============================================================
# ADX build 脚本（PowerShell 版）
# ============================================================
# 用法：
#   .\build              编译静态库（默认 arm-none-eabi 交叉编译）
#   .\build clean        清理 build/
#   .\build host         用主机 gcc 编译（语法验证）
#   .\build rebuild      clean + 编译
# ============================================================

param(
    [Parameter(Position = 0)]
    [ValidateSet("all", "clean", "rebuild", "host", "")]
    [string]$Action = "all"
)

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

switch ($Action) {
    "clean"   { make clean; break }
    "rebuild" { make clean; make; break }
    "host"    { make CC=gcc AR=ar; break }
    default   { make; break }
}
