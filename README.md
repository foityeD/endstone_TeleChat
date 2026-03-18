# TeleChat (Endstone Telegram Chat Bridge)

TeleChat is a C++ plugin for Endstone that bridges in-game chat with a Telegram bot.

## Requirements
- Windows
- Visual Studio 2022 with MSVC v143 toolset
- C++20

## Vendor Dependencies
Download the sources from these release pages and place them under `vendor/`:
- Endstone `v0.10.14` (required)
- SimpleIni `v4.22` (required)
- fmt `v11.2.0` (optional; not directly included by this project today)
- expected-lite `v0.8.0` (optional; not directly included by this project today)

Release links:
- Endstone: https://github.com/EndstoneMC/endstone/releases/tag/v0.10.14
- SimpleIni: https://github.com/brofield/simpleini/releases/tag/v4.22
- fmt: https://github.com/fmtlib/fmt/releases/tag/11.2.0
- expected-lite: https://github.com/martinmoene/expected-lite/releases/tag/v0.8.0

## Vendor Layout Expected by the Project
```
vendor/
  endstone/
    include/
      endstone/...
  simpleini/
    simpleini-4.22/
      SimpleIni.h
  fmt/
    include/
      fmt/...
  expected-lite/
    include/
      nonstd/expected.hpp
```

## Build
From a Developer Command Prompt for VS:
```powershell
msbuild .\endstone_tg_chat_bridge.sln /p:Configuration=Release /p:Platform=x64
```

Output:
```
x64\Release\TeleChat.dll
```

## Use
1. Place `TeleChat.dll` into your Endstone plugins folder.
2. Start the server once to generate `config.ini` in the plugin data folder.
3. Edit `config.ini` and set `bot_token` and `chat_id`.
4. Restart the server.
