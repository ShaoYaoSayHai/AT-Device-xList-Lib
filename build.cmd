@echo off
REM ============================================================
REM ADX build 脚本（CMD 包装，转交给 build.ps1）
REM ============================================================
REM 用法：
REM   build              编译静态库（默认 arm-none-eabi 交叉编译）
REM   build clean        清理 build/
REM   build host         用主机 gcc 编译（语法验证）
REM   build rebuild      clean + 编译
REM ============================================================

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
