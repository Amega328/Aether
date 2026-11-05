# SPDX-FileCopyrightText: (C) 2026 Rimora Studio
# SPDX-FileCopyrightText: (C) 2024 - 2026 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-FileCopyrightText: (C) 2012 - 2025 Symless Ltd
# SPDX-FileCopyrightText: (C) 2009 - 2012 Nick Bolton
# SPDX-License-Identifier: MIT

set(lib_name app)

# arch
if(WIN32)
  set(PLATFORM_CODE
    win32/AppUtilWindows.cpp
    win32/AppUtilWindows.h
  )
elseif(UNIX)
  set(PLATFORM_CODE
    unix/AppUtilUnix.cpp
    unix/AppUtilUnix.h
    unix/AetherXkbKeyboard.cpp
    unix/AetherXkbKeyboard.h
    unix/ISO639Table.h
    unix/XkbLayoutsParser.cpp
    unix/XkbLayoutsParser.h
  )
endif()

add_library(${lib_name} STATIC ${PLATFORM_CODE}
  App.cpp
  App.h
  AppUtil.cpp
  AppUtil.h
  Chunk.cpp
  Chunk.h
  ClientApp.cpp
  ClientApp.h
  ClipboardTypes.h
  Clipboard.cpp
  Clipboard.h
  ClipboardChunk.cpp
  ClipboardChunk.h
  ClipboardFiles.cpp
  ClipboardFiles.h
  AetherException.cpp
  AetherException.h
  DisplayInvalidException.h
  IApp.h
  IClient.h
  IClipboard.cpp
  IClipboard.h
  IKeyState.cpp
  IKeyState.h
  IPlatformScreen.cpp
  IPlatformScreen.h
  IPrimaryScreen.cpp
  IPrimaryScreen.h
  IScreen.h
  IScreenSaver.h
  ISecondaryScreen.h
  KeyTypes.cpp
  KeyTypes.h
  KeyMap.cpp
  KeyMap.h
  KeyState.cpp
  KeyState.h
  MouseTypes.h
  OptionTypes.h
  PacketStreamFilter.cpp
  PacketStreamFilter.h
  PlatformScreen.cpp
  PlatformScreen.h
  ProtocolTypes.cpp
  ProtocolTypes.h
  ProtocolUtil.cpp
  ProtocolUtil.h
  RandomAccessFile.cpp
  RandomAccessFile.h
  Screen.cpp
  Screen.h
  ScreenException.cpp
  ScreenException.h
  ServerApp.cpp
  ServerApp.h
  StreamChunker.cpp
  StreamChunker.h
  StreamPump.cpp
  StreamPump.h
  TransferHub.cpp
  TransferHub.h
  TransferManifest.cpp
  TransferManifest.h
  TransferStream.cpp
  TransferStream.h
  TransferWire.h
  KeyboardLayoutManager.cpp
  KeyboardLayoutManager.h
  ipc/IpcServer.cpp
  ipc/IpcServer.h
  ipc/DaemonIpcServer.cpp
  ipc/DaemonIpcServer.h
  ipc/CoreIpc.cpp
  ipc/CoreIpc.h
  ipc/CoreIpcServer.cpp
  ipc/CoreIpcServer.h
)

target_link_libraries(${lib_name}
  PUBLIC
    common
    Qt6::Network
  PRIVATE
    platform
)

if(UNIX AND NOT APPLE)
  target_link_libraries(${lib_name} PRIVATE server ${LIBXKBREGISTRY_LINK_LIBRARIES})
endif()
