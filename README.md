# Switch Cloud Backup

The cloud-based backup manager the Switch was missing.

Back up, restore and delete complete Switch game saves through Google Drive.

This project was largely vibe-coded because I did not have much free time too build it at this point in my life. I am releasing it publicly because I find it useful and convenient.

## Setup

0. Ensure that Ultrahand's memory limit is at least 6MB (Ultrahand -> '+' Button -> System -> Increase Overlay Usable Memory)
1. Copy the built `.ovl` to `/switch/.overlays/` on your SD card. An overlay loader is required, like Ultrahand.
2. Open **Settings → Global Google Drive** or choose a connection under **Google Drive per user**. Press **Connect**, scan the QR code and enter the displayed code if needed. Sign in with your own Google account and allow access. Keep the screen open until connected.

Configured releases include the application's Google OAuth client. Users can also connect through their own Google Cloud project and OAuth app using the optional `oauth` section in [auth.example.json](auth.example.json). See [Google OAuth setup and disclaimer](DISCLAIMER.md) for both options and the complete setup instructions.

The app stores personal connection tokens automatically in `/switch/.overlays/SwitchCloudBackup/auth.json`.

A user's own connection takes priority, otherwise the global connection is used. **Disconnect** removes the selected local connection.

## Usage

The selected local user must already have an account save for the game. On another console, start the game with the destination user and create a save first if none exists.

Select a user and game, then:

1. **Backup** uploads the entire save as a new ZIP.
2. **Restore** lists backup sources as `<User name> - <Switch name>`, including users with different UIDs on other consoles. Select a source and backup, then choose **Restore**. Opening a source uses the already loaded backup list; **Refresh** fetches it again from Drive. The save is restored into the local user selected before the game.
3. Under **Restore**, select a source and backup, then choose **Delete** to move the cloud backup to Google Drive's trash.

Close the game before backup or restore. If the current save hash is absent from Drive, restoring asks whether to upload it first, skip cloud backup or cancel. A failed upload stops the restore. A local recovery ZIP is always kept under `/switch/.overlays/SwitchCloudBackup/recovery/`.

Drive backups are stored under `SwitchSavegames/<User name> - <Switch name>/<Game title>/Savegames/`. The Switch name comes from the console nickname in System Settings. User names and Switch names are included in the ZIP manifest and Drive metadata; user and title IDs continue to distinguish equally named users and games. Existing user folders are updated on the next upload. Older backups without a Switch name remain available; sources without console metadata display `Unknown Switch`. Previous ID-named test folders are ignored.

Both consoles must use a Drive connection with access to the same backup folder. A per-user connection still takes priority over the global connection.

### Overlay updates

Open **Settings → Update Overlay** and select:

1. **Latest** downloads and installs the latest published full release from this GitHub repository.
2. **Specific Version** lists published full releases with an overlay and checksum. Select a version to install it, or use **Older releases** for the next page. Older versions can also be installed.

The updater checks the download size, overlay format and published SHA-256 checksum before replacing the running overlay's file, including renamed `.ovl` files. It keeps the previous overlay beside it as `<filename>.ovl.previous`; a failed replacement attempts to restore it. If installation is interrupted after moving the old file, rename this backup to its original `.ovl` name manually.

After a successful update, exit the overlay to the overlay loader and launch it again to load the new version. The overlay shortcut only hides or shows the running version.

A download can be cancelled before installation starts. No Google Drive connection is required for updates.

Backup, restore, Google sign-in and updates share one network client. It starts with the first network request and reuses connections where possible. After five minutes without a request, the connection cache and socket service are released automatically and recreated on the next request. Closing the overlay also releases them; an active transfer is allowed to finish or cancel before cleanup.

### Issues

If issues persist let me know via github issues and provide me with the log file `/switch/.overlays/SwitchCloudBackup/error.log`.

## Build

Requires CMake and the devkitPro Switch toolchain with libnx, curl, zlib and mbedTLS.

By default, a local build has no bundled OAuth client. Google sign-in then requires a complete local `oauth` configuration in `auth.json`. To embed a client, follow [Build with your own OAuth app](DISCLAIMER.md#build-with-your-own-oauth-app).

```sh
git submodule update --init --recursive
./build.sh
```

Output: `build-switch/SavegameBackupOverlay.ovl`.

See [DESIGN.md](DESIGN.md) for the architecture. QR encoding uses Project Nayuki's qrcodegen (MIT).
