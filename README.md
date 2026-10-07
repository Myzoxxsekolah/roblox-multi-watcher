# MultiRobloxWatcher

A program that closes the `ROBLOX_singletonEvent` when launching Roblox to allow playing on multiple accounts at once.

## How it works

Roblox builds guard against a second instance using a named `Event` kernel object (`...\BaseNamedObjects\ROBLOX_singletonEvent`). This watcher runs in the system tray and:

1. Checks every 2 seconds if a `RobloxPlayerBeta.exe` process is running.
2. Each new Roblox process is tracked. ~6 seconds after it first sees one (to let Roblox finish creating the event), it force-closes that `ROBLOX_singletonEvent` handle inside that Roblox process, the same handle-enumeration technique Sysinternals' `handle.exe -c` uses (`NtQuerySystemInformation` + `DuplicateHandle` with `DUPLICATE_CLOSE_SOURCE`).
3. If it fails for a process, it retries once on the next tick; if it still fails, it shows a tray notification and stops trying for that process.
4. Once a process is handled it is left alone until it exits. Every additional Roblox window gets the same treatment, so you can run as many instances as you like. (No need for admin rights, and 3rd party launchers like Bloxstrap and Fishstrap are supported)

## Features

- System tray icon, no visible window
- Enable/Disable checking from the tray menu
- Optional **Run on startup** toggle in the tray menu (off by default). It adds/removes a shortcut in your Startup folder (`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup`).

## Building

Requirements: Visual Studio (Desktop development with C++ workload).

1. Clone/download this repo.
2. Open `MultiRobloxWatcher.sln` in Visual Studio.
3. Set configuration to **Release / x64**.
4. Build Solution (Ctrl+Shift+B).
5. The built `MultiRobloxWatcher.exe` will be in `x64\Release\`.

## Credit / prior art

Inspired by [MultiRoblox](https://github.com/Dashbloxx/MultiRoblox) by Dashbloxx, which achieves the same goal (multiple simultaneous Roblox instances) via a different technique (pre-claiming the singleton mutex before Roblox launches). This project targets the newer Event-based singleton check with a different implementation, which I find more reliable.

## Disclaimer

This only manipulates a kernel object inside your own local Roblox process, so you almost certainly won't get banned so at your own risk. No guarantees. Also, this is vibecoded. yeah sorry guys, not learning to code for 30kb of code. 

## License

WTFPL (yes), see [LICENSE](LICENSE). Do what you want with it. Don't care.
