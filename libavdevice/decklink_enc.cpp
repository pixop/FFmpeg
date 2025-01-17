/*
 * Blackmagic DeckLink output
 * Copyright (c) 2013-2014 Ramiro Polla
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <atomic>
using std::atomic;

/* Include internal.h first to avoid conflict between winsock.h (used by
 * DeckLink headers) and winsock2.h (used by libavformat) in MSVC++ builds */
extern "C" {
#include "libavformat/internal.h"
}

#include <DeckLinkAPI_v14_2_1.h>

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/mastering_display_metadata.h"
#include "avdevice.h"
}

#include "decklink_common.h"
#include "decklink_enc.h"
#if CONFIG_LIBKLVANC
#include "libklvanc/vanc.h"
#include "libklvanc/vanc-lines.h"
#include "libklvanc/pixels.h"
#endif

static inline bool is_equal_guid(const REFIID &a, const REFIID &b) {
    return memcmp(&a, &b, sizeof(REFIID)) == 0;
}

/* DeckLink callback class declaration */
class decklink_frame : public IDeckLinkVideoFrame_v14_2_1, public IDeckLinkVideoFrameMetadataExtensions
{
public:
    decklink_frame(struct decklink_ctx *ctx, AVFrame *avframe, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(avframe), _avpacket(NULL), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width),  _refs(1) { }
    decklink_frame(struct decklink_ctx *ctx, AVPacket *avpacket, AVCodecID codec_id, int height, int width) :
        _ctx(ctx), _avframe(NULL), _avpacket(avpacket), _codec_id(codec_id), _ancillary(NULL), _height(height), _width(width), _refs(1) { }
    virtual long           STDMETHODCALLTYPE GetWidth      (void)          { return _width; }
    virtual long           STDMETHODCALLTYPE GetHeight     (void)          { return _height; }
    virtual long           STDMETHODCALLTYPE GetRowBytes   (void)
    {
      if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
          return _avframe->linesize[0] < 0 ? -_avframe->linesize[0] : _avframe->linesize[0];
      else
          return ((GetWidth() + 47) / 48) * 128;
    }
    virtual BMDPixelFormat STDMETHODCALLTYPE GetPixelFormat(void)
    {
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME)
            return bmdFormat8BitYUV;
        else
            return bmdFormat10BitYUV;
    }
    virtual BMDFrameFlags  STDMETHODCALLTYPE GetFlags      (void)
    {
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
            return _avframe->linesize[0] < 0 ? bmdFrameFlagFlipVertical : bmdFrameFlagDefault;
        } else {
            if (_ctx->supports_hdr && (hdr || lighting))
                return bmdFrameFlagDefault | bmdFrameContainsHDRMetadata;
            else
                return bmdFrameFlagDefault;
        }
    }

    virtual HRESULT        STDMETHODCALLTYPE GetBytes      (void **buffer)
    {
        if (_codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
            if (_avframe->linesize[0] < 0)
                *buffer = (void *)(_avframe->data[0] + _avframe->linesize[0] * (_avframe->height - 1));
            else
                *buffer = (void *)(_avframe->data[0]);
        } else {
            *buffer = (void *)(_avpacket->data);
        }
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE GetTimecode     (BMDTimecodeFormat format, IDeckLinkTimecode **timecode) { return S_FALSE; }
    virtual HRESULT STDMETHODCALLTYPE GetAncillaryData(IDeckLinkVideoFrameAncillary **ancillary)
    {
        *ancillary = _ancillary;
        if (_ancillary) {
            _ancillary->AddRef();
            return S_OK;
        } else {
            return S_FALSE;
        }
    }
    virtual HRESULT STDMETHODCALLTYPE SetAncillaryData(IDeckLinkVideoFrameAncillary *ancillary)
    {
        if (_ancillary)
            _ancillary->Release();
        _ancillary = ancillary;
        _ancillary->AddRef();
        return S_OK;
    }
/*
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) 
    {
        if (is_equal_guid(iid, IID_IDeckLinkVideoFrame_v14_2_1)) 
        { 
            *ppv = (IDeckLinkVideoFrame_v14_2_1*)this; 
            AddRef(); 
            return S_OK; 
        }
        return E_NOINTERFACE; 
    }
*/

    virtual HRESULT STDMETHODCALLTYPE SetMetadata(enum AVColorSpace colorspace, enum AVColorTransferCharacteristic eotf)
    {
        _colorspace = colorspace;
        _eotf = eotf;
        return S_OK;
    }

    // IDeckLinkVideoFrameMetadataExtensions interface
    virtual HRESULT GetInt(BMDDeckLinkFrameMetadataID metadataID, int64_t* value)
    {
        HRESULT result = S_OK;

        switch (metadataID) {
        case bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc:
            /* See CTA-861-G Sec 6.9 Dynamic Range and Mastering */

            switch(_eotf) {
            case AVCOL_TRC_SMPTEST2084:
                /* PQ */
                *value = 2;
               break;
            case AVCOL_TRC_ARIB_STD_B67:
                /* Also known as "HLG" */
                *value = 3;
                break;
            case AVCOL_TRC_SMPTE170M:
            case AVCOL_TRC_SMPTE240M:
            case AVCOL_TRC_BT709:
            default:
                /* SDR */
                *value = 0;
               break;
            }
            break;

        case bmdDeckLinkFrameMetadataColorspace:
            if (!_ctx->supports_colorspace) {
                result = E_NOTIMPL;
                break;
            }
            switch(_colorspace) {
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
            case AVCOL_SPC_SMPTE240M:
                *value = bmdColorspaceRec601;
                break;
            case AVCOL_SPC_BT2020_CL:
            case AVCOL_SPC_BT2020_NCL:
                *value = bmdColorspaceRec2020;
                break;
            case AVCOL_SPC_BT709:
            default:
                *value = bmdColorspaceRec709;
                break;
            }
            break;
        default:
            result = E_INVALIDARG;
        }

        return result;
    }
    virtual HRESULT GetFloat(BMDDeckLinkFrameMetadataID metadataID, double* value)
    {
        *value = 0;

        switch (metadataID) {
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[0][0]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[0][1]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[1][0]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[1][1]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[2][0]);
            break;
        case bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->display_primaries[2][1]);
            break;
        case bmdDeckLinkFrameMetadataHDRWhitePointX:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->white_point[0]);
            break;
        case bmdDeckLinkFrameMetadataHDRWhitePointY:
            if (hdr && hdr->has_primaries)
                *value = av_q2d(hdr->white_point[1]);
            break;
        case bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance:
            if (hdr && hdr->has_luminance)
                *value = av_q2d(hdr->max_luminance);
            break;
        case bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance:
            if (hdr && hdr->has_luminance)
                *value = av_q2d(hdr->min_luminance);
            break;
        case bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel:
            if (lighting)
                *value = (float) lighting->MaxCLL;
            else
                *value = 0;
            break;
        case bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel:
            if (lighting)
                *value = (float) lighting->MaxFALL;
            else
                *value = 0;
            break;
        default:
            return E_INVALIDARG;
        }

        return S_OK;
    }

    virtual HRESULT GetFlag(BMDDeckLinkFrameMetadataID metadataID, bool* value)
    {
        *value = false;
        return E_INVALIDARG;
    }
    virtual HRESULT GetString(BMDDeckLinkFrameMetadataID metadataID, const char** value)
    {
        *value = nullptr;
        return E_INVALIDARG;
    }
    virtual HRESULT GetBytes(BMDDeckLinkFrameMetadataID metadataID, void* buffer, uint32_t* bufferSize)
    {
        *bufferSize = 0;
        return E_INVALIDARG;
    }

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv)
    {
        CFUUIDBytes             iunknown;
        HRESULT                 result          = S_OK;

        if (!ppv)
            return E_INVALIDARG;

        *ppv = NULL;

        iunknown = CFUUIDGetUUIDBytes(IUnknownUUID);
        if (memcmp(&iid, &iunknown, sizeof(REFIID)) == 0) {
            *ppv = this;
            AddRef();
        } else if (memcmp(&iid, &IID_IDeckLinkVideoFrame_v14_2_1, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrame_v14_2_1*>(this);
            AddRef();
        } else if (memcmp(&iid, &IID_IDeckLinkVideoFrameMetadataExtensions, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoFrameMetadataExtensions*>(this);
            AddRef();
        } else {
            result = E_NOINTERFACE;
        }

        return result;
    }

    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return ++_refs; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)
    {
        int ret = --_refs;
        if (!ret) {
            av_frame_free(&_avframe);
            av_packet_free(&_avpacket);
            if (_ancillary)
                _ancillary->Release();
            delete this;
        }
        return ret;
    }

    struct decklink_ctx *_ctx;
    AVFrame *_avframe;
    AVPacket *_avpacket;
    AVCodecID _codec_id;
    IDeckLinkVideoFrameAncillary *_ancillary;
    int _height;
    int _width;
    enum AVColorSpace _colorspace;
    enum AVColorTransferCharacteristic _eotf;
    const AVMasteringDisplayMetadata *hdr;
    const AVContentLightMetadata *lighting;

private:
    std::atomic<int>  _refs;
};

class decklink_output_callback : public IDeckLinkVideoOutputCallback_v14_2_1
{
public:
    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame_v14_2_1 *_frame, BMDOutputFrameCompletionResult result)
    {
        decklink_frame *frame = static_cast<decklink_frame *>(_frame);
        struct decklink_ctx *ctx = frame->_ctx;

        if (frame->_avframe)
            av_frame_unref(frame->_avframe);
        if (frame->_avpacket)
            av_packet_unref(frame->_avpacket);

        pthread_mutex_lock(&ctx->mutex);
        ctx->frames_buffer_available_spots++;
        pthread_cond_broadcast(&ctx->cond);
        pthread_mutex_unlock(&ctx->mutex);

        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped(void)       { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv)
    {
        if (is_equal_guid(iid, IID_IDeckLinkVideoOutputCallback_v14_2_1)) 
        { 
            *ppv = (IDeckLinkVideoOutputCallback_v14_2_1*)this; 
            AddRef(); 
            return S_OK; 
        }
        return E_NOINTERFACE; 
    }
    virtual ULONG   STDMETHODCALLTYPE AddRef(void)                            { return 1; }
    virtual ULONG   STDMETHODCALLTYPE Release(void)                           { return 1; }
};

static int decklink_setup_video(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVCodecParameters *c = st->codecpar;

    if (ctx->video) {
        av_log(avctx, AV_LOG_ERROR, "Only one video stream is supported!\n");
        return -1;
    }

    if (c->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (c->format != AV_PIX_FMT_UYVY422) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format!"
                   " Only AV_PIX_FMT_UYVY422 is supported.\n");
            return -1;
        }
        ctx->raw_format = bmdFormat8BitYUV;
    } else if (c->codec_id != AV_CODEC_ID_V210) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec type!"
               " Only V210 and wrapped frame with AV_PIX_FMT_UYVY422 are supported.\n");
        return -1;
    } else {
        ctx->raw_format = bmdFormat10BitYUV;
    }

    if (ff_decklink_set_configs(avctx, DIRECTION_OUT) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not set output configuration\n");
        return -1;
    }
    if (ff_decklink_set_format(avctx, c->width, c->height,
                            st->time_base.num, st->time_base.den, c->field_order)) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported video size, framerate or field order!"
               " Check available formats with -list_formats 1.\n");
        return -1;
    }
    if (ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputVANC) != S_OK) {
        av_log(avctx, AV_LOG_WARNING, "Could not enable video output with VANC! Trying without...\n");
        ctx->supports_vanc = 0;
    }
    if (!ctx->supports_vanc && ctx->dlo->EnableVideoOutput(ctx->bmd_mode, bmdVideoOutputFlagDefault) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable video output!\n");
        return -1;
    }

    /* Set callback. */
    ctx->output_callback = new decklink_output_callback();
    ctx->dlo->SetScheduledFrameCompletionCallback(ctx->output_callback);

    ctx->frames_preroll = st->time_base.den * ctx->preroll;
    if (st->time_base.den > 1000)
        ctx->frames_preroll /= 1000;

    /* Buffer twice as many frames as the preroll. */
    ctx->frames_buffer = ctx->frames_preroll * 2;
    ctx->frames_buffer = FFMIN(ctx->frames_buffer, 60);
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->frames_buffer_available_spots = ctx->frames_buffer;

    av_log(avctx, AV_LOG_DEBUG, "output: %s, preroll: %d, frames buffer size: %d\n",
           avctx->url, ctx->frames_preroll, ctx->frames_buffer);

    /* The device expects the framerate to be fixed. */
    avpriv_set_pts_info(st, 64, st->time_base.num, st->time_base.den);

    ctx->video = 1;

    return 0;
}

static int decklink_setup_audio(AVFormatContext *avctx, AVStream *st)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVCodecParameters *c = st->codecpar;

    if (ctx->audio) {
        av_log(avctx, AV_LOG_ERROR, "Only one audio stream is supported!\n");
        return -1;
    }
    if (c->sample_rate != 48000) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate!"
               " Only 48kHz is supported.\n");
        return -1;
    }
    if (c->channels != 2 && c->channels != 8 && c->channels != 16) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported number of channels!"
               " Only 2, 8 or 16 channels are supported.\n");
        return -1;
    }
    if (ctx->dlo->EnableAudioOutput(bmdAudioSampleRate48kHz,
                                    bmdAudioSampleType16bitInteger,
                                    c->channels,
                                    bmdAudioOutputStreamTimestamped) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not enable audio output!\n");
        return -1;
    }
    if (ctx->dlo->BeginAudioPreroll() != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not begin audio preroll!\n");
        return -1;
    }

    /* The device expects the sample rate to be fixed. */
    avpriv_set_pts_info(st, 64, 1, c->sample_rate);
    ctx->channels = c->channels;

    ctx->audio = 1;

    return 0;
}

av_cold int ff_decklink_write_trailer(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;

    if (ctx->playback_started) {
        BMDTimeValue actual;
        ctx->dlo->StopScheduledPlayback(ctx->last_pts * ctx->bmd_tb_num,
                                        &actual, ctx->bmd_tb_den);
        ctx->dlo->DisableVideoOutput();
        if (ctx->audio)
            ctx->dlo->DisableAudioOutput();
    }

    ff_decklink_cleanup(avctx);

    if (ctx->output_callback)
        delete ctx->output_callback;

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);

#if CONFIG_LIBKLVANC
    klvanc_context_destroy(ctx->vanc_ctx);
#endif

    av_freep(&cctx->ctx);

    return 0;
}

#if CONFIG_LIBKLVANC
static void construct_cc(AVFormatContext *avctx, struct decklink_ctx *ctx,
                         AVPacket *pkt, struct klvanc_line_set_s *vanc_lines)
{
    struct klvanc_packet_eia_708b_s *cdp;
    uint16_t *cdp_words;
    uint16_t len;
    uint8_t cc_count;
    buffer_size_t size;
    int ret, i;

    const uint8_t *data = av_packet_get_side_data(pkt, AV_PKT_DATA_A53_CC, &size);
    if (!data)
        return;

    cc_count = size / 3;

    ret = klvanc_create_eia708_cdp(&cdp);
    if (ret)
        return;

    ret = klvanc_set_framerate_EIA_708B(cdp, ctx->bmd_tb_num, ctx->bmd_tb_den);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Invalid framerate specified: %lld/%lld\n",
               ctx->bmd_tb_num, ctx->bmd_tb_den);
        klvanc_destroy_eia708_cdp(cdp);
        return;
    }

    if (cc_count > KLVANC_MAX_CC_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "Illegal cc_count received: %d\n", cc_count);
        cc_count = KLVANC_MAX_CC_COUNT;
    }

    /* CC data */
    cdp->header.ccdata_present = 1;
    cdp->header.caption_service_active = 1;
    cdp->ccdata.cc_count = cc_count;
    for (i = 0; i < cc_count; i++) {
        if (data [3*i] & 0x04)
            cdp->ccdata.cc[i].cc_valid = 1;
        cdp->ccdata.cc[i].cc_type = data[3*i] & 0x03;
        cdp->ccdata.cc[i].cc_data[0] = data[3*i+1];
        cdp->ccdata.cc[i].cc_data[1] = data[3*i+2];
    }

    klvanc_finalize_EIA_708B(cdp, ctx->cdp_sequence_num++);
    ret = klvanc_convert_EIA_708B_to_words(cdp, &cdp_words, &len);
    klvanc_destroy_eia708_cdp(cdp);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed converting 708 packet to words\n");
        return;
    }

    ret = klvanc_line_insert(ctx->vanc_ctx, vanc_lines, cdp_words, len, 11, 0);
    free(cdp_words);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "VANC line insertion failed\n");
        return;
    }
}

static int decklink_construct_vanc(AVFormatContext *avctx, struct decklink_ctx *ctx,
                                   AVPacket *pkt, decklink_frame *frame)
{
    struct klvanc_line_set_s vanc_lines = { 0 };
    int ret = 0, i;

    if (!ctx->supports_vanc)
        return 0;

    construct_cc(avctx, ctx, pkt, &vanc_lines);

    IDeckLinkVideoFrameAncillary *vanc;
    int result = ctx->dlo->CreateAncillaryData(bmdFormat10BitYUV, &vanc);
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create vanc\n");
        ret = AVERROR(EIO);
        goto done;
    }

    /* Now that we've got all the VANC lines in a nice orderly manner, generate the
       final VANC sections for the Decklink output */
    for (i = 0; i < vanc_lines.num_lines; i++) {
        struct klvanc_line_s *line = vanc_lines.lines[i];
        int real_line;
        void *buf;

        if (!line)
            break;

        /* FIXME: include hack for certain Decklink cards which mis-represent
           line numbers for pSF frames */
        real_line = line->line_number;

        result = vanc->GetBufferForVerticalBlankingLine(real_line, &buf);
        if (result != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Failed to get VANC line %d: %d", real_line, result);
            continue;
        }

        /* Generate the full line taking into account all VANC packets on that line */
        result = klvanc_generate_vanc_line_v210(ctx->vanc_ctx, line, (uint8_t *) buf,
                                                ctx->bmd_width);
        if (result) {
            av_log(avctx, AV_LOG_ERROR, "Failed to generate VANC line\n");
            continue;
        }
    }

    result = frame->SetAncillaryData(vanc);
    vanc->Release();
    if (result != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Failed to set vanc: %d", result);
        ret = AVERROR(EIO);
    }

done:
    for (i = 0; i < vanc_lines.num_lines; i++)
        klvanc_line_free(vanc_lines.lines[i]);

    return ret;
}
#endif

static int decklink_write_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];
    AVFrame *avframe = NULL, *tmp = (AVFrame *)pkt->data;
    AVPacket *avpacket = NULL;
    decklink_frame *frame;
    uint32_t buffered;
    HRESULT hr;

    if (st->codecpar->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (tmp->format != AV_PIX_FMT_UYVY422 ||
            tmp->width  != ctx->bmd_width ||
            tmp->height != ctx->bmd_height) {
            av_log(avctx, AV_LOG_ERROR, "Got a frame with invalid pixel format or dimension.\n");
            return AVERROR(EINVAL);
        }

        avframe = av_frame_clone(tmp);
        if (!avframe) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone video frame.\n");
            return AVERROR(EIO);
        }

        frame = new decklink_frame(ctx, avframe, st->codecpar->codec_id, avframe->height, avframe->width);
    } else {
        avpacket = av_packet_clone(pkt);
        if (!avpacket) {
            av_log(avctx, AV_LOG_ERROR, "Could not clone video frame.\n");
            return AVERROR(EIO);
        }

        frame = new decklink_frame(ctx, avpacket, st->codecpar->codec_id, ctx->bmd_height, ctx->bmd_width);

#if CONFIG_LIBKLVANC
        if (decklink_construct_vanc(avctx, ctx, pkt, frame))
            av_log(avctx, AV_LOG_ERROR, "Failed to construct VANC\n");
#endif
    }

    if (!frame) {
        av_log(avctx, AV_LOG_ERROR, "Could not create new frame.\n");
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        return AVERROR(EIO);
    }

    /* Set frame metadata properties */
    int size;
    const AVMasteringDisplayMetadata *hdr = (const AVMasteringDisplayMetadata *) av_packet_get_side_data(pkt, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, &size);
    if (hdr && size > 0) {
        frame->hdr = hdr;
    } else {
        frame->hdr = nullptr;
    }

    const AVContentLightMetadata *lighting = (const AVContentLightMetadata *) av_packet_get_side_data(pkt, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, &size);
    if (lighting && size > 0) {
        frame->lighting = lighting;
    } else {
        frame->lighting = nullptr;
    }

    frame->SetMetadata(st->codecpar->color_space, st->codecpar->color_trc);

    /* Always keep at most one second of frames buffered. */
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->frames_buffer_available_spots == 0) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    ctx->frames_buffer_available_spots--;
    pthread_mutex_unlock(&ctx->mutex);

    /* Schedule frame for playback. */
    hr = ctx->dlo->ScheduleVideoFrame((class IDeckLinkVideoFrame_v14_2_1 *) frame,
                                      pkt->pts * ctx->bmd_tb_num,
                                      ctx->bmd_tb_num, ctx->bmd_tb_den);
    /* Pass ownership to DeckLink, or release on failure */
    frame->Release();
    if (hr != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule video frame."
                " error %08x.\n", (uint32_t) hr);
        return AVERROR(EIO);
    }

    ctx->dlo->GetBufferedVideoFrameCount(&buffered);
    av_log(avctx, AV_LOG_DEBUG, "Buffered video frames: %d.\n", (int) buffered);
    if (pkt->pts > 2 && buffered <= 2)
        av_log(avctx, AV_LOG_WARNING, "There are not enough buffered video frames."
               " Video may misbehave!\n");

    /* Preroll video frames. */
    if (!ctx->playback_started && pkt->pts > ctx->frames_preroll) {
        av_log(avctx, AV_LOG_DEBUG, "Ending audio preroll.\n");
        if (ctx->audio && ctx->dlo->EndAudioPreroll() != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not end audio preroll!\n");
            return AVERROR(EIO);
        }
        av_log(avctx, AV_LOG_DEBUG, "Starting scheduled playback.\n");
        if (ctx->dlo->StartScheduledPlayback(0, ctx->bmd_tb_den, 1.0) != S_OK) {
            av_log(avctx, AV_LOG_ERROR, "Could not start scheduled playback!\n");
            return AVERROR(EIO);
        }
        ctx->playback_started = 1;
    }

    return 0;
}

static int decklink_write_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    int sample_count = pkt->size / (ctx->channels << 1);
    uint32_t buffered;

    ctx->dlo->GetBufferedAudioSampleFrameCount(&buffered);
    if (pkt->pts > 1 && !buffered)
        av_log(avctx, AV_LOG_WARNING, "There's no buffered audio."
               " Audio will misbehave!\n");

    if (ctx->dlo->ScheduleAudioSamples(pkt->data, sample_count, pkt->pts,
                                       bmdAudioSampleRate48kHz, NULL) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not schedule audio samples.\n");
        return AVERROR(EIO);
    }

    return 0;
}

extern "C" {

av_cold int ff_decklink_write_header(AVFormatContext *avctx)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx;
    unsigned int n;
    int ret;

    ctx = (struct decklink_ctx *) av_mallocz(sizeof(struct decklink_ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    ctx->list_devices = cctx->list_devices;
    ctx->list_formats = cctx->list_formats;
    ctx->preroll      = cctx->preroll;
    ctx->duplex_mode  = cctx->duplex_mode;
    cctx->ctx = ctx;
#if CONFIG_LIBKLVANC
    if (klvanc_context_create(&ctx->vanc_ctx) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create VANC library context\n");
        return AVERROR(ENOMEM);
    }
    ctx->supports_vanc = 1;
#endif

    /* List available devices and exit. */
    if (ctx->list_devices) {
        ff_decklink_list_devices_legacy(avctx, 0, 1);
        return AVERROR_EXIT;
    }

    ret = ff_decklink_init_device(avctx, avctx->url);
    if (ret < 0)
        return ret;

    /* Get output device. */
    if (ctx->dl->QueryInterface(IID_IDeckLinkOutput_v14_2_1, (void **) &ctx->dlo) != S_OK) {
        av_log(avctx, AV_LOG_ERROR, "Could not open output device from '%s'\n",
               avctx->url);
        ret = AVERROR(EIO);
        goto error;
    }

    /* List supported formats. */
    if (ctx->list_formats) {
        ff_decklink_list_formats(avctx);
        ret = AVERROR_EXIT;
        goto error;
    }

    /* Setup streams. */
    ret = AVERROR(EIO);
    for (n = 0; n < avctx->nb_streams; n++) {
        AVStream *st = avctx->streams[n];
        AVCodecParameters *c = st->codecpar;
        if        (c->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (decklink_setup_audio(avctx, st))
                goto error;
        } else if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (decklink_setup_video(avctx, st))
                goto error;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Unsupported stream type.\n");
            goto error;
        }
    }

    return 0;

error:
    ff_decklink_cleanup(avctx);
    return ret;
}

int ff_decklink_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct decklink_cctx *cctx = (struct decklink_cctx *)avctx->priv_data;
    struct decklink_ctx *ctx = (struct decklink_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];

    ctx->last_pts = FFMAX(ctx->last_pts, pkt->pts);

    if      (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return decklink_write_video_packet(avctx, pkt);
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        return decklink_write_audio_packet(avctx, pkt);

    return AVERROR(EIO);
}

int ff_decklink_list_output_devices(AVFormatContext *avctx, struct AVDeviceInfoList *device_list)
{
    return ff_decklink_list_devices(avctx, device_list, 0, 1);
}

} /* extern "C" */
