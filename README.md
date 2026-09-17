# MultiRobloxWatcher

A program that closes the `ROBLOX_singletonEvent` when launching Roblox, to allow playing on multiple accounts at once.
Works with Windows 11 and 10.
## How it works

Roblox builds guard against a second instance using a named `Event` kernel object (`...\BaseNamedObjects\ROBLOX_singletonEvent`). This watcher runs quietly in the system tray and:

1. Checks every 2 seconds whether `RobloxPlayerBeta.exe` is running.
2. The first time it detects Roblox has launched, it waits ~4 seconds (to let Roblox finish creating the event), then force-closes that Event handle inside Roblox's own process, the same handle-enumeration technique Sysinternals' `handle.exe -c` uses (`NtQuerySystemInformation` + `DuplicateHandle` with `DUPLICATE_CLOSE_SOURCE`).
3. If it fails, it retries once on the next tick; if it still fails, it shows a tray notification and stops trying until Roblox is closed and relaunched.
4. Once handled, it leaves things alone, even if more Roblox windows are opened, until every Roblox process has fully closed, then resets and watches for the next launch.
(No need for admin rights, and 3rd party launchers like Bloxstrap and Fishstrap are supported)

## Features

- System tray icon, no visible window
- Enable/Disable checking from the tray menu
- Negligible CPU usage. The expensive handle scan only runs once per Roblox launch, not on every tick

## Building (The prebuilt binary is also provided)

Requirements: Visual Studio (Desktop development with C++ workload).

1. Clone/download this repo.
2. Open `MultiRobloxWatcher.sln` in Visual Studio.
3. Set configuration to **Release / x64**.
4. Build Solution (Ctrl+Shift+B).
5. The built `MultiRobloxWatcher.exe` will be in `x64\Release\`.

## Credit / prior art

Inspired by [MultiRoblox](https://github.com/Dashbloxx/MultiRoblox) by Dashbloxx, that does the same thing (multiple simultaneous Roblox instances) BUT with a different technique (pre-claiming the singleton mutex before Roblox launches). I instead targeted the Event-based singleton check with a different implementation, which I find more reliable.

## Disclaimer

This is vibecoded. yeah sorry guys, not learning to code for 30kb of code. This only manipulates a kernel object inside your own local Roblox process, on your own machine, and it's extremely low to no banning risk so no worries for your boblox account. I also recommend putting it in your shell:startup folder to make it run on startup so you don't have to execute it every single time ( Win+R, shell:startup, copy paste the file there.)

## License

WTFPL (yes), see [LICENSE](LICENSE). Do what you want with it, don't care. 
