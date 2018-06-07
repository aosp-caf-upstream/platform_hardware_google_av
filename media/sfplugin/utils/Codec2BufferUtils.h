/*
 * Copyright 2018, The Android Open Source Project
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

#ifndef CODEC2_BUFFER_UTILS_H_
#define CODEC2_BUFFER_UTILS_H_

#include <C2Buffer.h>

#include <media/hardware/VideoAPI.h>
#include <utils/Errors.h>

namespace android {

/**
 * Returns a planar YUV 420 8-bit media image descriptor.
 *
 * \param width width of image in pixels
 * \param height height of image in pixels
 * \param stride stride of image in pixels
 * \param vstride vertical stride of image in pixels
 */
MediaImage2 CreateYUV420PlanarMediaImage2(
        uint32_t width, uint32_t height, uint32_t stride, uint32_t vstride);

/**
 * Returns a semiplanar YUV 420 8-bit media image descriptor.
 *
 * \param width width of image in pixels
 * \param height height of image in pixels
 * \param stride stride of image in pixels
 * \param vstride vertical stride of image in pixels
 */
MediaImage2 CreateYUV420SemiPlanarMediaImage2(
        uint32_t width, uint32_t height, uint32_t stride, uint32_t vstride);

/**
 * Copies a graphic view into a media image.
 *
 * \param imgBase base of MediaImage
 * \param img MediaImage data
 * \param view graphic view
 *
 * \return OK on success
 */
status_t ImageCopy(uint8_t *imgBase, const MediaImage2 *img, const C2GraphicView &view);

/**
 * Copies a media image into a graphic view.
 *
 * \param view graphic view
 * \param imgBase base of MediaImage
 * \param img MediaImage data
 *
 * \return OK on success
 */
status_t ImageCopy(C2GraphicView &view, const uint8_t *imgBase, const MediaImage2 *img);

/**
 * Returns true iff a view has a YUV 420 888 layout.
 */
bool IsYUV420(const C2GraphicView &view);

} // namespace android

#endif  // CODEC2_BUFFER_UTILS_H_
