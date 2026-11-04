# teecon

Run a console app interactively, and simultaneously save a full transcript
of everything it prints to a file. The T is the shape: one stream in, two
copies out — screen and file.

A `>` redirect saves output by taking the console away, so color, prompt
editing, and fullscreen stop working. Pipe-based capture makes the same
trade: interactive or recorded, never both. `teecon` uses a ConPTY
instead, so the child keeps a real console of its own while `teecon`
relays a copy. You use it exactly as if launched from Explorer, and the
file grows live underneath.

The console always shows the raw stream untouched. The log is flat text by
default, or the raw VT stream with `-r`.

## Examples

Unlike PowerShell's transcript feature, this is not tied to one shell: it
works with any app, cmd.exe included.

```bat
teecon log.txt cmd.exe /k echo hi
teecon log.txt -a -r powershell.exe -NoExit
teecon log.txt -d C:\work -new cmd.exe /k echo hi
teecon run.log -- -weird.exe
```

With no command, the default is `cmd.exe`.

## Syntax

```text
teecon <logfile> [flags] [[--] <command>]
```

* `<logfile>` is required and positional: the first argument. By default
  each run replaces it, so the first log line is the child's first line.
* Everything from the first non-flag token onward is the child command,
  taken verbatim: spaces preserved, quotes optional.
* A lone `--` ends option parsing, for commands whose first token would
  otherwise read as a flag.
* Quoting is forgiving throughout: quoted or unquoted log paths, work
  dirs, and commands all work; one layer of wrapping quotes around the
  whole command is stripped if present.

## Flags

| Flag       | Meaning                                              |
|------------|------------------------------------------------------|
| `-a`       | Append to the log instead of replacing it.           |
| `-r`       | Log the raw VT stream instead of flat text.          |
| `-H`       | Write one header line at the top of the log.         |
| `-d DIR`   | Working directory for the child.                     |
| `-new`     | Relaunch in a brand-new console window, Win+R style. |
| `--`       | End of options; the rest of the line is the command. |
| `-h`, `-?` | Show help.                                           |

Flags take `-` or `/` and are case-insensitive except the `H`/`h` pair:
`-H` is the header flag, `-h` is help. An unrecognized `-x` token simply
starts the command. On `-new` the parent exits 0 as soon as the new
console is spawned.

## Log behavior

* Replace is the default: the file holds this run only. `-a` appends
  for history.
* Flat text is the default: a small VT parser resolves the cursor moves
  `cmd.exe` actually emits into plain lines, so the file reads
  line-by-line with no escape bytes. The filter is line-oriented, not a
  terminal emulator: fullscreen programs smear in the log — use `-r`
  for those.
* `-H` adds one header line, local time and teecon's own pid:
  `=== teecon 1.0 2026-02-03 14:05:06 pid=4128 cmd=cmd.exe ===`.
  Default is no header.
* The log is UTF-8 and opened shared, so a tail or an editor can follow
  it while the run is still going.
* Output is recorded before it is echoed, so the console never shows
  anything unrecorded; the disk flush runs on a 200 ms tick.
* Exit code: the child's exit code passes through. `1` means `teecon`
  itself failed (ConPTY unavailable, start failed); `2` means bad usage.

Ctrl+C goes to the child, not to `teecon`: it becomes a byte on the wire
and the pty host turns it into a real `CTRL_C_EVENT` in the child's
console. To stop `teecon` itself, exit the shell. Console modes and code
pages are saved on entry and restored on exit.

## Build environment

Uses the WinDDK 7600.16385.1 (WDK 7.1) environment, e.g.:

```bat
C:\WinDDK7600.16385.1\bin\setenv.bat C:\WinDDK7600.16385.1 fre x64 WIN7 no_oacr
```

then build from the project folder:

```bat
teecon\make.bat
```

`make.bat` runs that `setenv.bat` line itself and passes arguments
through to the build harness.

* `sources` holds the project config: `TARGETNAME=teecon`,
  `UMTYPE=console`, `UMENTRY=main`, `SOURCES=teecon.cpp`.
* `makefile` is the stock one-line WDK indirection. Do not edit it.
* One translation unit, plain C-in-C++: `teecon.cpp`, ASCII-only so the
  old toolchain stays codepage-safe.

**Target OS: Windows 10 plus.** The WDK 7.1 build environment is simply
the compiler used, not a statement about the OS.

ConPTY needs Windows 10 1809 (build 17763). The entry points are resolved
at runtime, so the binary still loads on older Windows and prints a clear
"unavailable" message instead of failing to start. A `conpty.dll` next to
the `.exe` is preferred over the inbox `kernel32` implementation when
present.

## Why the WDK

Building a console tool with the Windows Driver Kit is unusual, but
it suits a small utility like this one well:

* **Zero extra dependencies.** The WDK toolchain links against the OS's
  own DLLs and nothing else, including the C runtime, which is the stock
  `msvcrt.dll` that ships with Windows. There is no Visual C++
  redistributable to download and nothing to bundle: `teecon.exe` runs on
  a stock Windows machine as-is.
* **Tiny executables.** With the C runtime provided by the OS instead of
  linked in, the whole program is about 18 KB. That is small enough to
  copy anywhere.
* **Free.** The WDK 7.1 download costs nothing and needs no account or
  license. It is a self-contained compiler, linker, headers, and build
  harness, with no IDE required. Despite the name it is supports useland
  apps, from console to full GUI, as long as you speak C/C++ from the VC9 era.

Download: [Windows Driver Kit Version 7.1.0](https://www.microsoft.com/en-us/download/details.aspx?id=11800)
(Microsoft Download Center; it ships as an ~700BM GRMWDK_EN_7600_1.ISO).

## Files

* `teecon.cpp` — the entire program.
* `sources` — WDK build config.
* `makefile` — stock WDK one-liner.
* `make.bat` — project-local WDK build entry point.
* `clean.bat` — removes build artifacts.
