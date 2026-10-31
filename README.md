# PeacockLauncher

A vibe-coded Hitman 3 mod that starts the [Peacock](https://thepeacockproject.org/) server when the game launches and connects the game to it.

Works on Windows and on Linux under Proton. Under Proton, Peacock runs as a native Linux node process.

## What it does

As soon as the mod loads, before the engine has finished initialising, it:

1. Starts Peacock (`chunk0.js`) on the configured port, unless something is already listening there.
2. Applies the OnlineTools settings Peacock needs: HTTP instead of HTTPS, always send the auth header, bypass certificate pinning, and enable optional dynamic resources.
3. Points the game at `localhost:<port>`.
4. Holds the game's first online config request until Peacock is accepting connections, so the game doesn't drop to offline mode while Peacock is still starting.

When the game closes, Peacock is shut down too. On Windows this happens straight away through a job object. Under Proton it happens a few seconds later, once wineserver exits.

## Installation

1. Install [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK).
2. Copy `PeacockLauncher.dll` into the ZHMModSDK `mods` folder (e.g. `C:\Games\HITMAN 3\Retail\mods`).
3. Start the game, press `~`, enable `PeacockLauncher`, then open **PEACOCK LAUNCHER** from the menu and set the Peacock folder.
4. Restart the game.

If you use OnlineTools as well, don't give it a default domain, or it will fight with this mod over which server the game connects to.

## Settings

| Setting                   | Default | Notes                                                                                                                                                                                                                                                    |
| ------------------------- | ------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `path`                    |         | The folder that contains `chunk0.js`. Under Proton, Linux paths such as `/home/me/Peacock` work.                                                                                                                                                         |
| `node_path`               |         | Can be left empty. On Windows the mod then tries `nodedist\node.exe`, then `node.exe` on `PATH`. Under Proton it tries `node` on `PATH`, then the newest nvm install. Under Proton, a path ending in `.exe` runs that Windows node through Wine instead. |
| `port`                    | `8123`  | Peacock is started with `PORT=<port>`.                                                                                                                                                                                                                   |
| `startup_timeout_seconds` | `30`    | How long the game waits for Peacock before its first online request.                                                                                                                                                                                     |
| `auto_launch`             | `true`  |                                                                                                                                                                                                                                                          |
| `auto_connect`            | `true`  |                                                                                                                                                                                                                                                          |
| `update_mode`             | `alert` | `none`, `alert` or `auto`. The mod checks GitHub for a newer Peacock release when the game starts. `alert` shows a popup with an **Update now** button. `auto` installs the update before launching Peacock.                                             |

Updates use the zip that matches your install: the Windows zip if there is a `nodedist` folder, otherwise the Linux zip. The update is extracted to a staging folder before anything is changed. `resources`, `webui` and `nodedist` are replaced, and the other program files are overwritten. `userdata`, `contractSessions`, `contracts`, `plugins`, `images`, `logs` and an existing `options.ini` are never overwritten. The folder keeps its old name, so `Peacock-v8.9.1-linux` stays named that after updating.

Under Proton, Peacock's output goes to `peacock-launcher.log` in the Peacock folder.

## Building on Windows

Open the project in Visual Studio or CLion, as described for [Visual Studio](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-Visual-Studio-for-development) or [CLion](https://github.com/OrfeasZ/ZHMModSDK/wiki/Setting-up-CLion-for-development).

## Building on Linux (cross-compiling to Windows)

This uses the same setup as [H3-Crash-Debug-Helper](https://github.com/piepieonline/H3-Crash-Debug-Helper#building-on-linux-cross-compiling-to-windows): a local ZHMModSDK checkout, its Nix dev shell, and a `CMakeUserPresets.json` with an `x64-Release-Cross` preset.

```sh
cd /path/to/ZHMModSDK
nix develop --command bash -c '
    cd /path/to/PeacockLauncher &&
    cmake --preset x64-Release-Cross . &&
    cmake --build _build/x64-Release-Cross'
```
