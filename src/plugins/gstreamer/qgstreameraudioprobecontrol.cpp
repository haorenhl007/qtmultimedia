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

#include "qgstreameraudioprobecontrol.h"
#include <private/qgstutils_p.h>

QGstreamerAudioProbeControl::QGstreamerAudioProbeControl(QObject *parent)
    : QMediaAudioProbeControl(parent)
{

}

QGstreamerAudioProbeControl::~QGstreamerAudioProbeControl()
{

}

void QGstreamerAudioProbeControl::bufferProbed(GstBuffer* buffer)
{
    GstCaps* caps = gst_buffer_get_caps(buffer);
    if (!caps)
        return;

    QAudioFormat format = QGstUtils::audioFormatForCaps(caps);
    gst_caps_unref(caps);
    if (!format.isValid())
        return;

    QAudioBuffer audioBuffer = QAudioBuffer(QByteArray((const char*)buffer->data, buffer->size), format);

    {
        QMutexLocker locker(&m_bufferMutex);
        m_pendingBuffer = audioBuffer;
        QMetaObject::invokeMethod(this, "bufferProbed", Qt::QueuedConnection);
    }
}

void QGstreamerAudioProbeControl::bufferProbed()
{
    QAudioBuffer audioBuffer;
    {
        QMutexLocker locker(&m_bufferMutex);
        if (!m_pendingBuffer.isValid())
            return;
        audioBuffer = m_pendingBuffer;
    }
    emit audioBufferProbed(audioBuffer);
}
