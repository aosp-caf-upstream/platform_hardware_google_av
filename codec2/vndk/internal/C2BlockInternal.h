/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef ANDROID_STAGEFRIGHT_C2BLOCK_INTERNAL_H_
#define ANDROID_STAGEFRIGHT_C2BLOCK_INTERNAL_H_

#include <C2Buffer.h>

namespace android {
namespace hardware {
namespace media {
namespace bufferpool {

struct BufferPoolData;

}
}
}
}

/**
 * Stores informations from C2BlockPool implementations which are required by C2Block.
 */
struct C2_HIDE _C2BlockPoolData {
    enum Type : int {
        TYPE_BUFFERPOOL = 0,
        TYPE_BUFFERQUEUE,
    };

    virtual Type getType() const = 0;

protected:
    _C2BlockPoolData() = default;

    virtual ~_C2BlockPoolData() = default;
};

/**
 * Internal only interface for creating blocks by block pool/buffer passing implementations.
 *
 * \todo this must be hidden
 */
struct _C2BlockFactory {
    /**
     * Create a linear block from an allocation for an allotted range.
     *
     * @param alloc parent allocation
     * @param data  blockpool data
     * @param offset allotted range offset
     * @param size  allotted size
     *
     * @return shared pointer to the linear block. nullptr if there was not enough memory to
     *         create this block.
     */
    static
    std::shared_ptr<C2LinearBlock> CreateLinearBlock(
            const std::shared_ptr<C2LinearAllocation> &alloc,
            const std::shared_ptr<_C2BlockPoolData> &data = nullptr,
            size_t offset = 0,
            size_t size = ~(size_t)0);

    /**
     * Create a graphic block from an allocation for an allotted section.
     *
     * @param alloc parent allocation
     * @param data  blockpool data
     * @param crop  allotted crop region
     *
     * @return shared pointer to the graphic block. nullptr if there was not enough memory to
     *         create this block.
     */
    static
    std::shared_ptr<C2GraphicBlock> CreateGraphicBlock(
            const std::shared_ptr<C2GraphicAllocation> &alloc,
            const std::shared_ptr<_C2BlockPoolData> &data = nullptr,
            const C2Rect &allottedCrop = C2Rect(~0u, ~0u));

    /**
     * Return a block pool data from 1D block.
     *
     * @param shared pointer to the 1D block which is already created.
     */
    static
    std::shared_ptr<const _C2BlockPoolData> GetLinearBlockPoolData(
            const C2Block1D& block);

    /**
     * Return a block pool data from 2D block.
     *
     * @param shared pointer to the 2D block which is already created.
     */
    static
    std::shared_ptr<const _C2BlockPoolData> GetGraphicBlockPoolData(
            const C2Block2D& block);

    /**
     * Create a linear block from the received native handle.
     *
     * @param handle    native handle to a linear block
     *
     * @return shared pointer to the linear block. nullptr if there was not enough memory to
     *         create this block.
     */
    static
    std::shared_ptr<C2LinearBlock> CreateLinearBlock(
            const C2Handle *handle);

    /**
     * Create a graphic block from the received native handle.
     *
     * @param handle    native handle to a graphic block
     *
     * @return shared pointer to the graphic block. nullptr if there was not enough memory to
     *         create this block.
     */
    static
    std::shared_ptr<C2GraphicBlock> CreateGraphicBlock(
            const C2Handle *handle);

    /**
     * Create a linear block from the received bufferpool data.
     *
     * @param data  bufferpool data to a linear block
     *
     * @return shared pointer to the linear block. nullptr if there was not enough memory to
     *         create this block.
     */
    static
    std::shared_ptr<C2LinearBlock> CreateLinearBlock(
            const std::shared_ptr<android::hardware::media::bufferpool::BufferPoolData> &data);

    /**
     * Create a graphic block from the received bufferpool data.
     *
     * @param data  bufferpool data to a graphic block
     *
     * @return shared pointer to the graphic block. nullptr if there was not enough memory to
     *         create this block.
     */
    static
    std::shared_ptr<C2GraphicBlock> CreateGraphicBlock(
            const std::shared_ptr<android::hardware::media::bufferpool::BufferPoolData> &data);

    /**
     * Get bufferpool data from the blockpool data.
     *
     * @param poolData          blockpool data
     * @param bufferPoolData    pointer to bufferpool data where the bufferpool
     *                          data is stored.
     *
     * @return {@code true} when there is valid bufferpool data, {@code false} otherwise.
     */
    static
    bool GetBufferPoolData(
            const std::shared_ptr<const _C2BlockPoolData> &poolData,
            std::shared_ptr<android::hardware::media::bufferpool::BufferPoolData> *bufferPoolData);

    /**
     * Get bufferqueue data from the blockpool data.
     *
     * @param poolData  blockpool data
     * @param igbp_id   pointer to id of igbp to be stored
     * @param igbp_slot pointer to slot of igbp to be stored
     *
     * @return {@code true} when there is valid bufferqueue data, {@code false} otherwise.
     */
    static
    bool GetBufferQueueData(
            const std::shared_ptr<const _C2BlockPoolData> &poolData,
            uint64_t *igbp_id,
            int32_t *igbp_slot);
};

#endif // ANDROID_STAGEFRIGHT_C2BLOCK_INTERNAL_H_

