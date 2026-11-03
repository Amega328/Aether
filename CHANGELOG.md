# Changelog

All notable changes to Aether are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Files and folders copied on a Windows server can now be pasted into file
  managers on Wayland clients (via `text/uri-list` and
  `x-special/gnome-copied-files`), and files copied on a Wayland client can be
  pasted on Windows. Transfers are limited by the clipboard size setting.

### Fixed
- Wayland clients could end up with an empty clipboard when a paste request
  arrived while a clipboard update from the server was being stored. The
  clipboard cache is now exclusive across the portal and event threads, so a
  paste can no longer break an update in progress.

## [v1.2.4] - 2026-11-02

### Changed
- Replaced the standalone update button with a settings dropdown and moved
  update availability feedback into that menu.
- Send users to the latest GitHub release instead of the nonexistent download
  route.

### Fixed
- LAN-discovered servers now connect using their advertised IP address rather
  than the display name, and discovery labels no longer overwrite the saved
  connection address.
- Normalize previously saved discovery labels into their address when possible.

## [v1.2.3] - 2026-10-30

### Added
- Always-visible top-bar update check with checking, up-to-date, failure, and
  update-available feedback. A user-initiated update check offers to open the
  release download page.

### Fixed
- Update-check network errors now release their reply and restore the check
  control instead of leaving the request state stuck.

## [v1.2.2] - 2026-10-30

### Added
- Connection overview pills for peer RTT, peer count, and encryption state, plus
  a copyable connection-details summary for troubleshooting.

## [v1.2.1] - 2026-10-30

### Changed
- Windows mouse movement is dispatched asynchronously to reduce stutter on
  remote screens, while button and screen-transition messages remain ordered.

## [v1.2.0] - 2026-09-22

### Added
- Port Share: forward localhost TCP ports between machines over the existing
  encrypted link. A new "Port Share" page in the main window holds two fields:
  "Share my ports" (server whitelist, `server/sharedPorts`) and "Tunnel server
  ports" (client, `client/forwardPorts`). While connected, a listed port on the
  client's localhost reaches the same port on the server's localhost.
  Client entries support `serverPort:localPort` remapping (e.g. `3001:8080`
  listens on `localhost:8080` and reaches the server's port 3001).
  Loopback-only on both ends; forwards ride the normal TLS connection
  with a reserved `aetherfwd:<port>` client name.
- Nav rail in the main window with Workspace and Port Share pages.

## [v1.1.0] - 2026-09-18

### Added
- File transfer via clipboard: copy files or folders on one machine and paste
  them on another. Files are staged under `%TEMP%\AetherClipboard` on Windows.
  Total payload is limited by `server/clipboardSize` (default 3 MiB; raise it
  for larger transfers) plus a hard cap of 512 MiB. Requires all peers to run
  this version; older peers ignore the file format.
- LAN server auto-discovery in client mode: servers announce themselves over
  UDP broadcast (port 24801) and appear in the "Connect to" dropdown.
- Copy button next to the suggested IP address in the main window.
- Branded Windows installer: custom sidebar artwork, dialog and banner images,
  version display, and a Launch button on the success page.
- MSVC runtime DLLs now ship inside the app folder, so a matching Visual C++
  Redistributable is no longer required to run or install Aether.
- `REINSTALLMODE=amus` in the MSI so upgrades never leave missing Qt or
  OpenSSL DLLs behind when file versions go backwards.
- Theme-aware action icons (share-screen, remote-control, log-detach,
  log-close) for both light and dark themes.
- `aether::gui::secondaryTextColor()` / `applySecondaryText()` helpers that
  pull muted text color from the active Qlementine theme.
- Theme picker in Settings (System / Light / Dark), applied instantly.
- macOS arm64 build job in CI producing a DMG installer.
- Update check now queries the GitHub releases API and offers an
  "Update available" button that opens the download page.
- Setting to disable update checks.

### Changed
- Qt Widgets GUI now uses the Qlementine style on non-KDE platforms, with
  light/dark themes following the system appearance.
- Main window redesigned: branded header with device name and IP, clickable
  mode cards with icons, a hero status panel (Ready / Starting / Sharing /
  Connecting), and a large primary Start/Stop/Connect action with
  transitional states.
- Log dock restyled as a console panel with search, detach, and close quick
  actions, and padding aligned with the main content grid.
- Secondary text color now comes from Qlementine theme tokens instead of
  palette roles, so muted text stays readable in both dark and light mode.
- Reworked website home page around distributed workspace value proposition
- Reworked About page roadmap into 5-phase unified distributed workspace vision
- Rewrote README with quick start section, badges, and online doc links
- Added Google site verification meta tag to home page
- Added release, license, platform, and PR badges to README

### Fixed
- Daemon hanging during installation: an error `MessageBox` ran in Windows
  Session 0 where it could never be dismissed, stalling the service start
  and failing the install. Error boxes are now only shown in interactive
  sessions.
- Missing `Qt6Network.dll` after upgrading: MSI skipped lower-versioned
  files, then removed them during major-upgrade cleanup.
- Version check returning 404: the update URL was not a real GitHub API
  endpoint. Now uses `releases/latest` and parses `tag_name`.
- Clipboard unit test fixtures that still asserted Deskflow-era string lengths.
- `run_tests` post-build step missing `-C` flag under multi-config generators,
  which made all unit tests report "Not Run".
- I18N tests now locate translations under multi-config generators.
- Download page dropdown spacing between platform pills and download button
- Mode card radios and icons that rendered oddly or clipped in the dock.

## [v1.0.0] - 2026-09-09

### Added
- Keyboard and mouse sharing across Windows and Linux
- Clipboard sharing for text, HTML, and BMP images
- TLS encrypted network traffic between server and clients
- Fingerprint-based peer verification
- Synergy and Barrier wire protocol compatibility
- Qt6 GUI with drag-and-drop screen layout editor
- System tray integration with quick start/stop
- Auto-start on login (optional)
- Version checker with update notification (notification only, no auto-update)
- Multi-language UI: English, Italian, Spanish, Japanese, Korean, Russian, Chinese
- Windows installers: MSI, EXE bootstrapper, 7Z portable
- Linux packages: DEB, RPM, tar.gz
- GitHub Actions CI/CD with Telegram notifications
- Configurable firewall rules on Windows
- X11 and Wayland backend auto-selection on Linux
- Official website with download, docs, and about pages

### Known Limitations
- No code signing (Windows SmartScreen warning)
- No auto-updater (only version check notification)
- Linux build lacks Wayland input capture (libei 1.3+ required, Ubuntu 24.04 has 1.2.1)
- No macOS support
- No dark mode in GUI
- No mDNS discovery (manual IP entry required)

[Unreleased]: https://github.com/RimoraStudio/Aether/compare/v1.2.2...HEAD
[v1.2.2]: https://github.com/RimoraStudio/Aether/compare/v1.2.1...v1.2.2
[v1.2.1]: https://github.com/RimoraStudio/Aether/compare/v1.2.0...v1.2.1
[v1.2.0]: https://github.com/RimoraStudio/Aether/compare/v1.1.0...v1.2.0
[v1.1.0]: https://github.com/RimoraStudio/Aether/compare/v1.0.0...v1.1.0
[v1.0.0]: https://github.com/RimoraStudio/Aether/releases/tag/v1.0.0
