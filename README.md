# CheatRunner 🎮

**CheatRunner** is a PS5 web launcher and cheat trainer for **already-jailbroken PS5 consoles**.

It provides a local web dashboard to:

- list installed games and apps;
- launch installed titles from a browser;
- load local cheat files from the PS5 filesystem;
- use `.mc4`, `.shn`, and `.json` trainers;
- auto-apply local PS-Game-Patch XML patches when a game starts;
- optionally download cheat files from configured cheat sources;
- enable/disable supported cheats from your browser;
- inspect logs, diagnostics, and cheat debug information.

CheatRunner is focused on local/offline homebrew usage on an already-jailbroken PS5.

---

## ❤️ Support

CheatRunner is free and open-source. If you find it useful, feel free to support the project.

Ko-fi:

[![Support me on Ko-fi](https://www.ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/maj0r)

[Join our Discord Server](https://discord.gg/E4g6fEqp46)

---

## ⚠️ Disclaimer

CheatRunner is **not a jailbreak**, **not an exploit**, and **not a tool for stock/retail consoles**.

It only works when your PS5 is already jailbroken and already has the required homebrew/payload environment running.

CheatRunner does **not** provide:

- jailbreaks or kernel exploits;
- game backups or game content;
- piracy features;
- PSN bypasses;
- online services.

It is a local homebrew tool for testing trainers on your own already-jailbroken console.

Use it at your own risk.

Runtime cheat memory writes can crash the game, break a session, corrupt your savegame, softlock the title, or behave differently depending on the game version/update.

**Always back up your save files before using any cheats.**

---

## ✨ Features

- PS5 web dashboard.
- Installed games/apps list.
- PS5 / PS4 / Apps filters.
- App title names from `app.db` when SQLite support is enabled.
- Launch installed games/apps from the browser.
- Local cheat loading from `/data/cheatrunner/cheats`.
- `.mc4`, `.shn`, and `.json` cheat/trainer support.
- PS-Game-Patch XML auto-patch support.
- Optional remote cheat downloads from configured cheat sources.
- ON/OFF trainer toggles where supported.
- Runtime restore where possible.
- Crash-suspect detection.
- Logs panel.
- Copy Logs / Copy Cheat Debug / Copy Diagnostic Bundle.
- Shutdown Payload button for testing and cleanup.

---

## 🚀 Running

Send `CheatRunner.elf` to your already-jailbroken PS5 using your preferred payload loader.

Then open the dashboard in your browser:

```text
http://<PS5-IP>:9999
```

Example:

```text
http://192.168.1.100:9999
```

---

## 🧪 Notes

CheatRunner is still experimental.

Some cheats may:

- work correctly;
- fail to apply;
- mismatch the game version;
- crash the game;
- require a specific game update;
- behave differently after relaunching the game.

When reporting bugs, include:

- CheatRunner logs;
- Copy Cheat Debug output;
- Copy Diagnostic Bundle output if available;
- game title ID;
- game version/update;
- cheat file used;
- which cheat was enabled/disabled;
- what happened before the issue.

---

## 📁 Local Cheats

CheatRunner is local-first. Local cheat files remain the primary workflow.

Default path:

```text
/data/cheatrunner/cheats
```

Recommended folders:

```text
/data/cheatrunner/cheats/mc4
/data/cheatrunner/cheats/shn
/data/cheatrunner/cheats/json
```

Example files:

```text
/data/cheatrunner/cheats/mc4/PPSA30803_01.200.000.mc4
/data/cheatrunner/cheats/mc4/PPSA30803.mc4
/data/cheatrunner/cheats/shn/CUSA00000.shn
/data/cheatrunner/cheats/json/CUSA00000.json
```

CheatRunner will try to match cheats by title ID and version when possible.

---

## 📁 Local Patches

CheatRunner can auto-apply PS-Game-Patch style XML patches when a matching game starts.

Default paths:

```text
/data/cheatrunner/patches
/data/elf-arsenal/patches
```

Recommended folders:

```text
/data/cheatrunner/patches/xml
/data/cheatrunner/patches/json
/data/cheatrunner/patches/shn
/data/cheatrunner/patches/mc4
```

CheatRunner matches patches by title ID, honors `AppVer` when present, converts `ImageBase` patch addresses to runtime-relative offsets, and pauses kstuff before memory writes when kstuff runtime control is available.

---

## 🌐 Remote Cheat Sources

CheatRunner can optionally download cheat files from configured cheat repositories.

Remote downloads are user-triggered and are intended only as a convenience layer over the local cheat workflow.

The main local cheat path remains:

```text
/data/cheatrunner/cheats
```

Remote XML game patch downloads are not bundled. Use local patch files under `/data/cheatrunner/patches` or `/data/elf-arsenal/patches`.

---

## 🛠️ Building

CheatRunner is built with `ps5-payload-sdk`.

Recommended project layout:

```text
CheatRunner/
├── CheatRunnerPayload/
│   ├── src/
│   ├── CMakeLists.txt
│   └── build-cheatrunner.ps1
└── PS5-Payload-dev/
    └── sdk-master/
```

The build script searches for the SDK in this order:

```text
1. PS5_PAYLOAD_SDK environment variable
2. ../PS5-Payload-dev/sdk-master
3. ../ps5-payload-sdk
```

Build on Windows:

```powershell
cd CheatRunnerPayload
.\build-cheatrunner.ps1 -Clean -Sqlite
```

SQLite note:

- If `src/third_party/sqlite3.c` and `src/third_party/sqlite3.h` exist, bundled SQLite can be enabled.
- Use `-Sqlite` to force-enable SQLite support.
- Use `-NoSqlite` to disable SQLite explicitly.

Output:

```text
build/CheatRunner.elf
```

---

## 🧭 Roadmap

The following features were removed from the v0.1 stabilization pass and may return in a future release:

- controller hotkeys / ScePad monitor.

They were removed to keep the first stable CheatRunner release focused on games, launching, local/remote cheats, and trainer stability.

---

## 🤝 Credits

CheatRunner exists thanks to the PS4/PS5 homebrew and research community.

Special thanks to:

- **ELF Arsenal & VoidShell** for project ideas and implementations;
- **ps5-payload-sdk** developers and contributors;
- **TeeKay87** for the HEN-Cheats-Collection project;
- **etaHEN** for the PS5_Cheats project;
- **GoldHEN** for the GoldHEN_Cheat_Repository project;
- everyone testing, reporting logs, and helping improve the project.

---

## 📜 License

CheatRunner is licensed under the **GNU General Public License v3.0**.

See the `LICENSE` file for the full license text.

```text
GNU General Public License v3.0
```
