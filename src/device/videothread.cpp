/*
   DivvyDroid - Application to screencast and remote control Android devices.

   Copyright (C) 2019 - Mladen Milinkovic <maxrd2@smoothware.net>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "videothread.h"
#include "deviceinfo.h"
#include <QImage>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

VideoThread::VideoThread(QObject *parent)
    : QThread(parent)
{
}

VideoThread::~VideoThread()
{
	requestInterruption();
	wait();
}

void VideoThread::setImageSize(int w, int h)
{
    m_imageWidth = w;
    m_imageHeight = h;
}

void VideoThread::setNativeInterval(unsigned long ms)
{
    m_nativeInterval = ms;
}

void VideoThread::setVideoMode(VideMode mode)
{
    m_videoMode = mode;
}

const char *VideoThread::h264Error(int errorCode)
{
	static char errorText[1024];
	av_strerror(errorCode, errorText, sizeof(errorText));
	return errorText;
}

bool VideoThread::h264Connect()
{
    if (!m_adb->connectToDevice()) {
        return false;
    }

    QByteArray cmd("shell:stty raw; screenrecord --output-format=h264 --size ");
    cmd.append(QString::number(m_imageWidth).toUtf8())
        .append('x')
        .append(QString::number(m_imageHeight).toUtf8())
        .append(" -");

    if (!m_adb->send(cmd)) {
        qWarning() << "FRAMEBUFFER error executing" << cmd.mid(6);
        return false;
    }

    qDebug() << "FRAMEBUFFER connected";
    return true;
}

bool VideoThread::h264Init()
{
    auto read_packet = [](void *u, uint8_t *buf, int buf_size) -> int {
        VideoThread *me = reinterpret_cast<VideoThread *>(u);
        qint64 len = me->m_adb->bytesAvailable();
        while (len == 0) {
            if (!me->m_adb->isConnected() && !me->h264Connect()) {
                return -1;
            }
            const bool res = me->m_adb->waitForReadyRead(50);
            if (me->isInterruptionRequested()) {
                return -1;
            }
            if (!res) {
                const QTcpSocket::SocketError err = me->m_adb->error();
                if (err == QTcpSocket::RemoteHostClosedError) {
                    qDebug() << "FRAMEBUFFER host disconnected " << err;
                    return -1;
                }
                if (err != QTcpSocket::SocketTimeoutError) {
                    qDebug() << "FRAMEBUFFER read failed:" << err;
                    return -1;
                }
            } else {
                len = me->m_adb->bytesAvailable();
            }
        }
        if (len > buf_size) {
            len = buf_size;
        }
        if (!me->m_adb->read(buf, len)) {
            return -1;
        }
        return len;
    };

    const int bufSize = 8192;
    unsigned char *buf = reinterpret_cast<unsigned char *>(av_malloc(bufSize));

    // AVFormatContext
    m_avFormat = avformat_alloc_context();
    Q_ASSERT(m_avFormat != nullptr);
    m_avFormat->pb = avio_alloc_context(buf, bufSize, 0, this, read_packet, nullptr, nullptr);
    Q_ASSERT(m_avFormat->pb != nullptr);

    // AVFrame
    m_frame = av_frame_alloc();
    Q_ASSERT(m_frame != nullptr);

    // AVFrame
    m_rgbFrame = av_frame_alloc();
    Q_ASSERT(m_rgbFrame != nullptr);
    av_image_alloc(m_rgbFrame->data,
                   m_rgbFrame->linesize,
                   m_imageWidth,
                   m_imageHeight,
                   AV_PIX_FMT_RGB24,
                   32);

    int ret{};
    if ((ret = avformat_open_input(&m_avFormat, nullptr, nullptr, nullptr)) < 0) {
        qDebug() << "FRAMEBUFFER can't open input:" << h264Error(ret);
        return false;
    }

    m_avFormat->probesize = 32;
    //	m_avFormat->max_analyze_duration = 0;
    if ((ret = avformat_find_stream_info(m_avFormat, nullptr)) < 0) {
        qDebug() << "FRAMEBUFFER can't find stream information:" << h264Error(ret);
        return false;
    }
    av_dump_format(m_avFormat, 0, "", 0);

    return true;
}

int VideoThread::h264VideoStreamIndex()
{
    int streamIndex{-1};
    int ret{};

    for (unsigned int i = 0; i < m_avFormat->nb_streams; i++) {
        m_avStream = m_avFormat->streams[i];
        if (m_avStream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;

        const AVCodec *dec = avcodec_find_decoder(m_avStream->codecpar->codec_id);
        if (!dec) {
            qDebug() << "FRAMEBUFFER can't find decoder for stream" << i;
            continue;
        }

        m_codecCtx = avcodec_alloc_context3(dec);
        if (!m_codecCtx) {
            qDebug() << "FRAMEBUFFER can't allocate the decoder context for stream" << i;
            continue;
        }
        ret = avcodec_parameters_to_context(m_codecCtx, m_avStream->codecpar);
        if (ret < 0) {
            qDebug() << "FRAMEBUFFER failed to copy decoder parameters to input decoder context "
                        "for stream"
                     << i << h264Error(ret);
            avcodec_free_context(&m_codecCtx);
            continue;
        }
        if (m_codecCtx->codec_type != AVMEDIA_TYPE_VIDEO) {
            avcodec_free_context(&m_codecCtx);
            continue;
        }
        ret = avcodec_open2(m_codecCtx, dec, nullptr);
        if (ret < 0) {
            qDebug() << "FRAMEBUFFER failed to open decoder for stream" << i << h264Error(ret);
            avcodec_free_context(&m_codecCtx);
            continue;
        }

        m_swsContext = sws_getContext(m_codecCtx->width,
                                      m_codecCtx->height,
                                      m_codecCtx->pix_fmt,
                                      m_imageWidth,
                                      m_imageHeight,
                                      AV_PIX_FMT_RGB24,
                                      SWS_BICUBIC,
                                      nullptr,
                                      nullptr,
                                      nullptr);

        streamIndex = i;
    }

    return streamIndex;
}

bool VideoThread::h264Loop()
{
	if(!h264Init()) {
		h264Exit();
		return false;
	}

	int streamIndex = h264VideoStreamIndex();
	if(streamIndex == -1) {
		h264Exit();
		return false;
	}

	AVPacket pkt;
	pkt.data = nullptr;
	pkt.size = 0;
	bool res = false;

	while(!isInterruptionRequested()) {
        int ret = av_read_frame(m_avFormat, &pkt);
        bool drainDecoder = ret == AVERROR(EAGAIN) || ret == AVERROR_EOF;
        if (ret < 0 && !drainDecoder) {
            qDebug() << "FRAMEBUFFER av_read_frame() failed:" << h264Error(ret);
            break;
        }
        if (pkt.stream_index == streamIndex || drainDecoder) {
            ret = avcodec_send_packet(m_codecCtx, &pkt);
            if (ret < 0) {
                if (ret != AVERROR(EAGAIN)) {
                    qDebug() << "FRAMEBUFFER avcodec_send_packet() failed:" << h264Error(ret);
                    break;
                }
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(m_codecCtx, m_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    qDebug() << "FRAMEBUFFER avcodec_receive_frame() failed:" << h264Error(ret);
                    break;
                }

                sws_scale(m_swsContext,
                          m_frame->data,
                          m_frame->linesize,
                          0,
                          m_imageHeight,
                          m_rgbFrame->data,
                          m_rgbFrame->linesize);

                QImage img(m_imageWidth, m_imageHeight, QImage::Format_RGB888);
                const uint8_t *data = m_rgbFrame->data[0];
                for (int y = 0; y < m_imageHeight; y++) {
                    memcpy(img.scanLine(y), data, img.bytesPerLine());
                    data += m_rgbFrame->linesize[0];
                }

                auto s = img.size();
                res = true;
                emit imageReady(img);
            }
        }

        av_packet_unref(&pkt);

        if (drainDecoder) {
            break;
        }
    }

    h264Exit();
    return res;
}

void VideoThread::h264Exit()
{
    if (m_swsContext) {
        sws_freeContext(m_swsContext);
    }
    av_frame_free(&m_frame);
    av_freep(&m_rgbFrame->data[0]);
    av_frame_free(&m_rgbFrame);
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_avFormat) {
        AVIOContext *ioContext = m_avFormat->pb;
        avformat_close_input(&m_avFormat);
        avio_context_free(&ioContext);
    }
}

void VideoThread::nativeLoop()
{
    while (!isInterruptionRequested()) {
        QImage img;
        if (aDev->isScreenAwake()) {
            if (m_videoMode == NativeJpg) {
                img = m_adb->fetchScreenJpeg();
            } else if (m_videoMode == NativeRaw) {
                img = m_adb->fetchScreenRaw();
            } else if (m_videoMode == NativePng) {
                img = m_adb->fetchScreenPng();
            } else {
                return;
            }
        } else {
            img = QImage(m_imageWidth, m_imageHeight, QImage::Format_RGB888);
            img.fill(Qt::black);
        }
        emit imageReady(img.scaledToWidth(m_imageWidth, Qt::SmoothTransformation));
        msleep(m_nativeInterval);
    }
}

void VideoThread::run()
{
    m_adb = new AdbClient();
    if (m_videoMode == FastH264) {
        h264Loop();
    } else {
        nativeLoop();
    }
    m_adb->close();
    m_adb->waitForDisconnected();
    m_adb->deleteLater();
}
