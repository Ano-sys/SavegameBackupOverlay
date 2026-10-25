# Google OAuth setup and disclaimer

## Shared OAuth app

Configured releases use a Google OAuth app created by the application maintainer. This registration identifies Switch Cloud Backup to Google. Every user still signs in with their own Google account, grants their own permission, and stores backups in their own Drive.

The overlay communicates directly with Google's OAuth endpoints and Drive API. The maintainer does not operate a backup server. The shared Google project remains under the maintainer's control.

## Your own Google project and OAuth app

You can provide the entire Google-side configuration yourself. Create and manage your own Cloud project, enabled Drive API, OAuth consent configuration and OAuth client. The overlay can use that client instead of the one included in the release. You do not need to host an additional server or rebuild the overlay for this option.

### Create your own Google setup

1. Open the [Google Cloud console](https://console.cloud.google.com/) and create or select your own project. Use this same project for all following steps.
2. Under **APIs & Services → Library**, find **Google Drive API** and enable it. Google sign-in alone does not enable Drive access.
3. Open **Google Auth Platform → Branding** and configure the app name, support email and contact information. Choose an **External** audience when using personal Google accounts.
4. While the app is in **Testing**, add the Google accounts you want to connect under **Audience → Test users**.
5. Under **Data Access**, add `https://www.googleapis.com/auth/drive.file`.
6. Under **Clients → Create Client**, choose **TVs and Limited Input devices**. Save its client ID and client secret.
7. If you want accounts outside your test-user list to connect, publish the OAuth app for external users and complete Google's applicable verification requirements.

Google documents [API activation](https://developers.google.com/workspace/guides/enable-apis), [consent-screen configuration](https://developers.google.com/workspace/guides/configure-oauth-consent), and the [device authorization flow](https://developers.google.com/identity/protocols/oauth2/limited-input-device).

### Configure the overlay

For a new installation, copy [auth.example.json](auth.example.json) to `/switch/.overlays/SwitchCloudBackup/auth.json` on the SD card. Fill both fields in its optional `oauth` section:

```json
{
  "oauth": {
    "client_id": "YOUR_CLIENT_ID.apps.googleusercontent.com",
    "client_secret": "YOUR_CLIENT_SECRET"
  },
  "connections": {}
}
```

If `auth.json` already exists, edit its `oauth` section rather than replacing the whole file. When changing OAuth clients, disconnect existing global and per-user connections in Settings before editing the client values. Then use **Connect** again to sign in and obtain tokens for the selected app.

A complete custom pair takes priority over the bundled client. Leaving both values empty, or omitting `oauth`, selects the bundled client. Filling only one field produces an error. This client selection applies to the entire installation. The Google accounts connected globally or per Switch user remain separate connections.

The `drive.file` scope limits access to files created by or authorized for the app. A different Google project does not automatically gain access to backups associated with the previous app. Keep access to the previous OAuth app if you need to restore those backups. See [Drive scope documentation](https://developers.google.com/workspace/drive/api/guides/api-specific-auth).

## Build with your own OAuth app

To embed your own client in a build, supply a local client JSON file:

```sh
GOOGLE_OAUTH_CLIENT_FILE=/absolute/path/to/client.json \
GOOGLE_OAUTH_REQUIRED=ON ./build.sh
```

The build accepts Google's `installed` JSON format, a top-level object with `client_id` and `client_secret`, or the `oauth` section of an overlay configuration. Only these two app fields are embedded. Personal connection tokens are never included in the build.

Alternatively, set both `GOOGLE_OAUTH_CLIENT_ID` and `GOOGLE_OAUTH_CLIENT_SECRET` as build environment variables. `GOOGLE_OAUTH_REQUIRED=ON` rejects a build without client credentials. An unconfigured development build can still use a complete local `oauth` override.

For automated releases, set the repository Actions secrets `GOOGLE_OAUTH_CLIENT_ID` and `GOOGLE_OAUTH_CLIENT_SECRET`. Keep local client files outside the repository, or use the ignored filename `google-oauth.local.json`.

## App credentials and personal tokens

Google's device authorization flow assumes that a distributed application cannot keep its app credentials confidential. Client IDs and device-client secrets embedded in an overlay can be extracted. These credentials alone do not grant access to a user's Drive. Google requires the user's authorization and the resulting personal tokens. See [Google's device-client model](https://developers.google.com/identity/protocols/oauth2/limited-input-device).

Access and refresh tokens stored under `connections` belong to the connected accounts and must remain private. Do not share your populated `auth.json` or include it in a release. The app stores those tokens locally on the SD card.
