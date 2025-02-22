// Copyright (C) 2017 The Qt Company Ltd.
// Copyright (C) 2015 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
// Copyright (C) 2016 Pelagicore AG
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qeglfskmsgbmscreen_p.h"
#include "qeglfskmsgbmdevice_p.h"
#include "qeglfskmsgbmcursor_p.h"
#include <private/qeglfsintegration_p.h>

#include <QtCore/QLoggingCategory>

#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qtguiglobal_p.h>
#include <QtFbSupport/private/qfbvthandler_p.h>

#include <errno.h>

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(qLcEglfsKmsDebug)

static inline uint32_t drmFormatToGbmFormat(uint32_t drmFormat)
{
    Q_ASSERT(DRM_FORMAT_XRGB8888 == GBM_FORMAT_XRGB8888);
    return drmFormat;
}

static inline uint32_t gbmFormatToDrmFormat(uint32_t gbmFormat)
{
    Q_ASSERT(DRM_FORMAT_XRGB8888 == GBM_FORMAT_XRGB8888);
    return gbmFormat;
}

void QEglFSKmsGbmScreen::bufferDestroyedHandler(gbm_bo *bo, void *data)
{
    FrameBuffer *fb = static_cast<FrameBuffer *>(data);

    if (fb->fb) {
        gbm_device *device = gbm_bo_get_device(bo);
        drmModeRmFB(gbm_device_get_fd(device), fb->fb);
    }

    delete fb;
}

QEglFSKmsGbmScreen::FrameBuffer *QEglFSKmsGbmScreen::framebufferForBufferObject(gbm_bo *bo)
{
    {
        FrameBuffer *fb = static_cast<FrameBuffer *>(gbm_bo_get_user_data(bo));
        if (fb)
            return fb;
    }

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
    uint32_t strides[4] = { gbm_bo_get_stride(bo) };
    uint32_t offsets[4] = { 0 };
    uint32_t pixelFormat = gbmFormatToDrmFormat(gbm_bo_get_format(bo));

    auto fb = std::make_unique<FrameBuffer>();
    qCDebug(qLcEglfsKmsDebug, "Adding FB, size %ux%u, DRM format 0x%x, stride %u, handle %u",
            width, height, pixelFormat, strides[0], handles[0]);

    int ret = drmModeAddFB2(device()->fd(), width, height, pixelFormat,
                            handles, strides, offsets, &fb->fb, 0);

    if (ret) {
        qWarning("Failed to create KMS FB!");
        return nullptr;
    }

    gbm_bo_set_user_data(bo, fb.get(), bufferDestroyedHandler);
    return fb.release();
}

QEglFSKmsGbmScreen::QEglFSKmsGbmScreen(QEglFSKmsDevice *device, const QKmsOutput &output, bool headless)
    : QEglFSKmsScreen(device, output, headless)
    , m_gbm_surface(nullptr)
    , m_gbm_bo_current(nullptr)
    , m_gbm_bo_next(nullptr)
    , m_flipPending(false)
    , m_cursor(nullptr)
    , m_cloneSource(nullptr)
{
}

QEglFSKmsGbmScreen::~QEglFSKmsGbmScreen()
{
    const int remainingScreenCount = qGuiApp->screens().count();
    qCDebug(qLcEglfsKmsDebug, "Screen dtor. Remaining screens: %d", remainingScreenCount);
    if (!remainingScreenCount && !device()->screenConfig()->separateScreens())
        static_cast<QEglFSKmsGbmDevice *>(device())->destroyGlobalCursor();
}

QPlatformCursor *QEglFSKmsGbmScreen::cursor() const
{
    QKmsScreenConfig *config = device()->screenConfig();
    if (config->headless())
        return nullptr;
    if (config->hwCursor()) {
        if (!config->separateScreens())
            return static_cast<QEglFSKmsGbmDevice *>(device())->globalCursor();

        if (m_cursor.isNull()) {
            QEglFSKmsGbmScreen *that = const_cast<QEglFSKmsGbmScreen *>(this);
            that->m_cursor.reset(new QEglFSKmsGbmCursor(that));
        }

        return m_cursor.data();
    } else {
        return QEglFSScreen::cursor();
    }
}

gbm_surface *QEglFSKmsGbmScreen::createSurface(EGLConfig eglConfig)
{
    if (!m_gbm_surface) {
        qCDebug(qLcEglfsKmsDebug, "Creating gbm_surface for screen %s", qPrintable(name()));

        const auto gbmDevice = static_cast<QEglFSKmsGbmDevice *>(device())->gbmDevice();
        // If there was no format override given in the config file,
        // query the native (here, gbm) format from the EGL config.
        const bool queryFromEgl = !m_output.drm_format_requested_by_user;
        if (queryFromEgl) {
            EGLint native_format = -1;
            EGLBoolean success = eglGetConfigAttrib(display(), eglConfig, EGL_NATIVE_VISUAL_ID, &native_format);
            qCDebug(qLcEglfsKmsDebug) << "Got native format" << Qt::hex << native_format << Qt::dec
                                      << "from eglGetConfigAttrib() with return code" << bool(success);

            if (success) {
                m_gbm_surface = gbm_surface_create(gbmDevice,
                                                   rawGeometry().width(),
                                                   rawGeometry().height(),
                                                   native_format,
                                                   gbmFlags());
                if (m_gbm_surface)
                    m_output.drm_format = gbmFormatToDrmFormat(native_format);
            }
        }

        // Fallback for older drivers, and when "format" is explicitly specified
        // in the output config. (not guaranteed that the requested format works
        // of course, but do what we are told to)
        if (!m_gbm_surface) {
            uint32_t gbmFormat = drmFormatToGbmFormat(m_output.drm_format);
            if (queryFromEgl)
                qCDebug(qLcEglfsKmsDebug, "Could not create surface with EGL_NATIVE_VISUAL_ID, falling back to format %x", gbmFormat);
            m_gbm_surface = gbm_surface_create(gbmDevice,
                                           rawGeometry().width(),
                                           rawGeometry().height(),
                                           gbmFormat,
                                           gbmFlags());
        }
    }
    return m_gbm_surface; // not owned, gets destroyed in QEglFSKmsGbmIntegration::destroyNativeWindow() via QEglFSKmsGbmWindow::invalidateSurface()
}

void QEglFSKmsGbmScreen::resetSurface()
{
    m_gbm_surface = nullptr;
}

void QEglFSKmsGbmScreen::initCloning(QPlatformScreen *screenThisScreenClones,
                                     const QList<QPlatformScreen *> &screensCloningThisScreen)
{
    // clone destinations need to know the clone source
    const bool clonesAnother = screenThisScreenClones != nullptr;
    if (clonesAnother && !screensCloningThisScreen.isEmpty()) {
        qWarning("QEglFSKmsGbmScreen %s cannot be clone source and destination at the same time", qPrintable(name()));
        return;
    }
    if (clonesAnother)
        m_cloneSource = static_cast<QEglFSKmsGbmScreen *>(screenThisScreenClones);

    // clone sources need to know their additional destinations
    for (QPlatformScreen *s : screensCloningThisScreen) {
        CloneDestination d;
        d.screen = static_cast<QEglFSKmsGbmScreen *>(s);
        m_cloneDests.append(d);
    }
}

void QEglFSKmsGbmScreen::ensureModeSet(uint32_t fb)
{
    QKmsOutput &op(output());
    const int fd = device()->fd();

    if (!op.mode_set) {
        op.mode_set = true;

        bool doModeSet = true;
        drmModeCrtcPtr currentMode = drmModeGetCrtc(fd, op.crtc_id);
        const bool alreadySet = currentMode && currentMode->buffer_id == fb && !memcmp(&currentMode->mode, &op.modes[op.mode], sizeof(drmModeModeInfo));
        if (currentMode)
            drmModeFreeCrtc(currentMode);
        if (alreadySet)
            doModeSet = false;

        if (doModeSet) {
            qCDebug(qLcEglfsKmsDebug, "Setting mode for screen %s", qPrintable(name()));

            if (device()->hasAtomicSupport()) {
#if QT_CONFIG(drm_atomic)
                drmModeAtomicReq *request = device()->threadLocalAtomicRequest();
                if (request) {
                    drmModeAtomicAddProperty(request, op.connector_id, op.crtcIdPropertyId, op.crtc_id);
                    drmModeAtomicAddProperty(request, op.crtc_id, op.modeIdPropertyId, op.mode_blob_id);
                    drmModeAtomicAddProperty(request, op.crtc_id, op.activePropertyId, 1);
                }
#endif
            } else {
                int ret = drmModeSetCrtc(fd,
                                         op.crtc_id,
                                         fb,
                                         0, 0,
                                         &op.connector_id, 1,
                                         &op.modes[op.mode]);

                if (ret == 0)
                    setPowerState(PowerStateOn);
                else
                    qErrnoWarning(errno, "Could not set DRM mode for screen %s", qPrintable(name()));
            }
        }
    }
}

void QEglFSKmsGbmScreen::waitForFlip()
{
    if (m_headless || m_cloneSource)
        return;

    // Don't lock the mutex unless we actually need to
    if (!m_gbm_bo_next)
        return;

    m_flipMutex.lock();
    device()->eventReader()->startWaitFlip(this, &m_flipMutex, &m_flipCond);
    m_flipCond.wait(&m_flipMutex);
    m_flipMutex.unlock();

    flipFinished();

#if QT_CONFIG(drm_atomic)
    device()->threadLocalAtomicReset();
#endif
}

void QEglFSKmsGbmScreen::flip()
{
    // For headless screen just return silently. It is not necessarily an error
    // to end up here, so show no warnings.
    if (m_headless)
        return;

    if (m_cloneSource) {
        qWarning("Screen %s clones another screen. swapBuffers() not allowed.", qPrintable(name()));
        return;
    }

    if (!m_gbm_surface) {
        qWarning("Cannot sync before platform init!");
        return;
    }

    m_gbm_bo_next = gbm_surface_lock_front_buffer(m_gbm_surface);
    if (!m_gbm_bo_next) {
        qWarning("Could not lock GBM surface front buffer!");
        return;
    }

    FrameBuffer *fb = framebufferForBufferObject(m_gbm_bo_next);
    ensureModeSet(fb->fb);

    QKmsOutput &op(output());
    const int fd = device()->fd();
    m_flipPending = true;

    if (device()->hasAtomicSupport()) {
#if QT_CONFIG(drm_atomic)
        drmModeAtomicReq *request = device()->threadLocalAtomicRequest();
        if (request) {
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->framebufferPropertyId, fb->fb);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->crtcPropertyId, op.crtc_id);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->srcwidthPropertyId,
                                     op.size.width() << 16);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->srcXPropertyId, 0);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->srcYPropertyId, 0);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->srcheightPropertyId,
                                     op.size.height() << 16);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->crtcXPropertyId, 0);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->crtcYPropertyId, 0);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->crtcwidthPropertyId,
                                     m_output.modes[m_output.mode].hdisplay);
            drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->crtcheightPropertyId,
                                     m_output.modes[m_output.mode].vdisplay);

            static int zpos = qEnvironmentVariableIntValue("QT_QPA_EGLFS_KMS_ZPOS");
            if (zpos)
                drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->zposPropertyId, zpos);
            static uint blendOp = uint(qEnvironmentVariableIntValue("QT_QPA_EGLFS_KMS_BLEND_OP"));
            if (blendOp)
                drmModeAtomicAddProperty(request, op.eglfs_plane->id, op.eglfs_plane->blendOpPropertyId, blendOp);
        }
#endif
    } else {
        int ret = drmModePageFlip(fd,
                              op.crtc_id,
                              fb->fb,
                              DRM_MODE_PAGE_FLIP_EVENT,
                              this);
        if (ret) {
            qErrnoWarning("Could not queue DRM page flip on screen %s", qPrintable(name()));
            m_flipPending = false;
            gbm_surface_release_buffer(m_gbm_surface, m_gbm_bo_next);
            m_gbm_bo_next = nullptr;
            return;
        }
    }

    for (CloneDestination &d : m_cloneDests) {
        if (d.screen != this) {
            d.screen->ensureModeSet(fb->fb);
            d.cloneFlipPending = true;
            QKmsOutput &destOutput(d.screen->output());

            if (device()->hasAtomicSupport()) {
#if QT_CONFIG(drm_atomic)
                drmModeAtomicReq *request = device()->threadLocalAtomicRequest();
                if (request) {
                    drmModeAtomicAddProperty(request, destOutput.eglfs_plane->id,
                                                      destOutput.eglfs_plane->framebufferPropertyId, fb->fb);
                    drmModeAtomicAddProperty(request, destOutput.eglfs_plane->id,
                                                      destOutput.eglfs_plane->crtcPropertyId, destOutput.crtc_id);
                }
#endif
            } else {
                int ret = drmModePageFlip(fd,
                                          destOutput.crtc_id,
                                          fb->fb,
                                          DRM_MODE_PAGE_FLIP_EVENT,
                                          d.screen);
                if (ret) {
                    qErrnoWarning("Could not queue DRM page flip for clone screen %s", qPrintable(name()));
                    d.cloneFlipPending = false;
                }
            }
        }
    }

#if QT_CONFIG(drm_atomic)
    device()->threadLocalAtomicCommit(this);
#endif
}

void QEglFSKmsGbmScreen::flipFinished()
{
    if (m_cloneSource) {
        m_cloneSource->cloneDestFlipFinished(this);
        return;
    }

    m_flipPending = false;
    updateFlipStatus();
}

void QEglFSKmsGbmScreen::cloneDestFlipFinished(QEglFSKmsGbmScreen *cloneDestScreen)
{
    for (CloneDestination &d : m_cloneDests) {
        if (d.screen == cloneDestScreen) {
            d.cloneFlipPending = false;
            break;
        }
    }
    updateFlipStatus();
}

void QEglFSKmsGbmScreen::updateFlipStatus()
{
    Q_ASSERT(!m_cloneSource);

    if (m_flipPending)
        return;

    for (const CloneDestination &d : qAsConst(m_cloneDests)) {
        if (d.cloneFlipPending)
            return;
    }

    if (m_gbm_bo_current)
        gbm_surface_release_buffer(m_gbm_surface,
                                   m_gbm_bo_current);

    m_gbm_bo_current = m_gbm_bo_next;
    m_gbm_bo_next = nullptr;
}

QT_END_NAMESPACE
