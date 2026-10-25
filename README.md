# Switch Cloud Backup

The cloud-based backup manager the Switch was missing.

Back up, restore and delete complete Switch game saves through Google Drive.

## Setup

1. Copy the built `.ovl` to `/switch/.overlays/` on your SD card. An overlay loader is required, like Ultrahand.
2. Open **Settings → Global Google Drive** or choose a connection under **Google Drive per user**. Press **Connect**, scan the QR code and enter the displayed code if needed. Sign in with your own Google account and allow access. Keep the screen open until connected.

Configured releases include the application's Google OAuth client. Users can also connect through their own Google Cloud project and OAuth app using the optional `oauth` section in [auth.example.json](auth.example.json). See [Google OAuth setup and disclaimer](DISCLAIMER.md) for both options and the complete setup instructions.

The app stores personal connection tokens automatically in `/switch/.overlays/SwitchCloudBackup/auth.json`.

A user's own connection takes priority, otherwise the global connection is used. **Disconnect** removes the selected local connection.

## Usage

Select a user and game, then:

1. **Backup** uploads the entire save as a new ZIP.
2. **Restore** lists the game's cloud backups. Select one, then choose **Restore**.
3. **Delete** moves the save to the drives trash bin.

Close the game before backup or restore. If the current save hash is absent from Drive, restoring asks whether to upload it first, skip cloud backup or cancel. A failed upload stops the restore. A local recovery ZIP is always kept under `/switch/.overlays/SwitchCloudBackup/recovery/`.

Drive backups are stored under `SwitchSavegames/<Switch user name>/<Game title>/Savegames/`. User and title IDs remain in the metadata to distinguish equally named users and games. Previous ID-named test folders are ignored.

### Issues

If issues persist let me know via github issues and provide me with the log file `/switch/.overlays/SwitchCloudBackup/error.log`.

## Build

Requires CMake and the devkitPro Switch toolchain with libnx, curl, zlib and mbedTLS.

```sh
git submodule update --init --recursive
./build.sh
```

Output: `build-switch/SavegameBackupOverlay.ovl`.

See [DESIGN.md](DESIGN.md) for the architecture. QR encoding uses Project Nayuki's qrcodegen (MIT).
