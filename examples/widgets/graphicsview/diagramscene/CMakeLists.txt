# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)
project(diagramscene LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)

if(NOT DEFINED INSTALL_EXAMPLESDIR)
    set(INSTALL_EXAMPLESDIR "examples")
endif()

set(INSTALL_EXAMPLEDIR "${INSTALL_EXAMPLESDIR}/widgets/graphicsview/diagramscene")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_add_executable(diagramscene
    arrow.cpp arrow.h
    diagramitem.cpp diagramitem.h
    diagramscene.cpp diagramscene.h
    diagramtextitem.cpp diagramtextitem.h
    main.cpp
    mainwindow.cpp mainwindow.h
)

set_target_properties(diagramscene PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(diagramscene PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Widgets
)

# Resources:
set(diagramscene_resource_files
    "images/background1.png"
    "images/background2.png"
    "images/background3.png"
    "images/background4.png"
    "images/bold.png"
    "images/bringtofront.png"
    "images/delete.png"
    "images/floodfill.png"
    "images/italic.png"
    "images/linecolor.png"
    "images/linepointer.png"
    "images/pointer.png"
    "images/sendtoback.png"
    "images/textpointer.png"
    "images/underline.png"
)

qt_add_resources(diagramscene "diagramscene"
    PREFIX
        "/"
    FILES
        ${diagramscene_resource_files}
)

install(TARGETS diagramscene
    RUNTIME DESTINATION "${INSTALL_EXAMPLEDIR}"
    BUNDLE DESTINATION "${INSTALL_EXAMPLEDIR}"
    LIBRARY DESTINATION "${INSTALL_EXAMPLEDIR}"
)
