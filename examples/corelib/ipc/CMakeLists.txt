# Copyright (C) 2022 The Qt Company Ltd.
# SPDX-License-Identifier: BSD-3-Clause

if(NOT TARGET Qt::Widgets)
    return()
endif()
if(QT_FEATURE_sharedmemory)
    qt_internal_add_example(sharedmemory)
endif()
if(QT_FEATURE_localserver AND TARGET Qt::Network)
    qt_internal_add_example(localfortuneserver)
    qt_internal_add_example(localfortuneclient)
endif()
