/*
 * Copyright 2018, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "Codec2BufferUtils"
#include <utils/Log.h>

#include <media/hardware/HardwareAPI.h>
#include <media/stagefright/foundation/AUtils.h>

#include "Codec2BufferUtils.h"

namespace android {

namespace {

/**
 * A flippable, optimizable memcpy. Constructs such as (from ? src : dst) do not work as the results are
 * always const.
 */
template<bool ToA, size_t S>
struct MemCopier {
    template<typename A, typename B>
    inline static void copy(A *a, const B *b, size_t size) {
        __builtin_memcpy(a, b, size);
    }
};

template<size_t S>
struct MemCopier<false, S> {
    template<typename A, typename B>
    inline static void copy(const A *a, B *b, size_t size) {
        MemCopier<true, S>::copy(b, a, size);
    }
};

/**
 * Copies between a MediaImage and a graphic view.
 *
 * \param ToMediaImage whether to copy to (or from) the MediaImage
 * \param view graphic view (could be ConstGraphicView or GraphicView depending on direction)
 * \param img MediaImage data
 * \param imgBase base of MediaImage (could be const uint8_t* or uint8_t* depending on direction)
 */
template<bool ToMediaImage, typename View, typename ImagePixel>
static status_t _ImageCopy(View &view, const MediaImage2 *img, ImagePixel *imgBase) {
    // TODO: more efficient copying --- e.g. one row at a time, copying
    //       interleaved planes together, etc.
    const C2PlanarLayout &layout = view.layout();
    const size_t bpp = divUp(img->mBitDepthAllocated, 8u);
    if (view.width() != img->mWidth
            || view.height() != img->mHeight) {
        return BAD_VALUE;
    }
    for (uint32_t i = 0; i < layout.numPlanes; ++i) {
        typename std::conditional<ToMediaImage, uint8_t, const uint8_t>::type *imgRow =
            imgBase + img->mPlane[i].mOffset;
        typename std::conditional<ToMediaImage, const uint8_t, uint8_t>::type *viewRow =
            viewRow = view.data()[i];
        const C2PlaneInfo &plane = layout.planes[i];
        if (plane.colSampling != img->mPlane[i].mHorizSubsampling
                || plane.rowSampling != img->mPlane[i].mVertSubsampling
                || plane.allocatedDepth != img->mBitDepthAllocated
                || plane.allocatedDepth < plane.bitDepth
                // MediaImage only supports MSB values
                || plane.rightShift != plane.allocatedDepth - plane.bitDepth
                || (bpp > 1 && plane.endianness != plane.NATIVE)) {
            return BAD_VALUE;
        }

        uint32_t planeW = img->mWidth / plane.colSampling;
        uint32_t planeH = img->mHeight / plane.rowSampling;
        for (uint32_t row = 0; row < planeH; ++row) {
            decltype(imgRow) imgPtr = imgRow;
            decltype(viewRow) viewPtr = viewRow;
            for (uint32_t col = 0; col < planeW; ++col) {
                MemCopier<ToMediaImage, 0>::copy(imgPtr, viewPtr, bpp);
                imgPtr += img->mPlane[i].mColInc;
                viewPtr += plane.colInc;
            }
            imgRow += img->mPlane[i].mRowInc;
            viewRow += plane.rowInc;
        }
    }
    return OK;
}

}  // namespace

status_t ImageCopy(uint8_t *imgBase, const MediaImage2 *img, const C2GraphicView &view) {
    return _ImageCopy<true>(view, img, imgBase);
}

status_t ImageCopy(C2GraphicView &view, const uint8_t *imgBase, const MediaImage2 *img) {
    return _ImageCopy<false>(view, img, imgBase);
}

bool IsYUV420(const C2GraphicView &view) {
    const C2PlanarLayout &layout = view.layout();
    return (layout.numPlanes == 3
            && layout.type == C2PlanarLayout::TYPE_YUV
            && layout.planes[layout.PLANE_Y].channel == C2PlaneInfo::CHANNEL_Y
            && layout.planes[layout.PLANE_Y].allocatedDepth == 8
            && layout.planes[layout.PLANE_Y].bitDepth == 8
            && layout.planes[layout.PLANE_Y].rightShift == 0
            && layout.planes[layout.PLANE_Y].colSampling == 1
            && layout.planes[layout.PLANE_Y].rowSampling == 1
            && layout.planes[layout.PLANE_U].channel == C2PlaneInfo::CHANNEL_CB
            && layout.planes[layout.PLANE_U].allocatedDepth == 8
            && layout.planes[layout.PLANE_U].bitDepth == 8
            && layout.planes[layout.PLANE_U].rightShift == 0
            && layout.planes[layout.PLANE_U].colSampling == 2
            && layout.planes[layout.PLANE_U].rowSampling == 2
            && layout.planes[layout.PLANE_V].channel == C2PlaneInfo::CHANNEL_CR
            && layout.planes[layout.PLANE_V].allocatedDepth == 8
            && layout.planes[layout.PLANE_V].bitDepth == 8
            && layout.planes[layout.PLANE_V].rightShift == 0
            && layout.planes[layout.PLANE_V].colSampling == 2
            && layout.planes[layout.PLANE_V].rowSampling == 2);
}

MediaImage2 CreateYUV420PlanarMediaImage2(
        uint32_t width, uint32_t height, uint32_t stride, uint32_t vstride) {
    return MediaImage2 {
        .mType = MediaImage2::MEDIA_IMAGE_TYPE_YUV,
        .mNumPlanes = 3,
        .mWidth = width,
        .mHeight = height,
        .mBitDepth = 8,
        .mPlane = {
            {
                .mOffset = 0,
                .mColInc = 1,
                .mRowInc = (int32_t)stride,
                .mHorizSubsampling = 1,
                .mVertSubsampling = 1,
            },
            {
                .mOffset = stride * vstride,
                .mColInc = 1,
                .mRowInc = (int32_t)stride / 2,
                .mHorizSubsampling = 2,
                .mVertSubsampling = 2,
            },
            {
                .mOffset = stride * vstride * 5 / 4,
                .mColInc = 1,
                .mRowInc = (int32_t)stride / 2,
                .mHorizSubsampling = 2,
                .mVertSubsampling = 2,
            }
        },
    };
}

MediaImage2 CreateYUV420SemiPlanarMediaImage2(
        uint32_t width, uint32_t height, uint32_t stride, uint32_t vstride) {
    return MediaImage2 {
        .mType = MediaImage2::MEDIA_IMAGE_TYPE_YUV,
        .mNumPlanes = 3,
        .mWidth = width,
        .mHeight = height,
        .mBitDepth = 8,
        .mPlane = {
            {
                .mOffset = 0,
                .mColInc = 1,
                .mRowInc = (int32_t)stride,
                .mHorizSubsampling = 1,
                .mVertSubsampling = 1,
            },
            {
                .mOffset = stride * vstride,
                .mColInc = 2,
                .mRowInc = (int32_t)stride,
                .mHorizSubsampling = 2,
                .mVertSubsampling = 2,
            },
            {
                .mOffset = stride * vstride + 1,
                .mColInc = 2,
                .mRowInc = (int32_t)stride,
                .mHorizSubsampling = 2,
                .mVertSubsampling = 2,
            }
        },
    };
}

}  // namespace android
