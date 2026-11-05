# SPDX-FileCopyrightText: (C) 2026 Rimora Studio
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs version)
endif()

create_test(
  NAME ServerProxyTests
  DEPENDS client
  LIBS arch base io mt net platform server app ${extra_libs}
  SOURCE ServerProxyTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/client"
)

create_test(
  NAME TransferConnectorTests
  DEPENDS client
  LIBS app arch base io mt net ${extra_libs}
  SOURCE TransferConnectorTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/client"
)
