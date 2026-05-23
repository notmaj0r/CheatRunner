# CheatRunner 🎮

**CheatRunner** is a PS5 web cheat trainer for **already-jailbroken PS5 consoles**.

It gives you a web dashboard to:

- launch installed games and apps;
- load local cheat files from the PS5 filesystem;
- use `.mc4`, `.shn`, and `.json` trainers;
- enable/disable cheats from your browser.

---

## ❤️ Support

CheatRunner is free and open-source. If you find it useful, feel free to support the project.

Ko-fi:

[![Support me on Ko-fi](https://www.ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/maj0r)

Join us on our Discord Server:

```text
https://discord.gg/E4g6fEqp46
```

---

## ⚠️ Disclaimer

CheatRunner is **not a jailbreak**, **not an exploit**, and **not a tool for stock/retail consoles**.

It only works when your PS5 is already jailbroken and already has the needed environment running.

CheatRunner does **not** provide:

- jailbreaks or kernel exploits;
- game backups or game content;
- piracy features;
- PSN bypasses;
- online services.

It is a local homebrew tool for testing trainers on your own already-jailbroken console.

Use it at your own risk. 
By using cheats, it can crash the game, break a session, corrupt your savegame, softlock or behave differently depending on game versions.
**Always backup your save files before using any cheats!**

---

## ✨ Features

- PS5 web dashboard.
- Installed games/apps list.
- PS5 / PS4 / Apps filters.
- Launch games from the browser.
- Local cheat loading from `/data/cheatrunner/cheats`.
- `.mc4`, `.shn`, and `.json` support.
- ON/OFF trainer toggles.
- Runtime restore where possible.
- Crash-suspect detection.
- Logs panel.
- Copy Logs / Copy Cheat Debug / Copy Diagnostic Bundle.
- Restart / Disable Payload button for faster testing.

---

## 🚀 Running

Send `CheatRunner.elf` to your already-jailbroken PS5 using your preferred payload loader.

Then open the dashboard on your browser:

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

- work fine;
- fail to apply;
- mismatch the game version;
- crash the game;
- require a specific game update;
- behave differently after relaunching the game.

When reporting bugs, include:

- CheatRunner logs;
- Copy Cheat Debug output;
- game title ID;
- game version;
- cheat file used;
- what cheat was enabled/disabled before the issue happened.

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

## 🛠️ Building

CheatRunner is built with `ps5-payload-sdk`.

Example layout:

```text
CheatRunner/
├── CheatRunnerPayload/
│   ├── src/
│   ├── CMakeLists.txt
│   └── build-cheatrunner.ps1
└── ps5-payload-sdk/
```

Build on Windows:

```powershell
cd CheatRunnerPayload
.\build-cheatrunner.ps1
```

SQLite note:

- If `src/third_party/sqlite3.c` and `src/third_party/sqlite3.h` exist, bundled SQLite is enabled automatically.
- Use `-NoSqlite` to disable it explicitly.
- Use `-Sqlite` to force-enable and fail fast if amalgamation files are missing.

Output:

```text
build/CheatRunner.elf
```

---

## 🤝 Credits

CheatRunner exists thanks to the PS4/PS5 homebrew and research community.

Special thanks to:

- **ELF Arsenal & VoidShell** for project ideas and implementations;
- **ps5-payload-sdk** developers and contributors;
- **TeeKay87** for his HEN-Cheats-Collection project;
- **etaHEN** for his PS5_Cheats project;
- **GoldHEN** for his GoldHEN_Cheat_Repository project;
- everyone testing, reporting logs, and helping the project.

---

## 📜 License

CheatRunner is licensed under the **GNU General Public License v3.0**.

See the `LICENSE` file for the full license text.

```text
GNU General Public License v3.0
```
