# Map Batch Updater
Tool to automate updating map content by adding or removing files across multiple BSPs.

# Features
- Supports modifying local maps or remote workshop maps (workshop functionality only supported for Team Fortress 2)
- Downloading latest version of workshop maps and uploading the new version is automated
- Works on repacked (compressed) maps
- Easy to configure by editing `config.ini` in any text editor
- Safety guard rails against unintended behavior (e.g. logging all operations and text prompt to confirm before uploading) 
- Should support any Source 1 BSP (untested on anything other than TF2)

# Usage
1) Download [latest release](https://github.com/ficool2/map_batch_updater/releases/) and extract.
2) Configure the `config.ini` as needed.
3) Run the tool!

# Build
This project requires the following libraries. These are not included and must be retrieved or built yourself.

- [Steamworks SDK](https://partner.steamgames.com/doc/sdk).
- [minizip-ng](https://github.com/zlib-ng/minizip-ng). (Also includes the LZMA library)
- LZMA SDK. This is built as part of `minizip-ng`.

After fetching the libraries, copy `minizip-ng` includes to `include/minizip`, and copy the Steamworks SDK headers to `include/steam`.

Copy `liblzma`, `libminizip` and `steam_api64` .lib/.pdbs to `lib/debug` and `lib/release`.

Open `.sln` in Visual Studio 2022 and build.