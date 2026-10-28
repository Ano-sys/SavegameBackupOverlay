# Structural Design

## Path Structure

```text
CloudStorage/
└── SwitchSavegames/
    └── [USER] - [SWITCH]/
        └── [GAME]/
            └── Savegames/
                ├── [TIMESTAMP].zip
                ├── [TIMESTAMP].zip
                └── ...
```

Each backup is stored as a separate archive. Existing backups are never overwritten automatically.

`[USER]` is the selected Switch user's nickname, `[SWITCH]` is the console nickname
from System Settings, and `[GAME]` is the game title
shown in the overlay. User and title IDs are stored as private Drive folder
properties to keep equally named users and games separate. User folders and ZIPs
also store the source user name and console name. An upload updates folder names
and source metadata when the displayed names change. Previous ID-named test folders
are not searched or migrated.

## Backup Naming

```text
YYYY-MM-DD_HH-MM-SS.zip
```

The filename uses the console's UTC clock. Google Drive file IDs distinguish
separate uploads, including uploads with identical names. The backup list uses
Google's creation timestamp.

## Overlay Structure

```text
Savegame Backup
├── User Selection
│   └── Game List
│       └── Game
│           ├── Restore
│           │   └── Source User - Switch (sorted by name)
│           │       └── Google Drive Backups
│           │           └── Selected Backup
│           │               ├── Restore
│           │               └── Delete (Confirm, Move to Drive Trash)
│           └── Backup
└── Settings
    ├── Update Overlay
    │   ├── Latest
    │   └── Specific Version → Published Releases → Install
    ├── Global Google Drive
    └── Google Drive per User
```

## Backup Metadata

The archive manifest and Drive metadata contain or reference:

```text
Title ID
User ID
User Name
Switch Name
Backup ID
Savegame Hash
Upload Timestamp
Archive Hash
Backup Format Version
```

`SHA-256` is used to identify savegame contents independently of timestamps.

## Upload

```text
Mount Savegame
→ Calculate Hash
→ Create Archive
→ Upload as New Backup
→ Store Metadata
```

Uploads always create a new backup version.

## Restore

```text
Select Backup
→ Hash Current Save and Check All Cloud Backups
→ If Hash Is Missing: Ask to Back Up, Skip Cloud Backup, or Cancel
→ Download Archive
→ Validate Archive
→ Create and Validate Local Recovery Backup
→ Upload Current Save If Chosen (Abort on Failure)
→ Recheck Current Save Hash
→ Restore Savegame with Bounded Writes and Intermediate Commits
→ Verify Restored Save Hash
```

A restore must never overwrite the local save without first creating a recovery backup.
Restore lists the selected title under every user folder in the connected Drive
root. The selected backup's source UID and title ID validate its ZIP manifest;
the destination remains the locally selected user's UID. Current-save hash checks,
optional uploads and local recovery backups use the destination identity. Archives
without console metadata remain supported. Authentication always uses the local
destination user's effective Drive connection.
The local recovery ZIP is retained. A failed replacement or commit attempts a
rollback from this ZIP. An answer to the cloud backup question is valid only for
the hash that was checked when the question was shown.

Restore and recovery commit each deletion, directory creation and file creation.
File data is committed in chunks of at most one quarter of the actual save journal,
rounded down to 4 KiB and capped at 1 MiB. Writable files are flushed and closed
before a commit, then reopened without truncation at the next write offset.
If the journal size cannot be read, the restore stops before modifying the save.
A failed restore unmounts and reopens the save before recovering the original
contents from the verified local ZIP. Intermediate commits can leave a partial
save after a power loss, so the recovery ZIP must be retained for manual recovery.

## Cloud Interface

Initial backend:

```text
Google Drive API
```

Implemented operations:

```text
Authenticate
List Backups
Upload Backup
Download Backup
Move Backup to Trash
Read Metadata
```

## Google Drive Authentication

List, backup, restore, deletion, update and sign-in jobs share a single background
worker. Its 128 KiB stack is allocated on the first job and kept until overlay
shutdown. Jobs are queued without allocating queue nodes; each task owner waits
for its callback to fully return before releasing its state. This avoids repeatedly
allocating large aligned stacks in a fragmented heap after navigating menus.

All HTTP operations use one process-wide `CurlSession` transport. Individual
session objects do not allocate or close network resources. The first request
initializes BSD sockets and libcurl; subsequent operations share the easy handle
and its per-host connection cache. A mutex serializes requests, and each request
resets its options before its buffers and authentication headers leave scope.

A maintenance thread closes the easy handle, global curl state and BSD sockets
after five minutes without a request. It holds the same mutex, so cleanup cannot
interrupt a transfer. The next request initializes a new transport. The overlay
shuts down the transport and joins its maintenance thread after the GUI job
owners have stopped and the shared background worker has been joined.

The maintainer registers one Google OAuth client of type **TVs and Limited Input
devices**. Configured releases embed that app's client ID and client secret at
build time. End users only select **Connect** and approve the device code with
their own Google account. The overlay calls Google's endpoints directly.

Each connection has its own access and refresh tokens, stored locally in
`auth.json`. Tokens are never included in a build. New installations do not need
to create or edit this file. A complete legacy `oauth` override remains supported
to preserve connections made with a different client. Empty overrides use the
bundled app. Release builds reject missing app credentials, while development
builds can remain unconfigured.

## Local State

```text
/switch/.overlays/SwitchCloudBackup/
├── auth.json
├── tmp/
└── recovery/[USER]/[GAME]/before-restore-[unique]/save.zip
```
