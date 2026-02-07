![](images/banner.png)

### 64-DOS is a **text-mode operating environment** inspired by MS-DOS, built on top of a modern 64-bit Linux kernel.

It boots directly into a custom C `/init` shell (no systemd, no Bash),
providing a DOS-like user experience while retaining full modern
hardware support via the Linux kernel.

This project intentionally treats Linux as a **hardware abstraction layer**,
not a user experience.

---

## What this is

- 64-bit Linux kernel
- Custom C `/init` running as PID 1
- Text-only interface
- No GNU userland
- No systemd
- No login manager
- No desktop

## What this is NOT

- Not a Linux distribution
- Not binary-compatible with real DOS (yet)
- Not a shell theme or BusyBox wrapper

---

## The Goal of the Project
I wondered what it would be like if DOS had continued to be developed, evolving with changing hardware whilst retaining
it's infamous CLI aesthetics, offering a modern alternative to the GUI paradigm. FreeDOS tries to do this, at least
to some small extent, but it's hardware support can be sketchy, and it isn't 64-bit. It's mainly concerned with providing
en environment in which to run DOS programs.

Given the inherent incompatibilities in implementing a true Real-Mode DOS on modern CPUs, and wanting to avoid the huge effort
in building a modern DOS from the ground up (as much as I would love such a project to exist), the simplest solution I could think 
of was to utilise the Linux kernel and build on top of it a custom DOS-like command shell.

### Current Features
The system currently boots into the command shell and provides a suite of DOS builtins, written in C:
- DIR
- CLS
- REN
- TYPE
- COPY
- MD/RD
- DEL

ALl currently-implemented commands support the `/?` help switch, as well as wildcards.

### Planned Upgrades
I plan to complete all builtin DOS commands and then begin working on the external ones, such as `FDISK`, `FORMAT`, `EDIT` etc.

## Requirements (host)

- Linux host (tested on Ubuntu)
- VirtualBox
- VT-x / AMD-V enabled in BIOS
- Buildroot
- gcc

---

## Quick start (development)

This project assumes you already have:
- a Buildroot environment
- a VirtualBox VM wired to boot a rootfs image

### 1. Edit the init shell

```bash
cd init
nano init_shell.c
```

### 2. Rebuild and Inject `/init`

```
./build-init.sh
```

This will:
- compile `/init`
- inject it into the root filesystem image
- regenerate the VirtualBox disk
- upgrade the VM

### 3. Boot the VM

You should land directly in:

```
64-DOS init shell
C:\>
```

## Architecture

See [docs/architecture.md](docs/architecture.md) for design goals, boot flow
and rationale.
