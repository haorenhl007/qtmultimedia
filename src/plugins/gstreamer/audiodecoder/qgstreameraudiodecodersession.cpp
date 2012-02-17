/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/
//#define DEBUG_DECODER

#include "qgstreameraudiodecodersession.h"
#include <private/qgstreamerbushelper_p.h>

#include <private/qgstutils_p.h>

#include <gst/gstvalue.h>
#include <gst/base/gstbasesrc.h>

#include <QtCore/qdatetime.h>
#include <QtCore/qdebug.h>
#include <QtCore/qsize.h>
#include <QtCore/qtimer.h>
#include <QtCore/qdebug.h>
#include <QtCore/qdir.h>
#include <QtCore/qstandardpaths.h>
#include <QtCore/qurl.h>

#define MAX_BUFFERS_IN_QUEUE 4

QT_BEGIN_NAMESPACE

typedef enum {
    GST_PLAY_FLAG_VIDEO         = 0x00000001,
    GST_PLAY_FLAG_AUDIO         = 0x00000002,
    GST_PLAY_FLAG_TEXT          = 0x00000004,
    GST_PLAY_FLAG_VIS           = 0x00000008,
    GST_PLAY_FLAG_SOFT_VOLUME   = 0x00000010,
    GST_PLAY_FLAG_NATIVE_AUDIO  = 0x00000020,
    GST_PLAY_FLAG_NATIVE_VIDEO  = 0x00000040,
    GST_PLAY_FLAG_DOWNLOAD      = 0x00000080,
    GST_PLAY_FLAG_BUFFERING     = 0x000000100
} GstPlayFlags;

QGstreamerAudioDecoderSession::QGstreamerAudioDecoderSession(QObject *parent)
    : QObject(parent),
     m_state(QAudioDecoder::StoppedState),
     m_pendingState(QAudioDecoder::StoppedState),
     m_busHelper(0),
     m_bus(0),
     m_playbin(0),
     m_outputBin(0),
     m_audioConvert(0),
     m_appSink(0),
#if defined(HAVE_GST_APPSRC)
     m_appSrc(0),
#endif
     mDevice(0),
     m_buffersAvailable(0)
{
    // Create pipeline here
    m_playbin = gst_element_factory_make("playbin2", NULL);

    if (m_playbin != 0) {
        // Sort out messages
        m_bus = gst_element_get_bus(m_playbin);
        m_busHelper = new QGstreamerBusHelper(m_bus, this);
        m_busHelper->installMessageFilter(this);

        // Set the rest of the pipeline up
        setAudioFlags(true);

        m_audioConvert = gst_element_factory_make("audioconvert", NULL);

        m_outputBin = gst_bin_new("audio-output-bin");
        gst_bin_add(GST_BIN(m_outputBin), m_audioConvert);

        // add ghostpad
        GstPad *pad = gst_element_get_static_pad(m_audioConvert, "sink");
        Q_ASSERT(pad);
        gst_element_add_pad(GST_ELEMENT(m_outputBin), gst_ghost_pad_new("sink", pad));
        gst_object_unref(GST_OBJECT(pad));

        g_object_set(G_OBJECT(m_playbin), "audio-sink", m_outputBin, NULL);
        g_signal_connect(G_OBJECT(m_playbin), "deep-notify::source", (GCallback) &QGstreamerAudioDecoderSession::configureAppSrcElement, (gpointer)this);

        // Set volume to 100%
        gdouble volume = 1.0;
        g_object_set(G_OBJECT(m_playbin), "volume", volume, NULL);
    }
}

QGstreamerAudioDecoderSession::~QGstreamerAudioDecoderSession()
{
    if (m_playbin) {
        stop();

        delete m_busHelper;
#if defined(HAVE_GST_APPSRC)
        delete m_appSrc;
#endif
        gst_object_unref(GST_OBJECT(m_bus));
        gst_object_unref(GST_OBJECT(m_playbin));
    }
}

#if defined(HAVE_GST_APPSRC)
void QGstreamerAudioDecoderSession::configureAppSrcElement(GObject* object, GObject *orig, GParamSpec *pspec, QGstreamerAudioDecoderSession* self)
{
    Q_UNUSED(object);
    Q_UNUSED(pspec);

    // In case we switch from appsrc to not
    if (!self->appsrc())
        return;

    if (self->appsrc()->isReady())
        return;

    GstElement *appsrc;
    g_object_get(orig, "source", &appsrc, NULL);

    if (!self->appsrc()->setup(appsrc))
        qWarning()<<"Could not setup appsrc element";
}
#endif

bool QGstreamerAudioDecoderSession::processBusMessage(const QGstreamerMessage &message)
{
    GstMessage* gm = message.rawMessage();
    if (gm) {
        if (GST_MESSAGE_SRC(gm) == GST_OBJECT_CAST(m_playbin)) {
            switch (GST_MESSAGE_TYPE(gm))  {
            case GST_MESSAGE_STATE_CHANGED:
                {
                    GstState    oldState;
                    GstState    newState;
                    GstState    pending;

                    gst_message_parse_state_changed(gm, &oldState, &newState, &pending);

#ifdef DEBUG_DECODER
                    QStringList states;
                    states << "GST_STATE_VOID_PENDING" <<  "GST_STATE_NULL" << "GST_STATE_READY" << "GST_STATE_PAUSED" << "GST_STATE_PLAYING";

                    qDebug() << QString("state changed: old: %1  new: %2  pending: %3") \
                            .arg(states[oldState]) \
                            .arg(states[newState]) \
                                .arg(states[pending]) << "internal" << m_state;
#endif

                    switch (newState) {
                    case GST_STATE_VOID_PENDING:
                    case GST_STATE_NULL:
                        if (m_state != QAudioDecoder::StoppedState)
                            emit stateChanged(m_state = QAudioDecoder::StoppedState);
                        break;
                    case GST_STATE_READY:
                        if (m_state != QAudioDecoder::StoppedState)
                            emit stateChanged(m_state = QAudioDecoder::StoppedState);
                        break;
                    case GST_STATE_PLAYING:
                        if (m_state != QAudioDecoder::DecodingState)
                            emit stateChanged(m_state = QAudioDecoder::DecodingState);
                        break;
                    case GST_STATE_PAUSED:
                        if (m_state != QAudioDecoder::WaitingState)
                            emit stateChanged(m_state = QAudioDecoder::WaitingState);
                        break;
                    }
                }
                break;

            case GST_MESSAGE_EOS:
                emit stateChanged(m_state = QAudioDecoder::StoppedState);
                break;

            case GST_MESSAGE_ERROR: {
                    GError *err;
                    gchar *debug;
                    gst_message_parse_error(gm, &err, &debug);
                    if (err->domain == GST_STREAM_ERROR && err->code == GST_STREAM_ERROR_CODEC_NOT_FOUND)
                        processInvalidMedia(QAudioDecoder::FormatError, tr("Cannot play stream of type: <unknown>"));
                    else
                        processInvalidMedia(QAudioDecoder::ResourceError, QString::fromUtf8(err->message));
                    qWarning() << "Error:" << QString::fromUtf8(err->message);
                    g_error_free(err);
                    g_free(debug);
                }
                break;
            case GST_MESSAGE_WARNING:
                {
                    GError *err;
                    gchar *debug;
                    gst_message_parse_warning (gm, &err, &debug);
                    qWarning() << "Warning:" << QString::fromUtf8(err->message);
                    g_error_free (err);
                    g_free (debug);
                }
                break;
#ifdef DEBUG_DECODER
            case GST_MESSAGE_INFO:
                {
                    GError *err;
                    gchar *debug;
                    gst_message_parse_info (gm, &err, &debug);
                    qDebug() << "Info:" << QString::fromUtf8(err->message);
                    g_error_free (err);
                    g_free (debug);
                }
                break;
#endif
            default:
                break;
            }
        } else if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_ERROR) {
            GError *err;
            gchar *debug;
            gst_message_parse_error(gm, &err, &debug);
            // If the source has given up, so do we.
            if (qstrcmp(GST_OBJECT_NAME(GST_MESSAGE_SRC(gm)), "source") == 0) {
                processInvalidMedia(QAudioDecoder::ResourceError, QString::fromUtf8(err->message));
            } else if (err->domain == GST_STREAM_ERROR
                       && (err->code == GST_STREAM_ERROR_DECRYPT || err->code == GST_STREAM_ERROR_DECRYPT_NOKEY)) {
                processInvalidMedia(QAudioDecoder::AccessDeniedError, QString::fromUtf8(err->message));
            }
            g_error_free(err);
            g_free(debug);
        }
    }

    return false;
}

QString QGstreamerAudioDecoderSession::sourceFilename() const
{
    return mSource;
}

void QGstreamerAudioDecoderSession::setSourceFilename(const QString &fileName)
{
    stop();
    mDevice = 0;
    if (m_appSrc)
        m_appSrc->deleteLater();
    m_appSrc = 0;

    bool isSignalRequired = (mSource != fileName);
    mSource = fileName;
    if (isSignalRequired)
        emit sourceChanged();
}

QIODevice *QGstreamerAudioDecoderSession::sourceDevice() const
{
    return mDevice;
}

void QGstreamerAudioDecoderSession::setSourceDevice(QIODevice *device)
{
    stop();
    mSource.clear();
    bool isSignalRequired = (mDevice != device);
    mDevice = device;
    if (isSignalRequired)
        emit sourceChanged();
}

void QGstreamerAudioDecoderSession::start()
{
    if (!m_playbin) {
        processInvalidMedia(QAudioDecoder::ResourceError, "Playbin element is not valid");
        return;
    }

    addAppSink();

    if (!mSource.isEmpty()) {
        g_object_set(G_OBJECT(m_playbin), "uri", QUrl::fromLocalFile(mSource).toEncoded().constData(), NULL);
    } else if (mDevice) {
#if defined(HAVE_GST_APPSRC)
        // make sure we can read from device
        if (!mDevice->isOpen() || !mDevice->isReadable()) {
            processInvalidMedia(QAudioDecoder::AccessDeniedError, "Unable to read from specified device");
            return;
        }

        if (m_appSrc)
            m_appSrc->deleteLater();
        m_appSrc = new QGstAppSrc(this);
        m_appSrc->setStream(mDevice);

        g_object_set(G_OBJECT(m_playbin), "uri", "appsrc://", NULL);
#endif
    } else {
        return;
    }

    // Set audio format
    if (m_appSink) {
        if (mFormat.isValid()) {
            setAudioFlags(false);
            GstCaps *caps = QGstUtils::capsForAudioFormat(mFormat);
            gst_app_sink_set_caps(m_appSink, caps); // appsink unrefs caps
        } else {
            // We want whatever the native audio format is
            setAudioFlags(true);
            gst_app_sink_set_caps(m_appSink, NULL);
        }
    }

    m_pendingState = QAudioDecoder::DecodingState;
    if (gst_element_set_state(m_playbin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "GStreamer; Unable to start decoding process";
        m_pendingState = m_state = QAudioDecoder::StoppedState;

        emit stateChanged(m_state);
    }
}

void QGstreamerAudioDecoderSession::stop()
{
    if (m_playbin) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        removeAppSink();

        QAudioDecoder::State oldState = m_state;
        m_pendingState = m_state = QAudioDecoder::StoppedState;

        // GStreamer thread is stopped. Can safely access m_buffersAvailable
        if (m_buffersAvailable != 0) {
            m_buffersAvailable = 0;
            emit bufferAvailableChanged(false);
        }

        if (oldState != m_state)
            emit stateChanged(m_state);
    }
}

QAudioFormat QGstreamerAudioDecoderSession::audioFormat() const
{
    return mFormat;
}

void QGstreamerAudioDecoderSession::setAudioFormat(const QAudioFormat &format)
{
    if (mFormat != format) {
        mFormat = format;
        emit formatChanged(mFormat);
    }
}

QAudioBuffer QGstreamerAudioDecoderSession::read(bool *ok)
{
    QAudioBuffer audioBuffer;

    int buffersAvailable;
    {
        QMutexLocker locker(&m_buffersMutex);
        buffersAvailable = m_buffersAvailable;

        // need to decrement before pulling a buffer
        // to make sure assert in QGstreamerAudioDecoderSession::new_buffer works
        m_buffersAvailable--;
    }


    if (buffersAvailable) {
        if (buffersAvailable == 1)
            emit bufferAvailableChanged(false);

        GstBuffer *buffer = gst_app_sink_pull_buffer(m_appSink);

        QAudioFormat format = QGstUtils::audioFormatForBuffer(buffer);
        if (format.isValid()) {
            // XXX At the moment we have to copy data from GstBuffer into QAudioBuffer.
            // We could improve performance by implementing QAbstractAudioBuffer for GstBuffer.
            audioBuffer = QAudioBuffer(QByteArray((const char*)buffer->data, buffer->size), format);
        }
        gst_buffer_unref(buffer);
    }

    if (ok)
        *ok = audioBuffer.isValid();
    return audioBuffer;
}

bool QGstreamerAudioDecoderSession::bufferAvailable() const
{
    QMutexLocker locker(&m_buffersMutex);
    return m_buffersAvailable;
}

void QGstreamerAudioDecoderSession::processInvalidMedia(QAudioDecoder::Error errorCode, const QString& errorString)
{
    stop();
    emit error(int(errorCode), errorString);
}

GstFlowReturn QGstreamerAudioDecoderSession::new_buffer(GstAppSink *, gpointer user_data)
{
    // "Note that the preroll buffer will also be returned as the first buffer when calling gst_app_sink_pull_buffer()."
    QGstreamerAudioDecoderSession *session = reinterpret_cast<QGstreamerAudioDecoderSession*>(user_data);

    int buffersAvailable;
    {
        QMutexLocker locker(&session->m_buffersMutex);
        buffersAvailable = session->m_buffersAvailable;
        session->m_buffersAvailable++;
        Q_ASSERT(session->m_buffersAvailable <= MAX_BUFFERS_IN_QUEUE);
    }

    if (!buffersAvailable)
        QMetaObject::invokeMethod(session, "bufferAvailableChanged", Qt::QueuedConnection, Q_ARG(bool, true));
    QMetaObject::invokeMethod(session, "bufferReady", Qt::QueuedConnection);
    return GST_FLOW_OK;
}

void QGstreamerAudioDecoderSession::setAudioFlags(bool wantNativeAudio)
{
    int flags = 0;
    if (m_playbin) {
        g_object_get(G_OBJECT(m_playbin), "flags", &flags, NULL);
        // make sure not to use GST_PLAY_FLAG_NATIVE_AUDIO unless desired
        // it prevents audio format conversion
        flags &= ~(GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_NATIVE_VIDEO | GST_PLAY_FLAG_TEXT | GST_PLAY_FLAG_VIS | GST_PLAY_FLAG_NATIVE_AUDIO);
        flags |= GST_PLAY_FLAG_AUDIO;
        if (wantNativeAudio)
            flags |= GST_PLAY_FLAG_NATIVE_AUDIO;
        g_object_set(G_OBJECT(m_playbin), "flags", flags, NULL);
    }
}

void QGstreamerAudioDecoderSession::addAppSink()
{
    if (m_appSink)
        return;

    m_appSink = (GstAppSink*)gst_element_factory_make("appsink", NULL);

    GstAppSinkCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.new_buffer = &new_buffer;
    gst_app_sink_set_callbacks(m_appSink, &callbacks, this, NULL);
    gst_app_sink_set_max_buffers(m_appSink, MAX_BUFFERS_IN_QUEUE);
    gst_base_sink_set_sync(GST_BASE_SINK(m_appSink), FALSE);

    gst_bin_add(GST_BIN(m_outputBin), GST_ELEMENT(m_appSink));
    gst_element_link(m_audioConvert, GST_ELEMENT(m_appSink));
}

void QGstreamerAudioDecoderSession::removeAppSink()
{
    if (!m_appSink)
        return;

    gst_element_unlink(m_audioConvert, GST_ELEMENT(m_appSink));
    gst_bin_remove(GST_BIN(m_outputBin), GST_ELEMENT(m_appSink));

    m_appSink = 0;
}

QT_END_NAMESPACE
