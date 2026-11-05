# SPDX-FileCopyrightText: (C) 2025 Rimora Studio
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs version)
endif()

create_test(
  NAME StringTests
  DEPENDS base
  LIBS arch
  SOURCE StringTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME UnicodeTests
  DEPENDS base
  LIBS arch ${extra_libs}
  SOURCE UnicodeTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME LogTests
  DEPENDS base
  LIBS arch ${extra_libs}
  SOURCE LogTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME BaseExceptionTests
  DEPENDS base
  LIBS arch
  SOURCE BaseExceptionTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME EventQueueTests
  DEPENDS base
  LIBS arch mt ${extra_libs}
  SOURCE EventQueueTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME ChangeDetectorTests
  DEPENDS base
  LIBS arch
  SOURCE ChangeDetectorTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME ThreadCancelTests
  DEPENDS mt
  LIBS base arch ${extra_libs}
  SOURCE ThreadCancelTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)
