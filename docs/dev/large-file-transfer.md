<!--
SPDX-FileCopyrightText: (C) 2026 Rimora Studio
SPDX-License-Identifier: MIT
-->

# Aether large file transfer

Copy files or folders of any size on one machine, paste them on the other.
Replaces packing file contents into the clipboard (limited by memory, by
`clipboardSize`, and blocking input while it moved) for anything above 1 MiB.

## Flow

1. A platform clipboard (`MSWindowsClipboardFilesConverter`, `PortalClipboard`)
   asks `aether::clipboard::offerFiles(paths)` for the `Files` payload. Up to
   1 MiB it is the old inline pack. Above that, `TransferHub::offer` registers
   the files and returns a *manifest* (names, sizes, mtimes, a token). Files
   that a finished transfer staged map back to their original manifest, so
   re-reading the clipboard never starts a new transfer.
2. The manifest syncs like any clipboard (a few KB, so `clipboardSize` no
   longer matters). `Screen::setClipboard` intercepts it, calls
   `TransferHub::request`, and leaves the system clipboard alone until the
   transfer finishes; then it sets the clipboard to the staged files
   (`encodeLocalPaths`, an internal payload that is stripped if a peer sends it).
3. Data connections are always dialed by the client (the server cannot reach it):
   - **pull**: server-held files; the client dials when the manifest arrives.
   - **push**: client-held files; `Client::sendClipboard` calls `announceSent`
     after sending the manifest.
   `TransferConnector` does the dial and hello, the server's
   `ClientProxyUnknown` accepts, and both detach the socket into a
   `BulkChannel` (`SocketBulkChannel`, blocking, TLS preserved) for a hub
   worker thread. The event loop is not involved after the hello.

## Wire format

Hello-back client name: `aetherxfer:<pull|push>:<token hex>:<index>:<count>`
(`kTransferNamePrefix`). A pushing client waits for a 1-byte go from the server
(the server's packet filter may have buffered early bytes). Then, sender to
receiver: `AFS2` header (index, count, token, manifest on stream 0 only), the
raw bytes of the stream's units, and the receiver answers one ack byte.

Units: each file is cut into 4 MiB units, unit *i* belongs to stream
*i mod count*; both sides derive this from the manifest, so there is no
per-block framing. The receiver preallocates files and writes each unit at its
offset, then restores mtimes. Stream count: 1 under 16 MiB, 2 under 128 MiB,
else 4 (never more than the number of units).

## Safety

- Entry names go through `isSafeEntryName`; duplicates, directory content,
  absurd counts/sizes and oversized manifests are rejected on decode.
- Free disk space is checked before writing; a failed transfer deletes its files.
- Server accepts only when clipboard sharing is enabled.
- Staging lives in `<cache>/Aether/transfer/<token>-<n>`, newest two kept, all
  wiped at core start.

## Known limits

- No resume: a dropped connection fails the transfer; moving the cursor away
  and back retries it.
- A client relaying a server-held copy of another client's files fails if the
  server has not finished receiving them yet; retry after it completes.
- Names that Windows cannot hold (`: ? * < > | \`) still reject the selection.
- A folder's manifest must still fit `clipboardSize` (about 100 bytes per file
  before compression).
- No GUI progress yet; start, finish and speed are logged at INFO.
- X11 clients are not covered (same as the inline path).

## Tests

`TransferTests` (manifest, layout, streams, multi-stream, failures, opt-in TLS
benchmark via `AETHER_TRANSFER_BENCH_MB`), `TransferHubTests` (pull, push, relay,
retry, eviction, shutdown over in-memory pipes), `BulkChannelTests` (plain and
TLS channels, cancellation, detaching live `TCPSocket`s).
