/*
 * DirectShow parser filters
 *
 * Copyright 2010 Maarten Lankhorst for CodeWeavers
 * Copyright 2010 Aric Stewart for CodeWeavers
 * Copyright 2019-2020 Zebediah Figura
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "gst_private.h"
#include "gst_guids.h"
#include "gst_cbs.h"

#include "vfwmsgs.h"
#include "amvideo.h"

#include "wine/unicode.h"
#include "wine/debug.h"

#include <assert.h>

#include "dvdmedia.h"
#include "mmreg.h"
#include "ks.h"
#include "initguid.h"
#include "wmcodecdsp.h"
#include "ksmedia.h"

WINE_DEFAULT_DEBUG_CHANNEL(gstreamer);

static const GUID MEDIASUBTYPE_CVID = {mmioFOURCC('c','v','i','d'), 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID MEDIASUBTYPE_MP3  = {WAVE_FORMAT_MPEGLAYER3, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct parser
{
    struct strmbase_filter filter;
    IAMStreamSelect IAMStreamSelect_iface;

    struct strmbase_sink sink;
    IAsyncReader *reader;

    struct parser_source **sources;
    unsigned int source_count;
    BOOL enum_sink_first;

    LONGLONG file_size;

    struct wg_parser *wg_parser;

    /* FIXME: It would be nice to avoid duplicating these with strmbase.
     * However, synchronization is tricky; we need access to be protected by a
     * separate lock. */
    bool streaming, sink_connected;

    uint64_t next_pull_offset;

    HANDLE read_thread;

    BOOL (*init_gst)(struct parser *filter);
    HRESULT (*source_query_accept)(struct parser_source *pin, const AM_MEDIA_TYPE *mt);
    HRESULT (*source_get_media_type)(struct parser_source *pin, unsigned int index, AM_MEDIA_TYPE *mt);
};

struct parser_source
{
    struct strmbase_source pin;
    IQualityControl IQualityControl_iface;

    struct wg_parser_stream *wg_stream;

    SourceSeeking seek;

    CRITICAL_SECTION flushing_cs;
    HANDLE thread;
};

static inline struct parser *impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, struct parser, filter);
}

static const WCHAR wcsInputPinName[] = {'i','n','p','u','t',' ','p','i','n',0};
static const IMediaSeekingVtbl GST_Seeking_Vtbl;
static const IQualityControlVtbl GSTOutPin_QualityControl_Vtbl;

static struct parser_source *create_pin(struct parser *filter,
        struct wg_parser_stream *stream, const WCHAR *name);
static HRESULT GST_RemoveOutputPins(struct parser *This);
static HRESULT WINAPI GST_ChangeCurrent(IMediaSeeking *iface);
static HRESULT WINAPI GST_ChangeStop(IMediaSeeking *iface);
static HRESULT WINAPI GST_ChangeRate(IMediaSeeking *iface);

static DWORD channel_mask_from_count(uint32_t count)
{
    switch (count)
    {
        case 1: return KSAUDIO_SPEAKER_MONO;
        case 2: return KSAUDIO_SPEAKER_STEREO;
        case 4: return KSAUDIO_SPEAKER_SURROUND;
        case 5: return KSAUDIO_SPEAKER_5POINT1 & ~SPEAKER_LOW_FREQUENCY;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1;
        default: return 0;
    }
}

static bool amt_from_wg_format_audio(AM_MEDIA_TYPE *mt, const struct wg_format *format)
{
    mt->majortype = MEDIATYPE_Audio;
    mt->formattype = FORMAT_WaveFormatEx;

    switch (format->u.audio.format)
    {
    case WG_AUDIO_FORMAT_UNKNOWN:
        return false;

    case WG_AUDIO_FORMAT_MPEG1_LAYER1:
    case WG_AUDIO_FORMAT_MPEG1_LAYER2:
    {
        MPEG1WAVEFORMAT *wave_format;

        if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
            return false;
        memset(wave_format, 0, sizeof(*wave_format));

        mt->subtype = MEDIASUBTYPE_MPEG1AudioPayload;
        mt->cbFormat = sizeof(*wave_format);
        mt->pbFormat = (BYTE *)wave_format;
        wave_format->wfx.wFormatTag = WAVE_FORMAT_MPEG;
        wave_format->wfx.nChannels = format->u.audio.channels;
        wave_format->wfx.nSamplesPerSec = format->u.audio.rate;
        wave_format->wfx.cbSize = sizeof(*wave_format) - sizeof(WAVEFORMATEX);
        wave_format->fwHeadLayer = (format->u.audio.format == WG_AUDIO_FORMAT_MPEG1_LAYER1 ? 1 : 2);
        return true;
    }

    case WG_AUDIO_FORMAT_MPEG1_LAYER3:
    {
        MPEGLAYER3WAVEFORMAT *wave_format;

        if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
            return false;
        memset(wave_format, 0, sizeof(*wave_format));

        mt->subtype = MEDIASUBTYPE_MP3;
        mt->cbFormat = sizeof(*wave_format);
        mt->pbFormat = (BYTE *)wave_format;
        wave_format->wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
        wave_format->wfx.nChannels = format->u.audio.channels;
        wave_format->wfx.nSamplesPerSec = format->u.audio.rate;
        wave_format->wfx.cbSize = sizeof(*wave_format) - sizeof(WAVEFORMATEX);
        /* FIXME: We can't get most of the MPEG data from the caps. We may have
         * to manually parse the header. */
        wave_format->wID = MPEGLAYER3_ID_MPEG;
        wave_format->fdwFlags = MPEGLAYER3_FLAG_PADDING_ON;
        wave_format->nFramesPerBlock = 1;
        wave_format->nCodecDelay = 1393;
        return true;
    }

    case WG_AUDIO_FORMAT_U8:
    case WG_AUDIO_FORMAT_S16LE:
    case WG_AUDIO_FORMAT_S24LE:
    case WG_AUDIO_FORMAT_S32LE:
    case WG_AUDIO_FORMAT_F32LE:
    case WG_AUDIO_FORMAT_F64LE:
    {
        static const struct
        {
            bool is_float;
            WORD depth;
        }
        format_table[] =
        {
            {0},
            {false, 8},
            {false, 16},
            {false, 24},
            {false, 32},
            {true, 32},
            {true, 64},
        };

        bool is_float;
        WORD depth;

        assert(format->u.audio.format < ARRAY_SIZE(format_table));
        is_float = format_table[format->u.audio.format].is_float;
        depth = format_table[format->u.audio.format].depth;

        if (is_float || format->u.audio.channels > 2)
        {
            WAVEFORMATEXTENSIBLE *wave_format;

            if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
                return false;
            memset(wave_format, 0, sizeof(*wave_format));

            mt->subtype = is_float ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM;
            mt->bFixedSizeSamples = TRUE;
            mt->pbFormat = (BYTE *)wave_format;
            mt->cbFormat = sizeof(*wave_format);
            wave_format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wave_format->Format.nChannels = format->u.audio.channels;
            wave_format->Format.nSamplesPerSec = format->u.audio.rate;
            wave_format->Format.nAvgBytesPerSec = format->u.audio.rate * format->u.audio.channels * depth / 8;
            wave_format->Format.nBlockAlign = format->u.audio.channels * depth / 8;
            wave_format->Format.wBitsPerSample = depth;
            wave_format->Format.cbSize = sizeof(*wave_format) - sizeof(WAVEFORMATEX);
            wave_format->Samples.wValidBitsPerSample = depth;
            wave_format->dwChannelMask = channel_mask_from_count(format->u.audio.channels);
            wave_format->SubFormat = is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
            mt->lSampleSize = wave_format->Format.nBlockAlign;
        }
        else
        {
            WAVEFORMATEX *wave_format;

            if (!(wave_format = CoTaskMemAlloc(sizeof(*wave_format))))
                return false;
            memset(wave_format, 0, sizeof(*wave_format));

            mt->subtype = MEDIASUBTYPE_PCM;
            mt->bFixedSizeSamples = TRUE;
            mt->pbFormat = (BYTE *)wave_format;
            mt->cbFormat = sizeof(*wave_format);
            wave_format->wFormatTag = WAVE_FORMAT_PCM;
            wave_format->nChannels = format->u.audio.channels;
            wave_format->nSamplesPerSec = format->u.audio.rate;
            wave_format->nAvgBytesPerSec = format->u.audio.rate * format->u.audio.channels * depth / 8;
            wave_format->nBlockAlign = format->u.audio.channels * depth / 8;
            wave_format->wBitsPerSample = depth;
            wave_format->cbSize = 0;
            mt->lSampleSize = wave_format->nBlockAlign;
        }
        return true;
    }
    }

    assert(0);
    return false;
}

#define ALIGN(n, alignment) (((n) + (alignment) - 1) & ~((alignment) - 1))

static unsigned int get_image_size(const struct wg_format *format)
{
    unsigned int width = format->u.video.width, height = format->u.video.height;

    switch (format->u.video.format)
    {
        case WG_VIDEO_FORMAT_BGRA:
        case WG_VIDEO_FORMAT_BGRx:
        case WG_VIDEO_FORMAT_AYUV:
            return width * height * 4;

        case WG_VIDEO_FORMAT_BGR:
            return ALIGN(width * 3, 4) * height;

        case WG_VIDEO_FORMAT_RGB15:
        case WG_VIDEO_FORMAT_RGB16:
        case WG_VIDEO_FORMAT_UYVY:
        case WG_VIDEO_FORMAT_YUY2:
        case WG_VIDEO_FORMAT_YVYU:
            return ALIGN(width * 2, 4) * height;

        case WG_VIDEO_FORMAT_I420:
        case WG_VIDEO_FORMAT_YV12:
            return ALIGN(width, 4) * ALIGN(height, 2) /* Y plane */
                    + 2 * ALIGN((width + 1) / 2, 4) * ((height + 1) / 2); /* U and V planes */

        case WG_VIDEO_FORMAT_NV12:
            return ALIGN(width, 4) * ALIGN(height, 2) /* Y plane */
                    + ALIGN(width, 4) * ((height + 1) / 2); /* U/V plane */

        case WG_VIDEO_FORMAT_CINEPAK:
            /* Both ffmpeg's encoder and a Cinepak file seen in the wild report
             * 24 bpp. ffmpeg sets biSizeImage as below; others may be smaller,
             * but as long as every sample fits into our allocator, we're fine. */
            return width * height * 3;

        case WG_VIDEO_FORMAT_UNKNOWN:
            break;
    }

    assert(0);
    return 0;
}

static bool amt_from_wg_format_video(AM_MEDIA_TYPE *mt, const struct wg_format *format)
{
    static const struct
    {
        const GUID *subtype;
        DWORD compression;
        WORD depth;
    }
    format_table[] =
    {
        {0},
        {&MEDIASUBTYPE_ARGB32, BI_RGB,                      32},
        {&MEDIASUBTYPE_RGB32,  BI_RGB,                      32},
        {&MEDIASUBTYPE_RGB24,  BI_RGB,                      24},
        {&MEDIASUBTYPE_RGB555, BI_RGB,                      16},
        {&MEDIASUBTYPE_RGB565, BI_BITFIELDS,                16},
        {&MEDIASUBTYPE_AYUV,   mmioFOURCC('A','Y','U','V'), 32},
        {&MEDIASUBTYPE_I420,   mmioFOURCC('I','4','2','0'), 12},
        {&MEDIASUBTYPE_NV12,   mmioFOURCC('N','V','1','2'), 12},
        {&MEDIASUBTYPE_UYVY,   mmioFOURCC('U','Y','V','Y'), 16},
        {&MEDIASUBTYPE_YUY2,   mmioFOURCC('Y','U','Y','2'), 16},
        {&MEDIASUBTYPE_YV12,   mmioFOURCC('Y','V','1','2'), 12},
        {&MEDIASUBTYPE_YVYU,   mmioFOURCC('Y','V','Y','U'), 16},
        {&MEDIASUBTYPE_CVID,   mmioFOURCC('C','V','I','D'), 24},
    };

    VIDEOINFO *video_format;
    uint32_t frame_time;

    if (format->u.video.format == WG_VIDEO_FORMAT_UNKNOWN)
        return false;

    if (!(video_format = CoTaskMemAlloc(sizeof(*video_format))))
        return false;

    assert(format->u.video.format < ARRAY_SIZE(format_table));

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *format_table[format->u.video.format].subtype;
    mt->bTemporalCompression = TRUE;
    mt->lSampleSize = 1;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = (BYTE *)video_format;

    memset(video_format, 0, sizeof(*video_format));

    if ((frame_time = MulDiv(10000000, format->u.video.fps_d, format->u.video.fps_n)) != -1)
        video_format->AvgTimePerFrame = frame_time;
    video_format->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    video_format->bmiHeader.biWidth = format->u.video.width;
    video_format->bmiHeader.biHeight = format->u.video.height;
    video_format->bmiHeader.biPlanes = 1;
    video_format->bmiHeader.biBitCount = format_table[format->u.video.format].depth;
    video_format->bmiHeader.biCompression = format_table[format->u.video.format].compression;
    video_format->bmiHeader.biSizeImage = get_image_size(format);

    if (format->u.video.format == WG_VIDEO_FORMAT_RGB16)
    {
        mt->cbFormat = offsetof(VIDEOINFO, u.dwBitMasks[3]);
        video_format->u.dwBitMasks[iRED]   = 0xf800;
        video_format->u.dwBitMasks[iGREEN] = 0x07e0;
        video_format->u.dwBitMasks[iBLUE]  = 0x001f;
    }

    return true;
}

static bool amt_from_wg_format(AM_MEDIA_TYPE *mt, const struct wg_format *format)
{
    memset(mt, 0, sizeof(*mt));

    switch (format->major_type)
    {
    case WG_MAJOR_TYPE_UNKNOWN:
        return false;

    case WG_MAJOR_TYPE_AUDIO:
        return amt_from_wg_format_audio(mt, format);

    case WG_MAJOR_TYPE_VIDEO:
        return amt_from_wg_format_video(mt, format);
    }

    assert(0);
    return false;
}

static bool amt_to_wg_format_audio(const AM_MEDIA_TYPE *mt, struct wg_format *format)
{
    static const struct
    {
        const GUID *subtype;
        WORD depth;
        enum wg_audio_format format;
    }
    format_map[] =
    {
        {&MEDIASUBTYPE_PCM,          8, WG_AUDIO_FORMAT_U8},
        {&MEDIASUBTYPE_PCM,         16, WG_AUDIO_FORMAT_S16LE},
        {&MEDIASUBTYPE_PCM,         24, WG_AUDIO_FORMAT_S24LE},
        {&MEDIASUBTYPE_PCM,         32, WG_AUDIO_FORMAT_S32LE},
        {&MEDIASUBTYPE_IEEE_FLOAT,  32, WG_AUDIO_FORMAT_F32LE},
        {&MEDIASUBTYPE_IEEE_FLOAT,  64, WG_AUDIO_FORMAT_F64LE},
    };

    const WAVEFORMATEX *audio_format = (const WAVEFORMATEX *)mt->pbFormat;
    unsigned int i;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(WAVEFORMATEX) || !mt->pbFormat)
    {
        ERR("Unexpected format size %u.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WG_MAJOR_TYPE_AUDIO;
    format->u.audio.channels = audio_format->nChannels;
    format->u.audio.rate = audio_format->nSamplesPerSec;

    for (i = 0; i < ARRAY_SIZE(format_map); ++i)
    {
        if (IsEqualGUID(&mt->subtype, format_map[i].subtype)
                && audio_format->wBitsPerSample == format_map[i].depth)
        {
            format->u.audio.format = format_map[i].format;
            return true;
        }
    }

    FIXME("Unknown subtype %s, depth %u.\n", debugstr_guid(&mt->subtype), audio_format->wBitsPerSample);
    return false;
}

static bool amt_to_wg_format_audio_mpeg1(const AM_MEDIA_TYPE *mt, struct wg_format *format)
{
    const MPEG1WAVEFORMAT *audio_format = (const MPEG1WAVEFORMAT *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(*audio_format) || !mt->pbFormat)
    {
        ERR("Unexpected format size %u.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WG_MAJOR_TYPE_AUDIO;
    format->u.audio.channels = audio_format->wfx.nChannels;
    format->u.audio.rate = audio_format->wfx.nSamplesPerSec;
    if (audio_format->fwHeadLayer == 1)
        format->u.audio.format = WG_AUDIO_FORMAT_MPEG1_LAYER1;
    else if (audio_format->fwHeadLayer == 2)
        format->u.audio.format = WG_AUDIO_FORMAT_MPEG1_LAYER2;
    else if (audio_format->fwHeadLayer == 3)
        format->u.audio.format = WG_AUDIO_FORMAT_MPEG1_LAYER3;
    else
        return false;
    return true;
}

static bool amt_to_wg_format_audio_mpeg1_layer3(const AM_MEDIA_TYPE *mt, struct wg_format *format)
{
    const MPEGLAYER3WAVEFORMAT *audio_format = (const MPEGLAYER3WAVEFORMAT *)mt->pbFormat;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_WaveFormatEx))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(*audio_format) || !mt->pbFormat)
    {
        ERR("Unexpected format size %u.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WG_MAJOR_TYPE_AUDIO;
    format->u.audio.channels = audio_format->wfx.nChannels;
    format->u.audio.rate = audio_format->wfx.nSamplesPerSec;
    format->u.audio.format = WG_AUDIO_FORMAT_MPEG1_LAYER3;
    return true;
}

static bool amt_to_wg_format_video(const AM_MEDIA_TYPE *mt, struct wg_format *format)
{
    static const struct
    {
        const GUID *subtype;
        enum wg_video_format format;
    }
    format_map[] =
    {
        {&MEDIASUBTYPE_ARGB32,  WG_VIDEO_FORMAT_BGRA},
        {&MEDIASUBTYPE_RGB32,   WG_VIDEO_FORMAT_BGRx},
        {&MEDIASUBTYPE_RGB24,   WG_VIDEO_FORMAT_BGR},
        {&MEDIASUBTYPE_RGB555,  WG_VIDEO_FORMAT_RGB15},
        {&MEDIASUBTYPE_RGB565,  WG_VIDEO_FORMAT_RGB16},
        {&MEDIASUBTYPE_AYUV,    WG_VIDEO_FORMAT_AYUV},
        {&MEDIASUBTYPE_I420,    WG_VIDEO_FORMAT_I420},
        {&MEDIASUBTYPE_NV12,    WG_VIDEO_FORMAT_NV12},
        {&MEDIASUBTYPE_UYVY,    WG_VIDEO_FORMAT_UYVY},
        {&MEDIASUBTYPE_YUY2,    WG_VIDEO_FORMAT_YUY2},
        {&MEDIASUBTYPE_YV12,    WG_VIDEO_FORMAT_YV12},
        {&MEDIASUBTYPE_YVYU,    WG_VIDEO_FORMAT_YVYU},
        {&MEDIASUBTYPE_CVID,    WG_VIDEO_FORMAT_CINEPAK},
    };

    const VIDEOINFOHEADER *video_format = (const VIDEOINFOHEADER *)mt->pbFormat;
    unsigned int i;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
    {
        FIXME("Unknown format type %s.\n", debugstr_guid(&mt->formattype));
        return false;
    }
    if (mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat)
    {
        ERR("Unexpected format size %u.\n", mt->cbFormat);
        return false;
    }

    format->major_type = WG_MAJOR_TYPE_VIDEO;
    format->u.video.width = video_format->bmiHeader.biWidth;
    format->u.video.height = video_format->bmiHeader.biHeight;
    format->u.video.fps_n = 10000000;
    format->u.video.fps_d = video_format->AvgTimePerFrame;

    for (i = 0; i < ARRAY_SIZE(format_map); ++i)
    {
        if (IsEqualGUID(&mt->subtype, format_map[i].subtype))
        {
            format->u.video.format = format_map[i].format;
            return true;
        }
    }

    FIXME("Unknown subtype %s.\n", debugstr_guid(&mt->subtype));
    return false;
}

static bool amt_to_wg_format(const AM_MEDIA_TYPE *mt, struct wg_format *format)
{
    memset(format, 0, sizeof(*format));

    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Video))
        return amt_to_wg_format_video(mt, format);
    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Audio))
    {
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1AudioPayload))
            return amt_to_wg_format_audio_mpeg1(mt, format);
        if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MP3))
            return amt_to_wg_format_audio_mpeg1_layer3(mt, format);
        return amt_to_wg_format_audio(mt, format);
    }

    FIXME("Unknown major type %s.\n", debugstr_guid(&mt->majortype));
    return false;
}

/* Fill and send a single IMediaSample. */
static HRESULT send_sample(struct parser_source *pin, IMediaSample *sample,
        GstBuffer *buf, GstMapInfo *info, gsize offset, gsize size, DWORD bytes_per_second)
{
    HRESULT hr;
    BYTE *ptr = NULL;

    hr = IMediaSample_SetActualDataLength(sample, size);
    if(FAILED(hr)){
        WARN("SetActualDataLength failed: %08x\n", hr);
        return hr;
    }

    IMediaSample_GetPointer(sample, &ptr);

    memcpy(ptr, &info->data[offset], size);

    if (GST_BUFFER_PTS_IS_VALID(buf)) {
        REFERENCE_TIME rtStart, ptsStart = buf->pts;

        if (offset > 0)
            ptsStart = buf->pts + gst_util_uint64_scale(offset, GST_SECOND, bytes_per_second);
        rtStart = ((ptsStart / 100) - pin->seek.llCurrent) * pin->seek.dRate;

        if (GST_BUFFER_DURATION_IS_VALID(buf)) {
            REFERENCE_TIME rtStop, tStart, tStop, ptsStop = buf->pts + buf->duration;
            if (offset + size < info->size)
                ptsStop = buf->pts + gst_util_uint64_scale(offset + size, GST_SECOND, bytes_per_second);
            tStart = ptsStart / 100;
            tStop = ptsStop / 100;
            rtStop = ((ptsStop / 100) - pin->seek.llCurrent) * pin->seek.dRate;
            TRACE("Current time on %p: %i to %i ms\n", pin, (int)(rtStart / 10000), (int)(rtStop / 10000));
            IMediaSample_SetTime(sample, &rtStart, rtStop >= 0 ? &rtStop : NULL);
            IMediaSample_SetMediaTime(sample, &tStart, &tStop);
        } else {
            IMediaSample_SetTime(sample, rtStart >= 0 ? &rtStart : NULL, NULL);
            IMediaSample_SetMediaTime(sample, NULL, NULL);
        }
    } else {
        IMediaSample_SetTime(sample, NULL, NULL);
        IMediaSample_SetMediaTime(sample, NULL, NULL);
    }

    IMediaSample_SetDiscontinuity(sample, !offset && GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DISCONT));
    IMediaSample_SetPreroll(sample, GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_LIVE));
    IMediaSample_SetSyncPoint(sample, !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT));

    if (!pin->pin.pin.peer)
        hr = VFW_E_NOT_CONNECTED;
    else
        hr = IMemInputPin_Receive(pin->pin.pMemInputPin, sample);

    TRACE("sending sample returned: %08x\n", hr);

    return hr;
}

/* Send a single GStreamer buffer (splitting it into multiple IMediaSamples if
 * necessary). */
static void send_buffer(struct parser_source *pin, GstBuffer *buf)
{
    HRESULT hr;
    IMediaSample *sample;
    GstMapInfo info;

    gst_buffer_map(buf, &info, GST_MAP_READ);

    if (IsEqualGUID(&pin->pin.pin.mt.formattype, &FORMAT_WaveFormatEx)
            && (IsEqualGUID(&pin->pin.pin.mt.subtype, &MEDIASUBTYPE_PCM)
            || IsEqualGUID(&pin->pin.pin.mt.subtype, &MEDIASUBTYPE_IEEE_FLOAT)))
    {
        WAVEFORMATEX *format = (WAVEFORMATEX *)pin->pin.pin.mt.pbFormat;
        gsize offset = 0;
        while (offset < info.size)
        {
            gsize advance;

            hr = BaseOutputPinImpl_GetDeliveryBuffer(&pin->pin, &sample, NULL, NULL, 0);

            if (FAILED(hr))
            {
                if (hr != VFW_E_NOT_CONNECTED)
                    ERR("Could not get a delivery buffer (%x), returning GST_FLOW_FLUSHING\n", hr);
                break;
            }

            advance = min(IMediaSample_GetSize(sample), info.size - offset);

            hr = send_sample(pin, sample, buf, &info, offset, advance, format->nAvgBytesPerSec);

            IMediaSample_Release(sample);

            if (FAILED(hr))
                break;

            offset += advance;
        }
    }
    else
    {
        hr = BaseOutputPinImpl_GetDeliveryBuffer(&pin->pin, &sample, NULL, NULL, 0);

        if (FAILED(hr))
        {
            if (hr != VFW_E_NOT_CONNECTED)
                ERR("Could not get a delivery buffer (%x), returning GST_FLOW_FLUSHING\n", hr);
        }
        else
        {
            hr = send_sample(pin, sample, buf, &info, 0, info.size, 0);

            IMediaSample_Release(sample);
        }
    }

    gst_buffer_unmap(buf, &info);

    gst_buffer_unref(buf);
}

static DWORD CALLBACK stream_thread(void *arg)
{
    struct parser_source *pin = arg;
    struct parser *filter = impl_from_strmbase_filter(pin->pin.pin.filter);

    TRACE("Starting streaming thread for pin %p.\n", pin);

    while (filter->streaming)
    {
        struct wg_parser_event event;

        EnterCriticalSection(&pin->flushing_cs);

        if (!unix_funcs->wg_parser_stream_get_event(pin->wg_stream, &event))
        {
            LeaveCriticalSection(&pin->flushing_cs);
            continue;
        }

        TRACE("Got event of type %#x.\n", event.type);

        switch (event.type)
        {
            case WG_PARSER_EVENT_BUFFER:
                send_buffer(pin, event.u.buffer);
                break;

            case WG_PARSER_EVENT_EOS:
                IPin_EndOfStream(pin->pin.pin.peer);
                break;

            case WG_PARSER_EVENT_SEGMENT:
                IPin_NewSegment(pin->pin.pin.peer, event.u.segment.position,
                        event.u.segment.stop, event.u.segment.rate);
                break;

            case WG_PARSER_EVENT_NONE:
                assert(0);
        }

        LeaveCriticalSection(&pin->flushing_cs);
    }

    TRACE("Streaming stopped; exiting.\n");
    return 0;
}

static GstFlowReturn read_buffer(struct parser *This, guint64 ofs, guint len, GstBuffer *buffer)
{
    HRESULT hr;
    GstMapInfo info;

    TRACE("filter %p, offset %s, length %u, buffer %p.\n", This, wine_dbgstr_longlong(ofs), len, buffer);

    if (ofs == GST_BUFFER_OFFSET_NONE)
        ofs = This->next_pull_offset;
    if (ofs >= This->file_size)
    {
        WARN("Reading past eof: %s, %u\n", wine_dbgstr_longlong(ofs), len);
        return GST_FLOW_EOS;
    }
    if (len + ofs > This->file_size)
        len = This->file_size - ofs;
    This->next_pull_offset = ofs + len;

    gst_buffer_map(buffer, &info, GST_MAP_WRITE);
    hr = IAsyncReader_SyncRead(This->reader, ofs, len, info.data);
    gst_buffer_unmap(buffer, &info);
    if (FAILED(hr))
    {
        ERR("Failed to read data, hr %#x.\n", hr);
        return GST_FLOW_ERROR;
    }

    return GST_FLOW_OK;
}

static DWORD CALLBACK read_thread(void *arg)
{
    struct parser *filter = arg;
    struct wg_parser *parser = filter->wg_parser;

    TRACE("Starting read thread for filter %p.\n", filter);

    pthread_mutex_lock(&parser->mutex);

    while (filter->sink_connected)
    {
        while (parser->sink_connected && !parser->read_request.buffer)
            pthread_cond_wait(&parser->read_cond, &parser->mutex);

        if (!parser->sink_connected)
            break;

        parser->read_request.done = true;
        parser->read_request.ret = read_buffer(filter, parser->read_request.offset,
                parser->read_request.size, parser->read_request.buffer);
        parser->read_request.buffer = NULL;
        pthread_cond_signal(&parser->read_done_cond);
    }

    pthread_mutex_unlock(&parser->mutex);

    TRACE("Streaming stopped; exiting.\n");
    return 0;
}

static inline struct parser_source *impl_from_IMediaSeeking(IMediaSeeking *iface)
{
    return CONTAINING_RECORD(iface, struct parser_source, seek.IMediaSeeking_iface);
}

static struct strmbase_pin *parser_get_pin(struct strmbase_filter *base, unsigned int index)
{
    struct parser *filter = impl_from_strmbase_filter(base);

    if (filter->enum_sink_first)
    {
        if (!index)
            return &filter->sink.pin;
        else if (index <= filter->source_count)
            return &filter->sources[index - 1]->pin.pin;
    }
    else
    {
        if (index < filter->source_count)
            return &filter->sources[index]->pin.pin;
        else if (index == filter->source_count)
            return &filter->sink.pin;
    }
    return NULL;
}

static void parser_destroy(struct strmbase_filter *iface)
{
    struct parser *filter = impl_from_strmbase_filter(iface);
    HRESULT hr;

    /* Don't need to clean up output pins, disconnecting input pin will do that */
    if (filter->sink.pin.peer)
    {
        hr = IPin_Disconnect(filter->sink.pin.peer);
        assert(hr == S_OK);
        hr = IPin_Disconnect(&filter->sink.pin.IPin_iface);
        assert(hr == S_OK);
    }

    if (filter->reader)
        IAsyncReader_Release(filter->reader);
    filter->reader = NULL;

    unix_funcs->wg_parser_destroy(filter->wg_parser);

    strmbase_sink_cleanup(&filter->sink);
    strmbase_filter_cleanup(&filter->filter);
    heap_free(filter);
}

static HRESULT parser_init_stream(struct strmbase_filter *iface)
{
    struct parser *filter = impl_from_strmbase_filter(iface);
    struct wg_parser *parser = filter->wg_parser;
    DWORD stop_flags = AM_SEEKING_NoPositioning;
    const SourceSeeking *seeking;
    unsigned int i;

    if (!parser->container)
        return S_OK;

    filter->streaming = true;
    unix_funcs->wg_parser_end_flush(filter->wg_parser);

    /* DirectShow retains the old seek positions, but resets to them every time
     * it transitions from stopped -> paused. */

    seeking = &filter->sources[0]->seek;
    if (seeking->llStop && seeking->llStop != seeking->llDuration)
        stop_flags = AM_SEEKING_AbsolutePositioning;
    unix_funcs->wg_parser_stream_seek(filter->sources[0]->wg_stream, seeking->dRate,
            seeking->llCurrent, seeking->llStop, AM_SEEKING_AbsolutePositioning, stop_flags);

    for (i = 0; i < filter->source_count; ++i)
    {
        HRESULT hr;

        if (!filter->sources[i]->pin.pin.peer)
            continue;

        if (FAILED(hr = IMemAllocator_Commit(filter->sources[i]->pin.pAllocator)))
            ERR("Failed to commit allocator, hr %#x.\n", hr);

        filter->sources[i]->thread = CreateThread(NULL, 0, stream_thread, filter->sources[i], 0, NULL);
    }

    return S_OK;
}

static HRESULT parser_cleanup_stream(struct strmbase_filter *iface)
{
    struct parser *filter = impl_from_strmbase_filter(iface);
    struct wg_parser *parser = filter->wg_parser;
    unsigned int i;

    if (!parser->container)
        return S_OK;

    filter->streaming = false;
    unix_funcs->wg_parser_begin_flush(filter->wg_parser);

    for (i = 0; i < filter->source_count; ++i)
    {
        struct parser_source *pin = filter->sources[i];

        if (!pin->pin.pin.peer)
            continue;

        IMemAllocator_Decommit(pin->pin.pAllocator);

        WaitForSingleObject(pin->thread, INFINITE);
        CloseHandle(pin->thread);
        pin->thread = NULL;
    }

    return S_OK;
}

static const struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = parser_get_pin,
    .filter_destroy = parser_destroy,
    .filter_init_stream = parser_init_stream,
    .filter_cleanup_stream = parser_cleanup_stream,
};

static inline struct parser *impl_from_strmbase_sink(struct strmbase_sink *iface)
{
    return CONTAINING_RECORD(iface, struct parser, sink);
}

static HRESULT sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_OK;
    return S_FALSE;
}

static HRESULT parser_sink_connect(struct strmbase_sink *iface, IPin *peer, const AM_MEDIA_TYPE *pmt)
{
    struct parser *filter = impl_from_strmbase_sink(iface);
    HRESULT hr = S_OK;
    LONGLONG unused;
    unsigned int i;

    mark_wine_thread();

    filter->reader = NULL;
    if (FAILED(hr = IPin_QueryInterface(peer, &IID_IAsyncReader, (void **)&filter->reader)))
        return hr;

    IAsyncReader_Length(filter->reader, &filter->file_size, &unused);

    filter->sink_connected = true;
    filter->read_thread = CreateThread(NULL, 0, read_thread, filter, 0, NULL);
    filter->next_pull_offset = 0;

    if (FAILED(hr = unix_funcs->wg_parser_connect(filter->wg_parser, filter->file_size)))
        goto err;

    if (!filter->init_gst(filter))
        goto err;

    for (i = 0; i < filter->source_count; ++i)
    {
        struct parser_source *pin = filter->sources[i];

        pin->seek.llDuration = pin->seek.llStop = pin->wg_stream->duration;
        pin->seek.llCurrent = 0;
    }

    return S_OK;
err:
    GST_RemoveOutputPins(filter);
    IAsyncReader_Release(filter->reader);
    filter->reader = NULL;
    return hr;
}

static void parser_sink_disconnect(struct strmbase_sink *iface)
{
    struct parser *filter = impl_from_strmbase_sink(iface);

    mark_wine_thread();

    GST_RemoveOutputPins(filter);

    IAsyncReader_Release(filter->reader);
    filter->reader = NULL;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_accept = sink_query_accept,
    .sink_connect = parser_sink_connect,
    .sink_disconnect = parser_sink_disconnect,
};

static BOOL decodebin_parser_filter_init_gst(struct parser *filter)
{
    static const WCHAR formatW[] = {'S','t','r','e','a','m',' ','%','0','2','u',0};
    struct wg_parser *parser = filter->wg_parser;
    unsigned int i, stream_count;
    WCHAR source_name[20];

    stream_count = unix_funcs->wg_parser_get_stream_count(parser);
    for (i = 0; i < stream_count; ++i)
    {
        sprintfW(source_name, formatW, i);
        if (!create_pin(filter, unix_funcs->wg_parser_get_stream(parser, i), source_name))
            return FALSE;
    }

    return TRUE;
}

static HRESULT decodebin_parser_source_query_accept(struct parser_source *pin, const AM_MEDIA_TYPE *mt)
{
    struct wg_format format;

    /* At least make sure we can convert it to wg_format. */
    return amt_to_wg_format(mt, &format) ? S_OK : S_FALSE;
}

static HRESULT decodebin_parser_source_get_media_type(struct parser_source *pin,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct wg_format format;

    static const enum wg_video_format video_formats[] =
    {
        /* Try to prefer YUV formats over RGB ones. Most decoders output in the
         * YUV color space, and it's generally much less expensive for
         * videoconvert to do YUV -> YUV transformations. */
        WG_VIDEO_FORMAT_AYUV,
        WG_VIDEO_FORMAT_I420,
        WG_VIDEO_FORMAT_YV12,
        WG_VIDEO_FORMAT_YUY2,
        WG_VIDEO_FORMAT_UYVY,
        WG_VIDEO_FORMAT_YVYU,
        WG_VIDEO_FORMAT_NV12,
        WG_VIDEO_FORMAT_BGRA,
        WG_VIDEO_FORMAT_BGRx,
        WG_VIDEO_FORMAT_BGR,
        WG_VIDEO_FORMAT_RGB16,
        WG_VIDEO_FORMAT_RGB15,
    };

    unix_funcs->wg_parser_stream_get_preferred_format(pin->wg_stream, &format);

    memset(mt, 0, sizeof(AM_MEDIA_TYPE));

    if (amt_from_wg_format(mt, &format))
    {
        if (!index--)
            return S_OK;
        FreeMediaType(mt);
    }

    if (format.major_type == WG_MAJOR_TYPE_VIDEO && index < ARRAY_SIZE(video_formats))
    {
        format.u.video.format = video_formats[index];
        if (!amt_from_wg_format(mt, &format))
            return E_OUTOFMEMORY;
        return S_OK;
    }
    else if (format.major_type == WG_MAJOR_TYPE_AUDIO && !index)
    {
        format.u.audio.format = WG_AUDIO_FORMAT_S16LE;
        if (!amt_from_wg_format(mt, &format))
            return E_OUTOFMEMORY;
        return S_OK;
    }

    return VFW_S_NO_MORE_ITEMS;
}

static BOOL parser_init_gstreamer(void)
{
    if (!init_gstreamer())
        return FALSE;
    return TRUE;
}

HRESULT decodebin_parser_create(IUnknown *outer, IUnknown **out)
{
    struct parser *object;

    if (!parser_init_gstreamer())
        return E_FAIL;

    mark_wine_thread();

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (!(object->wg_parser = unix_funcs->wg_decodebin_parser_create()))
    {
        heap_free(object);
        return E_OUTOFMEMORY;
    }

    strmbase_filter_init(&object->filter, outer, &CLSID_decodebin_parser, &filter_ops);
    strmbase_sink_init(&object->sink, &object->filter, wcsInputPinName, &sink_ops, NULL);

    object->init_gst = decodebin_parser_filter_init_gst;
    object->source_query_accept = decodebin_parser_source_query_accept;
    object->source_get_media_type = decodebin_parser_source_get_media_type;

    TRACE("Created GStreamer demuxer %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return S_OK;
}

static struct parser *impl_from_IAMStreamSelect(IAMStreamSelect *iface)
{
    return CONTAINING_RECORD(iface, struct parser, IAMStreamSelect_iface);
}

static HRESULT WINAPI stream_select_QueryInterface(IAMStreamSelect *iface, REFIID iid, void **out)
{
    struct parser *filter = impl_from_IAMStreamSelect(iface);
    return IUnknown_QueryInterface(filter->filter.outer_unk, iid, out);
}

static ULONG WINAPI stream_select_AddRef(IAMStreamSelect *iface)
{
    struct parser *filter = impl_from_IAMStreamSelect(iface);
    return IUnknown_AddRef(filter->filter.outer_unk);
}

static ULONG WINAPI stream_select_Release(IAMStreamSelect *iface)
{
    struct parser *filter = impl_from_IAMStreamSelect(iface);
    return IUnknown_Release(filter->filter.outer_unk);
}

static HRESULT WINAPI stream_select_Count(IAMStreamSelect *iface, DWORD *count)
{
    FIXME("iface %p, count %p, stub!\n", iface, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_select_Info(IAMStreamSelect *iface, LONG index,
        AM_MEDIA_TYPE **mt, DWORD *flags, LCID *lcid, DWORD *group, WCHAR **name,
        IUnknown **object, IUnknown **unknown)
{
    FIXME("iface %p, index %d, mt %p, flags %p, lcid %p, group %p, name %p, object %p, unknown %p, stub!\n",
            iface, index, mt, flags, lcid, group, name, object, unknown);
    return E_NOTIMPL;
}

static HRESULT WINAPI stream_select_Enable(IAMStreamSelect *iface, LONG index, DWORD flags)
{
    FIXME("iface %p, index %d, flags %#x, stub!\n", iface, index, flags);
    return E_NOTIMPL;
}

static const IAMStreamSelectVtbl stream_select_vtbl =
{
    stream_select_QueryInterface,
    stream_select_AddRef,
    stream_select_Release,
    stream_select_Count,
    stream_select_Info,
    stream_select_Enable,
};

static HRESULT WINAPI GST_ChangeCurrent(IMediaSeeking *iface)
{
    struct parser_source *This = impl_from_IMediaSeeking(iface);
    TRACE("(%p)\n", This);
    return S_OK;
}

static HRESULT WINAPI GST_ChangeStop(IMediaSeeking *iface)
{
    struct parser_source *This = impl_from_IMediaSeeking(iface);
    TRACE("(%p)\n", This);
    return S_OK;
}

static HRESULT WINAPI GST_ChangeRate(IMediaSeeking *iface)
{
    struct parser_source *pin = impl_from_IMediaSeeking(iface);

    mark_wine_thread();
    unix_funcs->wg_parser_stream_seek(pin->wg_stream, pin->seek.dRate, 0, 0,
            AM_SEEKING_NoPositioning, AM_SEEKING_NoPositioning);
    return S_OK;
}

static HRESULT WINAPI GST_Seeking_QueryInterface(IMediaSeeking *iface, REFIID riid, void **ppv)
{
    struct parser_source *This = impl_from_IMediaSeeking(iface);
    return IPin_QueryInterface(&This->pin.pin.IPin_iface, riid, ppv);
}

static ULONG WINAPI GST_Seeking_AddRef(IMediaSeeking *iface)
{
    struct parser_source *This = impl_from_IMediaSeeking(iface);
    return IPin_AddRef(&This->pin.pin.IPin_iface);
}

static ULONG WINAPI GST_Seeking_Release(IMediaSeeking *iface)
{
    struct parser_source *This = impl_from_IMediaSeeking(iface);
    return IPin_Release(&This->pin.pin.IPin_iface);
}

static HRESULT WINAPI GST_Seeking_SetPositions(IMediaSeeking *iface,
        LONGLONG *current, DWORD current_flags, LONGLONG *stop, DWORD stop_flags)
{
    struct parser_source *pin = impl_from_IMediaSeeking(iface);
    struct parser *filter = impl_from_strmbase_filter(pin->pin.pin.filter);
    HRESULT hr = S_OK;
    int i;

    TRACE("pin %p, current %s, current_flags %#x, stop %s, stop_flags %#x.\n",
            pin, current ? debugstr_time(*current) : "<null>", current_flags,
            stop ? debugstr_time(*stop) : "<null>", stop_flags);

    mark_wine_thread();

    if (pin->pin.pin.filter->state == State_Stopped)
    {
        SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);
        return S_OK;
    }

    if (!(current_flags & AM_SEEKING_NoFlush))
    {
        unix_funcs->wg_parser_begin_flush(filter->wg_parser);

        for (i = 0; i < filter->source_count; ++i)
        {
            if (filter->sources[i]->pin.pin.peer)
                IPin_BeginFlush(filter->sources[i]->pin.pin.peer);
        }

        if (filter->reader)
            IAsyncReader_BeginFlush(filter->reader);
    }

    /* Acquire the flushing locks. This blocks the streaming threads, and
     * ensures the seek is serialized between flushes. */
    for (i = 0; i < filter->source_count; ++i)
    {
        if (filter->sources[i]->pin.pin.peer)
            EnterCriticalSection(&pin->flushing_cs);
    }

    SourceSeekingImpl_SetPositions(iface, current, current_flags, stop, stop_flags);

    if (!unix_funcs->wg_parser_stream_seek(pin->wg_stream, pin->seek.dRate,
            pin->seek.llCurrent, pin->seek.llStop, current_flags, stop_flags))
    {
        ERR("Failed to seek (current %s, stop %s).\n",
                debugstr_time(pin->seek.llCurrent), debugstr_time(pin->seek.llStop));
        hr = E_FAIL;
    }

    if (!(current_flags & AM_SEEKING_NoFlush))
    {
        unix_funcs->wg_parser_end_flush(filter->wg_parser);

        for (i = 0; i < filter->source_count; ++i)
        {
            if (filter->sources[i]->pin.pin.peer)
                IPin_EndFlush(filter->sources[i]->pin.pin.peer);
        }

        if (filter->reader)
            IAsyncReader_EndFlush(filter->reader);
    }

    /* Release the flushing locks. */
    for (i = filter->source_count - 1; i >= 0; --i)
    {
        if (filter->sources[i]->pin.pin.peer)
            LeaveCriticalSection(&pin->flushing_cs);
    }

    return hr;
}

static const IMediaSeekingVtbl GST_Seeking_Vtbl =
{
    GST_Seeking_QueryInterface,
    GST_Seeking_AddRef,
    GST_Seeking_Release,
    SourceSeekingImpl_GetCapabilities,
    SourceSeekingImpl_CheckCapabilities,
    SourceSeekingImpl_IsFormatSupported,
    SourceSeekingImpl_QueryPreferredFormat,
    SourceSeekingImpl_GetTimeFormat,
    SourceSeekingImpl_IsUsingTimeFormat,
    SourceSeekingImpl_SetTimeFormat,
    SourceSeekingImpl_GetDuration,
    SourceSeekingImpl_GetStopPosition,
    SourceSeekingImpl_GetCurrentPosition,
    SourceSeekingImpl_ConvertTimeFormat,
    GST_Seeking_SetPositions,
    SourceSeekingImpl_GetPositions,
    SourceSeekingImpl_GetAvailable,
    SourceSeekingImpl_SetRate,
    SourceSeekingImpl_GetRate,
    SourceSeekingImpl_GetPreroll
};

static inline struct parser_source *impl_from_IQualityControl( IQualityControl *iface )
{
    return CONTAINING_RECORD(iface, struct parser_source, IQualityControl_iface);
}

static HRESULT WINAPI GST_QualityControl_QueryInterface(IQualityControl *iface, REFIID riid, void **ppv)
{
    struct parser_source *pin = impl_from_IQualityControl(iface);
    return IPin_QueryInterface(&pin->pin.pin.IPin_iface, riid, ppv);
}

static ULONG WINAPI GST_QualityControl_AddRef(IQualityControl *iface)
{
    struct parser_source *pin = impl_from_IQualityControl(iface);
    return IPin_AddRef(&pin->pin.pin.IPin_iface);
}

static ULONG WINAPI GST_QualityControl_Release(IQualityControl *iface)
{
    struct parser_source *pin = impl_from_IQualityControl(iface);
    return IPin_Release(&pin->pin.pin.IPin_iface);
}

static HRESULT WINAPI GST_QualityControl_Notify(IQualityControl *iface, IBaseFilter *sender, Quality q)
{
    struct parser_source *pin = impl_from_IQualityControl(iface);
    uint64_t timestamp;
    int64_t diff;

    TRACE("pin %p, sender %p, type %s, proportion %u, late %s, timestamp %s.\n",
            pin, sender, q.Type == Famine ? "Famine" : "Flood", q.Proportion,
            debugstr_time(q.Late), debugstr_time(q.TimeStamp));

    mark_wine_thread();

    /* DirectShow filters sometimes pass negative timestamps (Audiosurf uses the
     * current time instead of the time of the last buffer). GstClockTime is
     * unsigned, so clamp it to 0. */
    timestamp = max(q.TimeStamp, 0);

    /* The documentation specifies that timestamp + diff must be nonnegative. */
    diff = q.Late;
    if (diff < 0 && timestamp < (uint64_t)-diff)
        diff = -timestamp;

    /* DirectShow "Proportion" describes what percentage of buffers the upstream
     * filter should keep (i.e. dropping the rest). If frames are late, the
     * proportion will be less than 1. For example, a proportion of 500 means
     * that the element should drop half of its frames, essentially because
     * frames are taking twice as long as they should to arrive.
     *
     * GStreamer "proportion" is the inverse of this; it describes how much
     * faster the upstream element should produce frames. I.e. if frames are
     * taking twice as long as they should to arrive, we want the frames to be
     * decoded twice as fast, and so we pass 2.0 to GStreamer. */

    if (!q.Proportion)
    {
        WARN("Ignoring quality message with zero proportion.\n");
        return S_OK;
    }

    /* GST_QOS_TYPE_OVERFLOW is also used for buffers that arrive on time, but
     * DirectShow filters might use Famine, so check that there actually is an
     * underrun. */
    unix_funcs->wg_parser_stream_notify_qos(pin->wg_stream, q.Type == Famine && q.Proportion < 1000,
            1000.0 / q.Proportion, diff, timestamp);

    return S_OK;
}

static HRESULT WINAPI GST_QualityControl_SetSink(IQualityControl *iface, IQualityControl *tonotify)
{
    struct parser_source *pin = impl_from_IQualityControl(iface);
    TRACE("(%p)->(%p)\n", pin, pin);
    /* Do nothing */
    return S_OK;
}

static const IQualityControlVtbl GSTOutPin_QualityControl_Vtbl = {
    GST_QualityControl_QueryInterface,
    GST_QualityControl_AddRef,
    GST_QualityControl_Release,
    GST_QualityControl_Notify,
    GST_QualityControl_SetSink
};

static inline struct parser_source *impl_source_from_IPin(IPin *iface)
{
    return CONTAINING_RECORD(iface, struct parser_source, pin.pin.IPin_iface);
}

static HRESULT source_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct parser_source *pin = impl_source_from_IPin(&iface->IPin_iface);

    if (IsEqualGUID(iid, &IID_IMediaSeeking))
        *out = &pin->seek.IMediaSeeking_iface;
    else if (IsEqualGUID(iid, &IID_IQualityControl))
        *out = &pin->IQualityControl_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT source_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    struct parser_source *pin = impl_source_from_IPin(&iface->IPin_iface);
    struct parser *filter = impl_from_strmbase_filter(iface->filter);
    return filter->source_query_accept(pin, mt);
}

static HRESULT source_get_media_type(struct strmbase_pin *iface, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct parser_source *pin = impl_source_from_IPin(&iface->IPin_iface);
    struct parser *filter = impl_from_strmbase_filter(iface->filter);
    return filter->source_get_media_type(pin, index, mt);
}

static HRESULT WINAPI GSTOutPin_DecideBufferSize(struct strmbase_source *iface,
        IMemAllocator *allocator, ALLOCATOR_PROPERTIES *props)
{
    struct parser_source *pin = impl_source_from_IPin(&iface->pin.IPin_iface);
    unsigned int buffer_size = 16384;
    ALLOCATOR_PROPERTIES ret_props;
    struct wg_format format;
    bool ret;

    if (IsEqualGUID(&pin->pin.pin.mt.formattype, &FORMAT_VideoInfo))
    {
        VIDEOINFOHEADER *format = (VIDEOINFOHEADER *)pin->pin.pin.mt.pbFormat;
        buffer_size = format->bmiHeader.biSizeImage;
    }
    else if (IsEqualGUID(&pin->pin.pin.mt.formattype, &FORMAT_WaveFormatEx)
            && (IsEqualGUID(&pin->pin.pin.mt.subtype, &MEDIASUBTYPE_PCM)
            || IsEqualGUID(&pin->pin.pin.mt.subtype, &MEDIASUBTYPE_IEEE_FLOAT)))
    {
        WAVEFORMATEX *format = (WAVEFORMATEX *)pin->pin.pin.mt.pbFormat;
        buffer_size = format->nAvgBytesPerSec;
    }

    ret = amt_to_wg_format(&pin->pin.pin.mt, &format);
    assert(ret);
    unix_funcs->wg_parser_stream_enable(pin->wg_stream, &format);

    /* We do need to drop any buffers that might have been sent with the old
     * caps, but this will be handled in parser_init_stream(). */

    props->cBuffers = max(props->cBuffers, 1);
    props->cbBuffer = max(props->cbBuffer, buffer_size);
    props->cbAlign = max(props->cbAlign, 1);
    return IMemAllocator_SetProperties(allocator, props, &ret_props);
}

static void source_disconnect(struct strmbase_source *iface)
{
    struct parser_source *pin = impl_source_from_IPin(&iface->pin.IPin_iface);

    unix_funcs->wg_parser_stream_disable(pin->wg_stream);
}

static void free_source_pin(struct parser_source *pin)
{
    if (pin->pin.pin.peer)
    {
        if (SUCCEEDED(IMemAllocator_Decommit(pin->pin.pAllocator)))
            IPin_Disconnect(pin->pin.pin.peer);
        IPin_Disconnect(&pin->pin.pin.IPin_iface);
    }

    pin->flushing_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&pin->flushing_cs);

    strmbase_seeking_cleanup(&pin->seek);
    strmbase_source_cleanup(&pin->pin);
    heap_free(pin);
}

static const struct strmbase_source_ops source_ops =
{
    .base.pin_query_interface = source_query_interface,
    .base.pin_query_accept = source_query_accept,
    .base.pin_get_media_type = source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = BaseOutputPinImpl_DecideAllocator,
    .pfnDecideBufferSize = GSTOutPin_DecideBufferSize,
    .source_disconnect = source_disconnect,
};

static struct parser_source *create_pin(struct parser *filter,
        struct wg_parser_stream *stream, const WCHAR *name)
{
    struct parser_source *pin, **new_array;

    if (!(new_array = heap_realloc(filter->sources, (filter->source_count + 1) * sizeof(*filter->sources))))
        return NULL;
    filter->sources = new_array;

    if (!(pin = heap_alloc_zero(sizeof(*pin))))
        return NULL;

    pin->wg_stream = stream;
    strmbase_source_init(&pin->pin, &filter->filter, name, &source_ops);
    pin->IQualityControl_iface.lpVtbl = &GSTOutPin_QualityControl_Vtbl;
    strmbase_seeking_init(&pin->seek, &GST_Seeking_Vtbl, GST_ChangeStop,
            GST_ChangeCurrent, GST_ChangeRate);
    BaseFilterImpl_IncrementPinVersion(&filter->filter);

    InitializeCriticalSection(&pin->flushing_cs);
    pin->flushing_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": pin.flushing_cs");

    filter->sources[filter->source_count++] = pin;
    return pin;
}

static HRESULT GST_RemoveOutputPins(struct parser *This)
{
    unsigned int i;

    TRACE("(%p)\n", This);
    mark_wine_thread();

    if (!This->sink_connected)
        return S_OK;

    unix_funcs->wg_parser_disconnect(This->wg_parser);

    /* read_thread() needs to stay alive to service any read requests GStreamer
     * sends, so we can only shut it down after GStreamer stops. */
    This->sink_connected = false;
    WaitForSingleObject(This->read_thread, INFINITE);
    CloseHandle(This->read_thread);

    for (i = 0; i < This->source_count; ++i)
    {
        if (This->sources[i])
            free_source_pin(This->sources[i]);
    }

    This->source_count = 0;
    heap_free(This->sources);
    This->sources = NULL;

    BaseFilterImpl_IncrementPinVersion(&This->filter);
    return S_OK;
}

static BOOL compare_media_types(const AM_MEDIA_TYPE *a, const AM_MEDIA_TYPE *b)
{
    return IsEqualGUID(&a->majortype, &b->majortype)
            && IsEqualGUID(&a->subtype, &b->subtype)
            && IsEqualGUID(&a->formattype, &b->formattype)
            && a->cbFormat == b->cbFormat
            && !memcmp(a->pbFormat, b->pbFormat, a->cbFormat);
}

static HRESULT wave_parser_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_FALSE;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_WAVE))
        return S_OK;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_AU) || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_AIFF))
        FIXME("AU and AIFF files are not yet supported.\n");
    return S_FALSE;
}

static const struct strmbase_sink_ops wave_parser_sink_ops =
{
    .base.pin_query_accept = wave_parser_sink_query_accept,
    .sink_connect = parser_sink_connect,
    .sink_disconnect = parser_sink_disconnect,
};

static BOOL wave_parser_filter_init_gst(struct parser *filter)
{
    static const WCHAR source_name[] = {'o','u','t','p','u','t',0};
    struct wg_parser *parser = filter->wg_parser;

    if (!create_pin(filter, unix_funcs->wg_parser_get_stream(parser, 0), source_name))
        return FALSE;

    return TRUE;
}

static HRESULT wave_parser_source_query_accept(struct parser_source *pin, const AM_MEDIA_TYPE *mt)
{
    struct wg_format format;
    AM_MEDIA_TYPE pad_mt;
    HRESULT hr;

    unix_funcs->wg_parser_stream_get_preferred_format(pin->wg_stream, &format);
    if (!amt_from_wg_format(&pad_mt, &format))
        return E_OUTOFMEMORY;
    hr = compare_media_types(mt, &pad_mt) ? S_OK : S_FALSE;
    FreeMediaType(&pad_mt);
    return hr;
}

static HRESULT wave_parser_source_get_media_type(struct parser_source *pin,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct wg_format format;

    if (index > 0)
        return VFW_S_NO_MORE_ITEMS;
    unix_funcs->wg_parser_stream_get_preferred_format(pin->wg_stream, &format);
    if (!amt_from_wg_format(mt, &format))
        return E_OUTOFMEMORY;
    return S_OK;
}

HRESULT wave_parser_create(IUnknown *outer, IUnknown **out)
{
    static const WCHAR sink_name[] = {'i','n','p','u','t',' ','p','i','n',0};
    struct parser *object;

    if (!parser_init_gstreamer())
        return E_FAIL;

    mark_wine_thread();

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (!(object->wg_parser = unix_funcs->wg_wave_parser_create()))
    {
        heap_free(object);
        return E_OUTOFMEMORY;
    }

    strmbase_filter_init(&object->filter, outer, &CLSID_WAVEParser, &filter_ops);
    strmbase_sink_init(&object->sink, &object->filter, sink_name, &wave_parser_sink_ops, NULL);
    object->init_gst = wave_parser_filter_init_gst;
    object->source_query_accept = wave_parser_source_query_accept;
    object->source_get_media_type = wave_parser_source_get_media_type;

    TRACE("Created WAVE parser %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return S_OK;
}

static HRESULT avi_splitter_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream)
            && IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_Avi))
        return S_OK;
    return S_FALSE;
}

static const struct strmbase_sink_ops avi_splitter_sink_ops =
{
    .base.pin_query_accept = avi_splitter_sink_query_accept,
    .sink_connect = parser_sink_connect,
    .sink_disconnect = parser_sink_disconnect,
};

static BOOL avi_splitter_filter_init_gst(struct parser *filter)
{
    static const WCHAR formatW[] = {'S','t','r','e','a','m',' ','%','0','2','u',0};
    struct wg_parser *parser = filter->wg_parser;
    uint32_t i, stream_count;
    WCHAR source_name[20];

    stream_count = unix_funcs->wg_parser_get_stream_count(parser);
    for (i = 0; i < stream_count; ++i)
    {
        sprintfW(source_name, formatW, i);
        if (!create_pin(filter, unix_funcs->wg_parser_get_stream(parser, i), source_name))
            return FALSE;
    }

    return TRUE;
}

static HRESULT avi_splitter_source_query_accept(struct parser_source *pin, const AM_MEDIA_TYPE *mt)
{
    struct wg_format format;
    AM_MEDIA_TYPE pad_mt;
    HRESULT hr;

    unix_funcs->wg_parser_stream_get_preferred_format(pin->wg_stream, &format);
    if (!amt_from_wg_format(&pad_mt, &format))
        return E_OUTOFMEMORY;
    hr = compare_media_types(mt, &pad_mt) ? S_OK : S_FALSE;
    FreeMediaType(&pad_mt);
    return hr;
}

static HRESULT avi_splitter_source_get_media_type(struct parser_source *pin,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct wg_format format;

    if (index > 0)
        return VFW_S_NO_MORE_ITEMS;
    unix_funcs->wg_parser_stream_get_preferred_format(pin->wg_stream, &format);
    if (!amt_from_wg_format(mt, &format))
        return E_OUTOFMEMORY;
    return S_OK;
}

HRESULT avi_splitter_create(IUnknown *outer, IUnknown **out)
{
    static const WCHAR sink_name[] = {'i','n','p','u','t',' ','p','i','n',0};
    struct parser *object;

    if (!parser_init_gstreamer())
        return E_FAIL;

    mark_wine_thread();

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (!(object->wg_parser = unix_funcs->wg_avi_parser_create()))
    {
        heap_free(object);
        return E_OUTOFMEMORY;
    }

    strmbase_filter_init(&object->filter, outer, &CLSID_AviSplitter, &filter_ops);
    strmbase_sink_init(&object->sink, &object->filter, sink_name, &avi_splitter_sink_ops, NULL);
    object->init_gst = avi_splitter_filter_init_gst;
    object->source_query_accept = avi_splitter_source_query_accept;
    object->source_get_media_type = avi_splitter_source_get_media_type;

    TRACE("Created AVI splitter %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return S_OK;
}

static HRESULT mpeg_splitter_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Stream))
        return S_FALSE;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Audio))
        return S_OK;
    if (IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1Video)
            || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1System)
            || IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_MPEG1VideoCD))
        FIXME("Unsupported subtype %s.\n", wine_dbgstr_guid(&mt->subtype));
    return S_FALSE;
}

static const struct strmbase_sink_ops mpeg_splitter_sink_ops =
{
    .base.pin_query_accept = mpeg_splitter_sink_query_accept,
    .sink_connect = parser_sink_connect,
    .sink_disconnect = parser_sink_disconnect,
};

static BOOL mpeg_splitter_filter_init_gst(struct parser *filter)
{
    static const WCHAR source_name[] = {'A','u','d','i','o',0};
    struct wg_parser *parser = filter->wg_parser;

    if (!create_pin(filter, unix_funcs->wg_parser_get_stream(parser, 0), source_name))
        return FALSE;

    return TRUE;
}

static HRESULT mpeg_splitter_source_query_accept(struct parser_source *pin, const AM_MEDIA_TYPE *mt)
{
    struct wg_format format;
    AM_MEDIA_TYPE pad_mt;
    HRESULT hr;

    unix_funcs->wg_parser_stream_get_preferred_format(pin->wg_stream, &format);
    if (!amt_from_wg_format(&pad_mt, &format))
        return E_OUTOFMEMORY;
    hr = compare_media_types(mt, &pad_mt) ? S_OK : S_FALSE;
    FreeMediaType(&pad_mt);
    return hr;
}

static HRESULT mpeg_splitter_source_get_media_type(struct parser_source *pin,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct wg_format format;

    if (index > 0)
        return VFW_S_NO_MORE_ITEMS;
    unix_funcs->wg_parser_stream_get_preferred_format(pin->wg_stream, &format);
    if (!amt_from_wg_format(mt, &format))
        return E_OUTOFMEMORY;
    return S_OK;
}

static HRESULT mpeg_splitter_query_interface(struct strmbase_filter *iface, REFIID iid, void **out)
{
    struct parser *filter = impl_from_strmbase_filter(iface);

    if (IsEqualGUID(iid, &IID_IAMStreamSelect))
    {
        *out = &filter->IAMStreamSelect_iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static const struct strmbase_filter_ops mpeg_splitter_ops =
{
    .filter_query_interface = mpeg_splitter_query_interface,
    .filter_get_pin = parser_get_pin,
    .filter_destroy = parser_destroy,
    .filter_init_stream = parser_init_stream,
    .filter_cleanup_stream = parser_cleanup_stream,
};

HRESULT mpeg_splitter_create(IUnknown *outer, IUnknown **out)
{
    static const WCHAR sink_name[] = {'I','n','p','u','t',0};
    struct parser *object;

    if (!parser_init_gstreamer())
        return E_FAIL;

    mark_wine_thread();

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return E_OUTOFMEMORY;

    if (!(object->wg_parser = unix_funcs->wg_mpeg_audio_parser_create()))
    {
        heap_free(object);
        return E_OUTOFMEMORY;
    }

    strmbase_filter_init(&object->filter, outer, &CLSID_MPEG1Splitter, &mpeg_splitter_ops);
    strmbase_sink_init(&object->sink, &object->filter, sink_name, &mpeg_splitter_sink_ops, NULL);
    object->IAMStreamSelect_iface.lpVtbl = &stream_select_vtbl;

    object->init_gst = mpeg_splitter_filter_init_gst;
    object->source_query_accept = mpeg_splitter_source_query_accept;
    object->source_get_media_type = mpeg_splitter_source_get_media_type;
    object->enum_sink_first = TRUE;

    TRACE("Created MPEG-1 splitter %p.\n", object);
    *out = &object->filter.IUnknown_inner;
    return S_OK;
}
