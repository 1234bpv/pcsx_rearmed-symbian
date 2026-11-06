> **This is the core of [PS1Emulator](https://github.com/1234bpv/PS1Emulator),**
> a PlayStation emulator for Nokia phones with Symbian 9.1-9.3 and an ARM11
> processor. It is a fork of
> [notaz/pcsx_rearmed](https://github.com/notaz/pcsx_rearmed), and the Symbian
> changes are on the branch `symbian`. The installation file, the instructions
> and the build steps are in PS1Emulator, which checks this repository out into
> its `pcsx_rearmed/` folder. The licence is GPL-2.0, as upstream (see
> [`COPYING`](COPYING)). Everything below this note is the upstream README and
> describes upstream, including the CI badge.

PCSX-ReARMed - yet another PCSX fork
====================================

![CI (Linux)](https://github.com/notaz/pcsx_rearmed/workflows/CI%20(Linux)/badge.svg)

PCSX ReARMed is yet another PCSX fork based on the PCSX-Reloaded project,
which itself contains code from PCSX, PCSX-df and PCSX-Revolution. This
version was originally ARM architecture oriented (hence the name) with
its MIPS->ARM dynamic recompilation and assembly optimizations, but more
recently it targets other architectures too. A fork of this emulator was
used in [PS Classic](https://en.wikipedia.org/wiki/PlayStation_Classic)
(without any coordination or even notification).

## Features
* ARM/ARM64 dynamic recompiler by Ari64
* [lightrec](https://github.com/pcercuei/lightrec/) dynamic recompiler for other architectures
* NEON GPU by Exophase for ARM NEON and x86 SSE2+
* PCSX4ALL GPU by Una-i/senquack for other architectures
* heavily modified P.E.Op.S. SPU
* BIOS HLE emulation (most games run without proprietary BIOS)
* libretro support

