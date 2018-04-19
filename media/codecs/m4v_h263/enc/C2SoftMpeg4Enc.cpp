/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#ifdef MPEG4
  #define LOG_TAG "C2SoftMpeg4Enc"
#else
  #define LOG_TAG "C2SoftH263Enc"
#endif
#include <utils/Log.h>
#include <utils/misc.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/MediaDefs.h>
#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>
#include <inttypes.h>

#include "C2SoftMpeg4Enc.h"
#include "mp4enc_api.h"

namespace android {

#ifdef MPEG4
constexpr char kComponentName[] = "c2.google.mpeg4.encoder";
#define CODEC_WIDTH    720
#define CODEC_HEIGHT   480
#define CODEC_BITRATE      216000
#define CODEC_FRAMERATE    17
#else
constexpr char kComponentName[] = "c2.google.h263.encoder";
#define CODEC_WIDTH    176
#define CODEC_HEIGHT   144
#define CODEC_BITRATE      128000
#define CODEC_FRAMERATE    15
#endif

std::shared_ptr<C2ComponentInterface> BuildIntf(
        const char *name, c2_node_id_t id,
        std::function<void(C2ComponentInterface*)> deleter =
            std::default_delete<C2ComponentInterface>()) {
    return SimpleC2Interface::Builder(name, id, deleter)
            .inputFormat(C2FormatVideo)
            .outputFormat(C2FormatCompressed)
            .inputMediaType(MEDIA_MIMETYPE_VIDEO_RAW)
            .outputMediaType(
#ifdef MPEG4
                    MEDIA_MIMETYPE_VIDEO_MPEG4
#else
                    MEDIA_MIMETYPE_VIDEO_H263
#endif
            )
            .build();
}

C2SoftMpeg4Enc::C2SoftMpeg4Enc(const char *name, c2_node_id_t id)
    : SimpleC2Component(BuildIntf(name, id)),
      mHandle(nullptr),
      mEncParams(nullptr),
      mStarted(false),
      mWidth(CODEC_WIDTH),
      mHeight(CODEC_HEIGHT),
      mFramerate(CODEC_FRAMERATE),
      mBitrate(CODEC_BITRATE),
      mOutBufferSize(524288),
      mKeyFrameInterval(10) {
}

C2SoftMpeg4Enc::~C2SoftMpeg4Enc() {
    onRelease();
}

c2_status_t C2SoftMpeg4Enc::onInit() {
#ifdef MPEG4
    mEncodeMode = COMBINE_MODE_WITH_ERR_RES;
#else
    mEncodeMode = H263_MODE;
#endif
    if (!mHandle) {
        mHandle = new tagvideoEncControls;
    }
    if (!mEncParams) {
        mEncParams = new tagvideoEncOptions;
    }
    mSignalledOutputEos = false;
    mSignalledError = false;

    return initEncoder();
}

c2_status_t C2SoftMpeg4Enc::onStop() {
    if (!mStarted) {
        return C2_OK;
    }
    if (mHandle) {
        (void)PVCleanUpVideoEncoder(mHandle);
    }
    mStarted = false;
    mSignalledOutputEos = false;
    mSignalledError = false;
    return C2_OK;
}

void C2SoftMpeg4Enc::onReset() {
    onStop();
    initEncoder();
}

void C2SoftMpeg4Enc::onRelease() {
    onStop();
    if (mEncParams) {
        delete mEncParams;
        mEncParams = nullptr;
    }
    if (mHandle) {
        delete mHandle;
        mHandle = nullptr;
    }
}

c2_status_t C2SoftMpeg4Enc::onFlush_sm() {
    return C2_OK;
}

static void ConvertRGBToPlanarYUV(uint8_t *dstY, size_t dstHStride,
                                  size_t dstVStride, const C2GraphicView &src) {
    CHECK((dstHStride & 1) == 0);
    CHECK((dstVStride & 1) == 0);

    uint8_t *dstU = dstY + dstHStride * dstVStride;
    uint8_t *dstV = dstU + (dstHStride >> 1) * (dstVStride >> 1);

    const C2PlanarLayout &layout = src.layout();
    const uint8_t *pRed   = src.data()[C2PlanarLayout::PLANE_R];
    const uint8_t *pGreen = src.data()[C2PlanarLayout::PLANE_G];
    const uint8_t *pBlue  = src.data()[C2PlanarLayout::PLANE_B];

    for (size_t y = 0; y < dstVStride; ++y) {
        for (size_t x = 0; x < dstHStride; ++x) {
            unsigned red   = *pRed;
            unsigned green = *pGreen;
            unsigned blue  = *pBlue;

            // using ITU-R BT.601 conversion matrix
            unsigned luma =
                ((red * 66 + green * 129 + blue * 25) >> 8) + 16;

            dstY[x] = luma;

            if ((x & 1) == 0 && (y & 1) == 0) {
                unsigned U =
                    ((-red * 38 - green * 74 + blue * 112) >> 8) + 128;

                unsigned V =
                    ((red * 112 - green * 94 - blue * 18) >> 8) + 128;

                dstU[x >> 1] = U;
                dstV[x >> 1] = V;
            }
            pRed   += layout.planes[C2PlanarLayout::PLANE_R].colInc;
            pGreen += layout.planes[C2PlanarLayout::PLANE_G].colInc;
            pBlue  += layout.planes[C2PlanarLayout::PLANE_B].colInc;
        }

        if ((y & 1) == 0) {
            dstU += dstHStride >> 1;
            dstV += dstHStride >> 1;
        }

        pRed   -= layout.planes[C2PlanarLayout::PLANE_R].colInc * dstHStride;
        pGreen -= layout.planes[C2PlanarLayout::PLANE_G].colInc * dstHStride;
        pBlue  -= layout.planes[C2PlanarLayout::PLANE_B].colInc * dstHStride;
        pRed   += layout.planes[C2PlanarLayout::PLANE_R].rowInc;
        pGreen += layout.planes[C2PlanarLayout::PLANE_G].rowInc;
        pBlue  += layout.planes[C2PlanarLayout::PLANE_B].rowInc;

        dstY += dstHStride;
    }
}

static void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        ALOGV("signalling eos");
    }
    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

c2_status_t C2SoftMpeg4Enc::initEncParams() {
    if (mHandle) {
        memset(mHandle, 0, sizeof(tagvideoEncControls));
    } else return C2_CORRUPTED;
    if (mEncParams) {
        memset(mEncParams, 0, sizeof(tagvideoEncOptions));
    } else return C2_CORRUPTED;

    if (!PVGetDefaultEncOption(mEncParams, 0)) {
        ALOGE("Failed to get default encoding parameters");
        return C2_CORRUPTED;
    }

    if (mFramerate == 0) {
        ALOGE("Framerate should not be 0");
        return C2_BAD_VALUE;
    }

    mEncParams->encMode = mEncodeMode;
    mEncParams->encWidth[0] = mWidth;
    mEncParams->encHeight[0] = mHeight;
    mEncParams->encFrameRate[0] = mFramerate;
    mEncParams->rcType = VBR_1;
    mEncParams->vbvDelay = 5.0f;

    mEncParams->profile_level = CORE_PROFILE_LEVEL2;
    mEncParams->packetSize = 32;
    mEncParams->rvlcEnable = PV_OFF;
    mEncParams->numLayers = 1;
    mEncParams->timeIncRes = 1000;
    mEncParams->tickPerSrc = mEncParams->timeIncRes / mFramerate;

    mEncParams->bitRate[0] = mBitrate;
    mEncParams->iQuant[0] = 15;
    mEncParams->pQuant[0] = 12;
    mEncParams->quantType[0] = 0;
    mEncParams->noFrameSkipped = PV_OFF;

    // PV's MPEG4 encoder requires the video dimension of multiple
    if (mWidth % 16 != 0 || mHeight % 16 != 0) {
        ALOGE("Video frame size %dx%d must be a multiple of 16", mWidth, mHeight);
        return C2_BAD_VALUE;
    }

    // Set IDR frame refresh interval
    mEncParams->intraPeriod = mKeyFrameInterval;
    mEncParams->numIntraMB = 0;
    mEncParams->sceneDetect = PV_ON;
    mEncParams->searchRange = 16;
    mEncParams->mv8x8Enable = PV_OFF;
    mEncParams->gobHeaderInterval = 0;
    mEncParams->useACPred = PV_ON;
    mEncParams->intraDCVlcTh = 0;

    return C2_OK;
}

c2_status_t C2SoftMpeg4Enc::initEncoder() {
    if (mStarted) {
        return C2_OK;
    }
    c2_status_t err = initEncParams();
    if (C2_OK != err) {
        ALOGE("Failed to initialized encoder params");
        mSignalledError = true;
        return err;
    }
    if (!PVInitVideoEncoder(mHandle, mEncParams)) {
        ALOGE("Failed to initialize the encoder");
        mSignalledError = true;
        return C2_CORRUPTED;
    }

    // 1st buffer for codec specific data
    mNumInputFrames = -1;
    mStarted = true;
    return C2_OK;
}

void C2SoftMpeg4Enc::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    // Initialize encoder if not already initialized
    if (!mStarted && C2_OK != initEncoder()) {
        ALOGE("Failed to initialize encoder");
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        return;
    }

    std::shared_ptr<C2LinearBlock> block;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
    c2_status_t err = pool->fetchLinearBlock(mOutBufferSize, usage, &block);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock for Output failed with status %d", err);
        work->result = C2_NO_MEMORY;
        return;
    }

    C2WriteView wView = block->map().get();
    if (wView.error()) {
        ALOGE("write view map failed %d", wView.error());
        work->result = wView.error();
        return;
    }

    uint8_t *outPtr = (uint8_t *)wView.data();
    if (mNumInputFrames < 0) {
        // The very first thing we want to output is the codec specific data.
        int32_t outputSize = mOutBufferSize;
        if (!PVGetVolHeader(mHandle, outPtr, &outputSize, 0)) {
            ALOGE("Failed to get VOL header");
            work->result = C2_CORRUPTED;
            mSignalledError = true;
            return;
        } else {
            ALOGV("Bytes Generated in header %d\n", outputSize);
        }

        ++mNumInputFrames;
        std::unique_ptr<C2StreamCsdInfo::output> csd =
            C2StreamCsdInfo::output::AllocUnique(outputSize, 0u);
        memcpy(csd->m.value, outPtr, outputSize);
        work->worklets.front()->output.configUpdate.push_back(std::move(csd));
    }

    uint64_t inputTimeStamp = work->input.ordinal.timestamp.peekull();
    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    const C2ConstGraphicBlock inBuffer = work->input.buffers[0]->data().graphicBlocks().front();
    if (inBuffer.width() == 0 || inBuffer.height() == 0) {
        fillEmptyWork(work);
        if (eos) {
            mSignalledOutputEos = true;
            ALOGV("signalled EOS");
        }
        return;
    }
    if (inBuffer.width() < mWidth || inBuffer.height() < mHeight) {
        /* Expect width height to be configured */
        ALOGW("unexpected Capacity Aspect %d(%d) x %d(%d)", inBuffer.width(),
              mWidth, inBuffer.height(),  mHeight);
        work->result = C2_BAD_VALUE;
        return;
    }
    const C2GraphicView rView = work->input.buffers[0]->data().graphicBlocks().front().map().get();
    if (rView.error() != C2_OK) {
        ALOGE("graphic view map err = %d", rView.error());
        work->result = rView.error();
        return;
    }

    const C2PlanarLayout &layout = rView.layout();
    uint8_t *yPlane = const_cast<uint8_t *>(rView.data()[C2PlanarLayout::PLANE_Y]);
    uint8_t *uPlane = const_cast<uint8_t *>(rView.data()[C2PlanarLayout::PLANE_U]);
    uint8_t *vPlane = const_cast<uint8_t *>(rView.data()[C2PlanarLayout::PLANE_V]);
    int32_t yStride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    int32_t uStride = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
    int32_t vStride = layout.planes[C2PlanarLayout::PLANE_V].rowInc;

    switch (layout.type) {
        case C2PlanarLayout::TYPE_RGB:
            // fall-through
        case C2PlanarLayout::TYPE_RGBA: {
            size_t yPlaneSize = mWidth * mHeight;
            std::unique_ptr<uint8_t[]> freeBuffer;
            if (mFreeConversionBuffers.empty()) {
                freeBuffer.reset(new uint8_t[yPlaneSize * 3 / 2]);
            } else {
                freeBuffer.swap(mFreeConversionBuffers.front());
                mFreeConversionBuffers.pop_front();
            }
            yPlane = freeBuffer.get();
            mConversionBuffersInUse.push_back(std::move(freeBuffer));
            uPlane = yPlane + yPlaneSize;
            vPlane = uPlane + yPlaneSize / 4;
            yStride = mWidth;
            uStride = vStride = mWidth / 2;
            ConvertRGBToPlanarYUV(yPlane, yStride, mHeight, rView);
            break;
        }
        case C2PlanarLayout::TYPE_YUV:
            // fall-through
        case C2PlanarLayout::TYPE_YUVA:
            // Do nothing
            break;
        default:
            ALOGE("Unrecognized plane type: %d", layout.type);
            work->result = C2_BAD_VALUE;
    }

    CHECK(NULL != yPlane);
    /* Encode frames */
    VideoEncFrameIO vin, vout;
    memset(&vin, 0, sizeof(vin));
    memset(&vout, 0, sizeof(vout));
    vin.yChan = yPlane;
    vin.uChan = uPlane;
    vin.vChan = vPlane;
    vin.timestamp = (inputTimeStamp + 500) / 1000;  // in ms
    vin.height = align(mHeight, 16);
    vin.pitch = align(mWidth, 16);

    uint32_t modTimeMs = 0;
    int32_t nLayer = 0;
    MP4HintTrack hintTrack;
    int32_t outputSize = mOutBufferSize;
    if (!PVEncodeVideoFrame(mHandle, &vin, &vout, &modTimeMs, outPtr, &outputSize, &nLayer) ||
        !PVGetHintTrack(mHandle, &hintTrack)) {
        ALOGE("Failed to encode frame or get hint track at frame %" PRId64, mNumInputFrames);
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        return;
    }
    ALOGV("outputSize filled : %d", outputSize);
    ++mNumInputFrames;
    CHECK(NULL == PVGetOverrunBuffer(mHandle));

    fillEmptyWork(work);
    if (outputSize) {
        std::shared_ptr<C2Buffer> buffer = createLinearBuffer(block, 0, outputSize);
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal.timestamp = inputTimeStamp;
        if (hintTrack.CodeType == 0) {
            buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
                    0u /* stream id */, C2PictureTypeKeyFrame));
        }
    }
    if (eos) {
        mSignalledOutputEos = true;
    }

    auto it = std::find_if(
            mConversionBuffersInUse.begin(), mConversionBuffersInUse.end(),
            [yPlane](const auto &elem) { return elem.get() == yPlane; });
    if (it != mConversionBuffersInUse.end()) {
        mFreeConversionBuffers.push_back(std::move(*it));
        mConversionBuffersInUse.erase(it);
    }
}

c2_status_t C2SoftMpeg4Enc::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    (void)pool;
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    return C2_OK;
}


class C2SoftMpeg4EncFactory : public C2ComponentFactory {
public:
    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        *component = std::shared_ptr<C2Component>(new C2SoftMpeg4Enc(kComponentName, id), deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = BuildIntf(kComponentName, id, deleter);
        return C2_OK;
    }

    virtual ~C2SoftMpeg4EncFactory() override = default;
};

}  // namespace android

extern "C" ::C2ComponentFactory* CreateCodec2Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2SoftMpeg4EncFactory();
}

extern "C" void DestroyCodec2Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}
