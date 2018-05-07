/*
 * Copyright 2017, The Android Open Source Project
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
#define LOG_TAG "CCodecBufferChannel"
#include <utils/Log.h>

#include <numeric>
#include <thread>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <C2BlockInternal.h>
#include <C2Config.h>

#include <android/hardware/cas/native/1.0/IDescrambler.h>
#include <binder/MemoryDealer.h>
#include <gui/Surface.h>
#include <media/openmax/OMX_Core.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/MediaCodec.h>
#include <media/MediaCodecBuffer.h>
#include <system/window.h>

#include "CCodecBufferChannel.h"
#include "Codec2Buffer.h"

namespace android {

using hardware::hidl_handle;
using hardware::hidl_string;
using hardware::hidl_vec;
using namespace hardware::cas::V1_0;
using namespace hardware::cas::native::V1_0;

using CasStatus = hardware::cas::V1_0::Status;

/**
 * Base class for representation of buffers at one port.
 */
class CCodecBufferChannel::Buffers {
public:
    Buffers() = default;
    virtual ~Buffers() = default;

    /**
     * Set format for MediaCodec-facing buffers.
     */
    void setFormat(const sp<AMessage> &format) {
        CHECK(format != nullptr);
        mFormat = format;
    }

    /**
     * Returns true if the buffers are operating under array mode.
     */
    virtual bool isArrayMode() const { return false; }

    /**
     * Fills the vector with MediaCodecBuffer's if in array mode; otherwise,
     * no-op.
     */
    virtual void getArray(Vector<sp<MediaCodecBuffer>> *) const {}

protected:
    // Format to be used for creating MediaCodec-facing buffers.
    sp<AMessage> mFormat;

private:
    DISALLOW_EVIL_CONSTRUCTORS(Buffers);
};

class CCodecBufferChannel::InputBuffers : public CCodecBufferChannel::Buffers {
public:
    InputBuffers() = default;
    virtual ~InputBuffers() = default;

    /**
     * Set a block pool to obtain input memory blocks.
     */
    void setPool(const std::shared_ptr<C2BlockPool> &pool) { mPool = pool; }

    /**
     * Get a new MediaCodecBuffer for input and its corresponding index.
     * Returns false if no new buffer can be obtained at the moment.
     */
    virtual bool requestNewBuffer(size_t *index, sp<MediaCodecBuffer> *buffer) = 0;

    /**
     * Release the buffer obtained from requestNewBuffer() and get the
     * associated C2Buffer object back. Returns empty shared_ptr if the
     * buffer is not on file.
     */
    virtual std::shared_ptr<C2Buffer> releaseBuffer(const sp<MediaCodecBuffer> &buffer) = 0;

    /**
     * Flush internal state. After this call, no index or buffer previously
     * returned from requestNewBuffer() is valid.
     */
    virtual void flush() = 0;

    /**
     * Return array-backed version of input buffers. The returned object
     * shall retain the internal state so that it will honor index and
     * buffer from previous calls of requestNewBuffer().
     */
    virtual std::unique_ptr<InputBuffers> toArrayMode() = 0;

protected:
    // Pool to obtain blocks for input buffers.
    std::shared_ptr<C2BlockPool> mPool;

private:
    DISALLOW_EVIL_CONSTRUCTORS(InputBuffers);
};

class CCodecBufferChannel::OutputBuffers : public CCodecBufferChannel::Buffers {
public:
    OutputBuffers() = default;
    virtual ~OutputBuffers() = default;

    /**
     * Register output C2Buffer from the component and obtain corresponding
     * index and MediaCodecBuffer object. Returns false if registration
     * fails.
     */
    virtual bool registerBuffer(
            const std::shared_ptr<C2Buffer> &buffer,
            size_t *index,
            sp<MediaCodecBuffer> *clientBuffer) = 0;

    /**
     * Register codec specific data as a buffer to be consistent with
     * MediaCodec behavior.
     */
    virtual bool registerCsd(
            const C2StreamCsdInfo::output * /* csd */,
            size_t * /* index */,
            sp<MediaCodecBuffer> * /* clientBuffer */) = 0;

    /**
     * Release the buffer obtained from registerBuffer() and get the
     * associated C2Buffer object back. Returns empty shared_ptr if the
     * buffer is not on file.
     */
    virtual std::shared_ptr<C2Buffer> releaseBuffer(const sp<MediaCodecBuffer> &buffer) = 0;

    /**
     * Flush internal state. After this call, no index or buffer previously
     * returned from registerBuffer() is valid.
     */
    virtual void flush(const std::list<std::unique_ptr<C2Work>> &flushedWork) = 0;

    /**
     * Return array-backed version of output buffers. The returned object
     * shall retain the internal state so that it will honor index and
     * buffer from previous calls of registerBuffer().
     */
    virtual std::unique_ptr<OutputBuffers> toArrayMode() = 0;

private:
    DISALLOW_EVIL_CONSTRUCTORS(OutputBuffers);
};

class CCodecBufferChannel::OutputBufferQueue {
public:
    OutputBufferQueue() : mIgbp(nullptr), mIgbpId(0) {}
    bool isNewIgbp(sp<IGraphicBufferProducer> igbp) {
        if (!igbp || igbp == mIgbp) return false;
        mIgbp = igbp;
        mIgbp->getUniqueId(&mIgbpId);
        mIgbp->setGenerationNumber(0);
        mIgbp->setMaxDequeuedBufferCount(16); // TODO: tune
        return true;
    }

    bool isNewIgbpId(uint64_t igbpId) {
        return igbpId != mIgbpId;
    }

private:
    sp<IGraphicBufferProducer> mIgbp;
    uint64_t mIgbpId;
};

namespace {

// TODO: get this info from component
const static size_t kMinBufferArraySize = 16;
const static size_t kLinearBufferSize = 524288;
const static size_t kMaxGraphicBufferRefCount = 4;

/**
 * Simple local buffer pool backed by std::vector.
 */
class LocalBufferPool : public std::enable_shared_from_this<LocalBufferPool> {
public:
    /**
     * Create a new LocalBufferPool object.
     *
     * \param poolCapacity  max total size of buffers managed by this pool.
     *
     * \return  a newly created pool object.
     */
    static std::shared_ptr<LocalBufferPool> Create(size_t poolCapacity) {
        return std::shared_ptr<LocalBufferPool>(new LocalBufferPool(poolCapacity));
    }

    /**
     * Return an ABuffer object whose size is at least |capacity|.
     *
     * \param   capacity  requested capacity
     * \return  nullptr if the pool capacity is reached
     *          an ABuffer object otherwise.
     */
    sp<ABuffer> newBuffer(size_t capacity) {
        Mutex::Autolock lock(mMutex);
        auto it = std::find_if(
                mPool.begin(), mPool.end(),
                [capacity](const std::vector<uint8_t> &vec) {
                    return vec.capacity() >= capacity;
                });
        if (it != mPool.end()) {
            sp<ABuffer> buffer = new VectorBuffer(std::move(*it), shared_from_this());
            mPool.erase(it);
            return buffer;
        }
        if (mUsedSize + capacity > mPoolCapacity) {
            while (!mPool.empty()) {
                mUsedSize -= mPool.back().capacity();
                mPool.pop_back();
            }
            if (mUsedSize + capacity > mPoolCapacity) {
                ALOGD("mUsedSize = %zu, capacity = %zu, mPoolCapacity = %zu",
                        mUsedSize, capacity, mPoolCapacity);
                return nullptr;
            }
        }
        std::vector<uint8_t> vec(capacity);
        mUsedSize += vec.capacity();
        return new VectorBuffer(std::move(vec), shared_from_this());
    }

private:
    /**
     * ABuffer backed by std::vector.
     */
    class VectorBuffer : public ::android::ABuffer {
    public:
        /**
         * Construct a VectorBuffer by taking the ownership of supplied vector.
         *
         * \param vec   backing vector of the buffer. this object takes
         *              ownership at construction.
         * \param pool  a LocalBufferPool object to return the vector at
         *              destruction.
         */
        VectorBuffer(std::vector<uint8_t> &&vec, const std::shared_ptr<LocalBufferPool> &pool)
            : ABuffer(vec.data(), vec.capacity()),
              mVec(std::move(vec)),
              mPool(pool) {
        }

        ~VectorBuffer() override {
            std::shared_ptr<LocalBufferPool> pool = mPool.lock();
            if (pool) {
                // If pool is alive, return the vector back to the pool so that
                // it can be recycled.
                pool->returnVector(std::move(mVec));
            }
        }

    private:
        std::vector<uint8_t> mVec;
        std::weak_ptr<LocalBufferPool> mPool;
    };

    Mutex mMutex;
    size_t mPoolCapacity;
    size_t mUsedSize;
    std::list<std::vector<uint8_t>> mPool;

    /**
     * Private constructor to prevent constructing non-managed LocalBufferPool.
     */
    explicit LocalBufferPool(size_t poolCapacity)
        : mPoolCapacity(poolCapacity), mUsedSize(0) {
    }

    /**
     * Take back the ownership of vec from the destructed VectorBuffer and put
     * it in front of the pool.
     */
    void returnVector(std::vector<uint8_t> &&vec) {
        Mutex::Autolock lock(mMutex);
        mPool.push_front(std::move(vec));
    }

    DISALLOW_EVIL_CONSTRUCTORS(LocalBufferPool);
};

sp<GraphicBlockBuffer> AllocateGraphicBuffer(
        const std::shared_ptr<C2BlockPool> &pool,
        const sp<AMessage> &format,
        uint32_t pixelFormat,
        const C2MemoryUsage &usage,
        const std::shared_ptr<LocalBufferPool> &localBufferPool) {
    int32_t width, height;
    if (!format->findInt32("width", &width) || !format->findInt32("height", &height)) {
        ALOGD("format lacks width or height");
        return nullptr;
    }

    std::shared_ptr<C2GraphicBlock> block;
    c2_status_t err = pool->fetchGraphicBlock(
            width, height, pixelFormat, usage, &block);
    if (err != C2_OK) {
        ALOGD("fetch graphic block failed: %d", err);
        return nullptr;
    }

    return GraphicBlockBuffer::Allocate(
            format,
            block,
            [localBufferPool](size_t capacity) {
                return localBufferPool->newBuffer(capacity);
            });
}

class BuffersArrayImpl;

/**
 * Flexible buffer slots implementation.
 */
class FlexBuffersImpl {
public:
    FlexBuffersImpl() = default;

    /**
     * Assign an empty slot for a buffer and return the index. If there's no
     * empty slot, just add one at the end and return it.
     *
     * \param buffer[in]  a new buffer to assign a slot.
     * \return            index of the assigned slot.
     */
    size_t assignSlot(const sp<Codec2Buffer> &buffer) {
        for (size_t i = 0; i < mBuffers.size(); ++i) {
            if (mBuffers[i].clientBuffer == nullptr
                    && mBuffers[i].compBuffer.expired()) {
                mBuffers[i].clientBuffer = buffer;
                return i;
            }
        }
        mBuffers.push_back({ buffer, std::weak_ptr<C2Buffer>() });
        return mBuffers.size() - 1;
    }

    /**
     * Release the slot from the client, and get the C2Buffer object back from
     * the previously assigned buffer. Note that the slot is not completely free
     * until the returned C2Buffer object is freed.
     *
     * \param buffer[in]  the buffer previously assigned a slot.
     * \return            C2Buffer object from |buffer|.
     */
    std::shared_ptr<C2Buffer> releaseSlot(const sp<MediaCodecBuffer> &buffer) {
        sp<Codec2Buffer> c2Buffer;
        size_t index = mBuffers.size();
        for (size_t i = 0; i < mBuffers.size(); ++i) {
            if (mBuffers[i].clientBuffer == buffer) {
                c2Buffer = mBuffers[i].clientBuffer;
                mBuffers[i].clientBuffer.clear();
                index = i;
                break;
            }
        }
        if (c2Buffer == nullptr) {
            ALOGD("No matching buffer found");
            return nullptr;
        }
        std::shared_ptr<C2Buffer> result = c2Buffer->asC2Buffer();
        mBuffers[index].compBuffer = result;
        return result;
    }

private:
    friend class BuffersArrayImpl;

    struct Entry {
        sp<Codec2Buffer> clientBuffer;
        std::weak_ptr<C2Buffer> compBuffer;
    };
    std::vector<Entry> mBuffers;
};

/**
 * Static buffer slots implementation based on a fixed-size array.
 */
class BuffersArrayImpl {
public:
    /**
     * Initialize buffer array from the original |impl|. The buffers known by
     * the client is preserved, and the empty slots are populated so that the
     * array size is at least |minSize|.
     *
     * \param impl[in]      FlexBuffersImpl object used so far.
     * \param minSize[in]   minimum size of the buffer array.
     * \param allocate[in]  function to allocate a client buffer for an empty slot.
     */
    void initialize(
            const FlexBuffersImpl &impl,
            size_t minSize,
            std::function<sp<Codec2Buffer>()> allocate) {
        for (size_t i = 0; i < impl.mBuffers.size(); ++i) {
            sp<Codec2Buffer> clientBuffer = impl.mBuffers[i].clientBuffer;
            bool ownedByClient = (clientBuffer != nullptr);
            if (!ownedByClient) {
                clientBuffer = allocate();
            }
            mBuffers.push_back({ clientBuffer, impl.mBuffers[i].compBuffer, ownedByClient });
        }
        for (size_t i = impl.mBuffers.size(); i < minSize; ++i) {
            mBuffers.push_back({ allocate(), std::weak_ptr<C2Buffer>(), false });
        }
    }

    /**
     * Grab a buffer from the underlying array which matches the criteria.
     *
     * \param index[out]    index of the slot.
     * \param buffer[out]   the matching buffer.
     * \param match[in]     a function to test whether the buffer matches the
     *                      criteria or not.
     * \return OK           if successful,
     *         NO_MEMORY    if there's no available slot meets the criteria.
     */
    status_t grabBuffer(
            size_t *index,
            sp<Codec2Buffer> *buffer,
            std::function<bool(const sp<Codec2Buffer> &)> match =
                [](const sp<Codec2Buffer> &) { return true; }) {
        for (size_t i = 0; i < mBuffers.size(); ++i) {
            if (!mBuffers[i].ownedByClient && mBuffers[i].compBuffer.expired()
                    && match(mBuffers[i].clientBuffer)) {
                mBuffers[i].ownedByClient = true;
                *buffer = mBuffers[i].clientBuffer;
                (*buffer)->meta()->clear();
                (*buffer)->setRange(0, (*buffer)->capacity());
                *index = i;
                return OK;
            }
        }
        return NO_MEMORY;
    }

    /**
     * Return the buffer from the client, and get the C2Buffer object back from
     * the buffer. Note that the slot is not completely free until the returned
     * C2Buffer object is freed.
     *
     * \param buffer[in]  the buffer previously grabbed.
     * \return            C2Buffer object from |buffer|.
     */
    std::shared_ptr<C2Buffer> returnBuffer(const sp<MediaCodecBuffer> &buffer) {
        sp<Codec2Buffer> c2Buffer;
        size_t index = mBuffers.size();
        for (size_t i = 0; i < mBuffers.size(); ++i) {
            if (mBuffers[i].clientBuffer == buffer) {
                if (!mBuffers[i].ownedByClient) {
                    ALOGD("Client returned a buffer it does not own according to our record: %zu", i);
                }
                c2Buffer = mBuffers[i].clientBuffer;
                mBuffers[i].ownedByClient = false;
                index = i;
                break;
            }
        }
        if (c2Buffer == nullptr) {
            ALOGD("No matching buffer found");
            return nullptr;
        }
        std::shared_ptr<C2Buffer> result = c2Buffer->asC2Buffer();
        mBuffers[index].compBuffer = result;
        return result;
    }

    /**
     * Populate |array| with the underlying buffer array.
     *
     * \param array[out]  an array to be filled with the underlying buffer array.
     */
    void getArray(Vector<sp<MediaCodecBuffer>> *array) const {
        array->clear();
        for (const Entry &entry : mBuffers) {
            array->push(entry.clientBuffer);
        }
    }

    /**
     * The client abandoned all known buffers, so reclaim the ownership.
     */
    void flush() {
        for (Entry &entry : mBuffers) {
            entry.ownedByClient = false;
        }
    }

private:
    struct Entry {
        const sp<Codec2Buffer> clientBuffer;
        std::weak_ptr<C2Buffer> compBuffer;
        bool ownedByClient;
    };
    std::vector<Entry> mBuffers;
};

class InputBuffersArray : public CCodecBufferChannel::InputBuffers {
public:
    InputBuffersArray() = default;
    ~InputBuffersArray() override = default;

    void initialize(
            const FlexBuffersImpl &impl,
            size_t minSize,
            std::function<sp<Codec2Buffer>()> allocate) {
        mImpl.initialize(impl, minSize, allocate);
    }

    bool isArrayMode() const final { return true; }

    std::unique_ptr<CCodecBufferChannel::InputBuffers> toArrayMode() final {
        return nullptr;
    }

    void getArray(Vector<sp<MediaCodecBuffer>> *array) const final {
        mImpl.getArray(array);
    }

    bool requestNewBuffer(size_t *index, sp<MediaCodecBuffer> *buffer) override {
        sp<Codec2Buffer> c2Buffer;
        status_t err = mImpl.grabBuffer(index, &c2Buffer);
        if (err == OK) {
            c2Buffer->setFormat(mFormat);
            *buffer = c2Buffer;
            return true;
        }
        return false;
    }

    std::shared_ptr<C2Buffer> releaseBuffer(const sp<MediaCodecBuffer> &buffer) override {
        return mImpl.returnBuffer(buffer);
    }

    void flush() override {
        mImpl.flush();
    }

private:
    BuffersArrayImpl mImpl;
};

class LinearInputBuffers : public CCodecBufferChannel::InputBuffers {
public:
    using CCodecBufferChannel::InputBuffers::InputBuffers;

    bool requestNewBuffer(size_t *index, sp<MediaCodecBuffer> *buffer) override {
        int32_t capacity = kLinearBufferSize;
        (void)mFormat->findInt32(C2_NAME_STREAM_MAX_BUFFER_SIZE_SETTING, &capacity);
        // TODO: proper max input size
        // TODO: read usage from intf
        sp<Codec2Buffer> newBuffer = alloc((size_t)capacity);
        if (newBuffer == nullptr) {
            return false;
        }
        *index = mImpl.assignSlot(newBuffer);
        *buffer = newBuffer;
        return true;
    }

    std::shared_ptr<C2Buffer> releaseBuffer(const sp<MediaCodecBuffer> &buffer) override {
        return mImpl.releaseSlot(buffer);
    }

    void flush() override {
        // This is no-op by default unless we're in array mode where we need to keep
        // track of the flushed work.
    }

    std::unique_ptr<CCodecBufferChannel::InputBuffers> toArrayMode() final {
        std::unique_ptr<InputBuffersArray> array(new InputBuffersArray);
        array->setFormat(mFormat);
        array->initialize(
                mImpl,
                kMinBufferArraySize,
                [this] () -> sp<Codec2Buffer> { return alloc(kLinearBufferSize); });
        return std::move(array);
    }

    virtual sp<Codec2Buffer> alloc(size_t size) const {
        C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
        std::shared_ptr<C2LinearBlock> block;

        c2_status_t err = mPool->fetchLinearBlock(size, usage, &block);
        if (err != C2_OK) {
            return nullptr;
        }

        return LinearBlockBuffer::Allocate(mFormat, block);
    }

private:
    FlexBuffersImpl mImpl;
};

class EncryptedLinearInputBuffers : public LinearInputBuffers {
public:
    EncryptedLinearInputBuffers(
            bool secure,
            const sp<MemoryDealer> &dealer,
            const sp<ICrypto> &crypto,
            int32_t heapSeqNum)
        : mUsage({0, 0}),
          mDealer(dealer),
          mCrypto(crypto),
          mHeapSeqNum(heapSeqNum) {
        if (secure) {
            mUsage = { C2MemoryUsage::READ_PROTECTED, 0 };
        } else {
            mUsage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
        }
        for (size_t i = 0; i < kMinBufferArraySize; ++i) {
            sp<IMemory> memory = mDealer->allocate(kLinearBufferSize);
            if (memory == nullptr) {
                ALOGD("Failed to allocate memory from dealer: only %zu slots allocated", i);
                break;
            }
            mMemoryVector.push_back({std::weak_ptr<C2LinearBlock>(), memory});
        }
    }

    ~EncryptedLinearInputBuffers() override {
    }

    sp<Codec2Buffer> alloc(size_t size) const override {
        sp<IMemory> memory;
        for (const Entry &entry : mMemoryVector) {
            if (entry.block.expired()) {
                memory = entry.memory;
                break;
            }
        }
        if (memory == nullptr) {
            return nullptr;
        }

        std::shared_ptr<C2LinearBlock> block;
        c2_status_t err = mPool->fetchLinearBlock(size, mUsage, &block);
        if (err != C2_OK) {
            return nullptr;
        }

        return new EncryptedLinearBlockBuffer(mFormat, block, memory, mHeapSeqNum);
    }

private:
    C2MemoryUsage mUsage;
    sp<MemoryDealer> mDealer;
    sp<ICrypto> mCrypto;
    int32_t mHeapSeqNum;
    struct Entry {
        std::weak_ptr<C2LinearBlock> block;
        sp<IMemory> memory;
    };
    std::vector<Entry> mMemoryVector;
};

class GraphicInputBuffers : public CCodecBufferChannel::InputBuffers {
public:
    GraphicInputBuffers() : mLocalBufferPool(LocalBufferPool::Create(1920 * 1080 * 16)) {}
    ~GraphicInputBuffers() override = default;

    bool requestNewBuffer(size_t *index, sp<MediaCodecBuffer> *buffer) override {
        // TODO: proper max input size
        // TODO: read usage from intf
        C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
        sp<GraphicBlockBuffer> newBuffer = AllocateGraphicBuffer(
                mPool, mFormat, HAL_PIXEL_FORMAT_YV12, usage, mLocalBufferPool);
        if (newBuffer == nullptr) {
            return false;
        }
        *index = mImpl.assignSlot(newBuffer);
        *buffer = newBuffer;
        return true;
    }

    std::shared_ptr<C2Buffer> releaseBuffer(const sp<MediaCodecBuffer> &buffer) override {
        return mImpl.releaseSlot(buffer);
    }

    void flush() override {
        // This is no-op by default unless we're in array mode where we need to keep
        // track of the flushed work.
    }

    std::unique_ptr<CCodecBufferChannel::InputBuffers> toArrayMode() final {
        std::unique_ptr<InputBuffersArray> array(new InputBuffersArray);
        array->setFormat(mFormat);
        array->initialize(
                mImpl,
                kMinBufferArraySize,
                [pool = mPool, format = mFormat, lbp = mLocalBufferPool]() -> sp<Codec2Buffer> {
                    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
                    return AllocateGraphicBuffer(
                            pool, format, HAL_PIXEL_FORMAT_YV12, usage, lbp);
                });
        return std::move(array);
    }

private:
    FlexBuffersImpl mImpl;
    std::shared_ptr<LocalBufferPool> mLocalBufferPool;
};

class DummyInputBuffers : public CCodecBufferChannel::InputBuffers {
public:
    DummyInputBuffers() = default;

    bool requestNewBuffer(size_t *, sp<MediaCodecBuffer> *) override {
        return false;
    }

    std::shared_ptr<C2Buffer> releaseBuffer(const sp<MediaCodecBuffer> &) override {
        return nullptr;
    }

    void flush() override {
    }

    std::unique_ptr<CCodecBufferChannel::InputBuffers> toArrayMode() final {
        return nullptr;
    }

    bool isArrayMode() const final { return true; }

    void getArray(Vector<sp<MediaCodecBuffer>> *array) const final {
        array->clear();
    }
};

class OutputBuffersArray : public CCodecBufferChannel::OutputBuffers {
public:
    OutputBuffersArray() = default;
    ~OutputBuffersArray() override = default;

    void initialize(
            const FlexBuffersImpl &impl,
            size_t minSize,
            std::function<sp<Codec2Buffer>()> allocate) {
        mImpl.initialize(impl, minSize, allocate);
    }

    bool isArrayMode() const final { return true; }

    std::unique_ptr<CCodecBufferChannel::OutputBuffers> toArrayMode() final {
        return nullptr;
    }

    bool registerBuffer(
            const std::shared_ptr<C2Buffer> &buffer,
            size_t *index,
            sp<MediaCodecBuffer> *clientBuffer) final {
        sp<Codec2Buffer> c2Buffer;
        status_t err = mImpl.grabBuffer(
                index,
                &c2Buffer,
                [buffer](const sp<Codec2Buffer> &clientBuffer) {
                    return clientBuffer->canCopy(buffer);
                });
        if (err != OK) {
            ALOGD("grabBuffer failed: %d", err);
            return false;
        }
        c2Buffer->setFormat(mFormat);
        if (!c2Buffer->copy(buffer)) {
            ALOGD("copy buffer failed");
            return false;
        }
        *clientBuffer = c2Buffer;
        return true;
    }

    bool registerCsd(
            const C2StreamCsdInfo::output *csd,
            size_t *index,
            sp<MediaCodecBuffer> *clientBuffer) final {
        sp<Codec2Buffer> c2Buffer;
        status_t err = mImpl.grabBuffer(
                index,
                &c2Buffer,
                [csd](const sp<Codec2Buffer> &clientBuffer) {
                    return clientBuffer->base() != nullptr
                            && clientBuffer->capacity() >= csd->flexCount();
                });
        if (err != OK) {
            return false;
        }
        // TODO: proper format update
        sp<ABuffer> csdBuffer = ABuffer::CreateAsCopy(csd->m.value, csd->flexCount());
        mFormat = mFormat->dup();
        mFormat->setBuffer("csd-0", csdBuffer);

        memcpy(c2Buffer->base(), csd->m.value, csd->flexCount());
        c2Buffer->setRange(0, csd->flexCount());
        c2Buffer->setFormat(mFormat);
        *clientBuffer = c2Buffer;
        return true;
    }

    std::shared_ptr<C2Buffer> releaseBuffer(const sp<MediaCodecBuffer> &buffer) final {
        return mImpl.returnBuffer(buffer);
    }

    void flush(const std::list<std::unique_ptr<C2Work>> &flushedWork) override {
        (void) flushedWork;
        mImpl.flush();
    }

    void getArray(Vector<sp<MediaCodecBuffer>> *array) const final {
        mImpl.getArray(array);
    }

private:
    BuffersArrayImpl mImpl;
};

class FlexOutputBuffers : public CCodecBufferChannel::OutputBuffers {
public:
    using CCodecBufferChannel::OutputBuffers::OutputBuffers;

    bool registerBuffer(
            const std::shared_ptr<C2Buffer> &buffer,
            size_t *index,
            sp<MediaCodecBuffer> *clientBuffer) override {
        sp<Codec2Buffer> newBuffer = wrap(buffer);
        newBuffer->setFormat(mFormat);
        *index = mImpl.assignSlot(newBuffer);
        *clientBuffer = newBuffer;
        return true;
    }

    bool registerCsd(
            const C2StreamCsdInfo::output *csd,
            size_t *index,
            sp<MediaCodecBuffer> *clientBuffer) final {
        // TODO: proper format update
        sp<ABuffer> csdBuffer = ABuffer::CreateAsCopy(csd->m.value, csd->flexCount());
        mFormat = mFormat->dup();
        mFormat->setBuffer("csd-0", csdBuffer);

        sp<Codec2Buffer> newBuffer = new LocalLinearBuffer(mFormat, csdBuffer);
        *index = mImpl.assignSlot(newBuffer);
        *clientBuffer = newBuffer;
        return true;
    }

    std::shared_ptr<C2Buffer> releaseBuffer(
            const sp<MediaCodecBuffer> &buffer) override {
        return mImpl.releaseSlot(buffer);
    }

    void flush(
            const std::list<std::unique_ptr<C2Work>> &flushedWork) override {
        (void) flushedWork;
        // This is no-op by default unless we're in array mode where we need to keep
        // track of the flushed work.
    }

    std::unique_ptr<CCodecBufferChannel::OutputBuffers> toArrayMode() override {
        std::unique_ptr<OutputBuffersArray> array(new OutputBuffersArray);
        array->setFormat(mFormat);
        array->initialize(
                mImpl,
                kMinBufferArraySize,
                [this]() { return allocateArrayBuffer(); });
        return std::move(array);
    }

    /**
     * Return an appropriate Codec2Buffer object for the type of buffers.
     *
     * \param buffer  C2Buffer object to wrap.
     *
     * \return  appropriate Codec2Buffer object to wrap |buffer|.
     */
    virtual sp<Codec2Buffer> wrap(const std::shared_ptr<C2Buffer> &buffer) = 0;

    /**
     * Return an appropriate Codec2Buffer object for the type of buffers, to be
     * used as an empty array buffer.
     *
     * \return  appropriate Codec2Buffer object which can copy() from C2Buffers.
     */
    virtual sp<Codec2Buffer> allocateArrayBuffer() = 0;

private:
    FlexBuffersImpl mImpl;
};

class LinearOutputBuffers : public FlexOutputBuffers {
public:
    using FlexOutputBuffers::FlexOutputBuffers;

    sp<Codec2Buffer> wrap(const std::shared_ptr<C2Buffer> &buffer) override {
        if (buffer == nullptr) {
            return new DummyContainerBuffer(mFormat, buffer);
        }
        if (buffer->data().type() != C2BufferData::LINEAR) {
            // We expect linear output buffers from the component.
            return nullptr;
        }
        if (buffer->data().linearBlocks().size() != 1u) {
            // We expect one and only one linear block from the component.
            return nullptr;
        }
        return ConstLinearBlockBuffer::Allocate(mFormat, buffer);
    }

    sp<Codec2Buffer> allocateArrayBuffer() override {
        // TODO: proper max output size
        return new LocalLinearBuffer(mFormat, new ABuffer(kLinearBufferSize));
    }
};

class GraphicOutputBuffers : public FlexOutputBuffers {
public:
    using FlexOutputBuffers::FlexOutputBuffers;

    sp<Codec2Buffer> wrap(const std::shared_ptr<C2Buffer> &buffer) override {
        return new DummyContainerBuffer(mFormat, buffer);
    }

    sp<Codec2Buffer> allocateArrayBuffer() override {
        return new DummyContainerBuffer(mFormat);
    }
};

class RawGraphicOutputBuffers : public FlexOutputBuffers {
public:
    RawGraphicOutputBuffers()
        : mLocalBufferPool(LocalBufferPool::Create(1920 * 1080 * 16)) {
    }
    ~RawGraphicOutputBuffers() override = default;

    sp<Codec2Buffer> wrap(const std::shared_ptr<C2Buffer> &buffer) override {
        return ConstGraphicBlockBuffer::Allocate(
                mFormat,
                buffer,
                [lbp = mLocalBufferPool](size_t capacity) {
                    return lbp->newBuffer(capacity);
                });
    }

    sp<Codec2Buffer> allocateArrayBuffer() override {
        return ConstGraphicBlockBuffer::AllocateEmpty(
                mFormat,
                [lbp = mLocalBufferPool](size_t capacity) {
                    return lbp->newBuffer(capacity);
                });
    }

private:
    std::shared_ptr<LocalBufferPool> mLocalBufferPool;
};

}  // namespace

CCodecBufferChannel::QueueGuard::QueueGuard(
        CCodecBufferChannel::QueueSync &sync) : mSync(sync) {
    std::unique_lock<std::mutex> l(mSync.mMutex);
    // At this point it's guaranteed that mSync is not under state transition,
    // as we are holding its mutex.
    if (mSync.mCount == -1) {
        mRunning = false;
    } else {
        ++mSync.mCount;
        mRunning = true;
    }
}

CCodecBufferChannel::QueueGuard::~QueueGuard() {
    if (mRunning) {
        // We are not holding mutex at this point so that QueueSync::stop() can
        // keep holding the lock until mCount reaches zero.
        --mSync.mCount;
    }
}

void CCodecBufferChannel::QueueSync::start() {
    std::unique_lock<std::mutex> l(mMutex);
    // If stopped, it goes to running state; otherwise no-op.
    int32_t expected = -1;
    (void)mCount.compare_exchange_strong(expected, 0);
}

void CCodecBufferChannel::QueueSync::stop() {
    std::unique_lock<std::mutex> l(mMutex);
    if (mCount == -1) {
        // no-op
        return;
    }
    // Holding mutex here blocks creation of additional QueueGuard objects, so
    // mCount can only decrement. In other words, threads that acquired the lock
    // are allowed to finish execution but additional threads trying to acquire
    // the lock at this point will block, and then get QueueGuard at STOPPED
    // state.
    int32_t expected = 0;
    while (!mCount.compare_exchange_weak(expected, -1)) {
        std::this_thread::yield();
    }
}

CCodecBufferChannel::CCodecBufferChannel(
        const std::function<void(status_t, enum ActionCode)> &onError)
    : mHeapSeqNum(-1),
      mOnError(onError),
      mFrameIndex(0u),
      mFirstValidFrameIndex(0u),
      mOutputBufferQueue(new OutputBufferQueue()) {
}

CCodecBufferChannel::~CCodecBufferChannel() {
    if (mCrypto != nullptr && mDealer != nullptr && mHeapSeqNum >= 0) {
        mCrypto->unsetHeap(mHeapSeqNum);
    }
}

void CCodecBufferChannel::setComponent(
        const std::shared_ptr<Codec2Client::Component> &component) {
    mComponent = component;
}

status_t CCodecBufferChannel::setInputSurface(
        const std::shared_ptr<InputSurfaceWrapper> &surface) {
    ALOGV("setInputSurface");
    mInputSurface = surface;
    return OK;
}

status_t CCodecBufferChannel::queueInputBufferInternal(const sp<MediaCodecBuffer> &buffer) {
    int64_t timeUs;
    CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

    int32_t flags = 0;
    int32_t tmp = 0;
    if (buffer->meta()->findInt32("eos", &tmp) && tmp) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        ALOGV("input EOS");
    }
    if (buffer->meta()->findInt32("csd", &tmp) && tmp) {
        flags |= C2FrameData::FLAG_CODEC_CONFIG;
    }
    ALOGV("queueInputBuffer: buffer->size() = %zu", buffer->size());
    std::unique_ptr<C2Work> work(new C2Work);
    work->input.flags = (C2FrameData::flags_t)flags;
    work->input.ordinal.timestamp = timeUs;
    work->input.ordinal.frameIndex = mFrameIndex++;
    work->input.buffers.clear();
    {
        Mutexed<std::unique_ptr<InputBuffers>>::Locked buffers(mInputBuffers);
        std::shared_ptr<C2Buffer> c2buffer = (*buffers)->releaseBuffer(buffer);
        if (!c2buffer) {
            return -ENOENT;
        }
        work->input.buffers.push_back(c2buffer);

        // TODO: Use BufferPool
        Mutexed<InputRefs>::Locked inputRefs(mInputRefs);
        inputRefs->bufferRefs.emplace(work->input.ordinal.frameIndex.peekull(), c2buffer);
    }
    // TODO: fill info's

    work->worklets.clear();
    work->worklets.emplace_back(new C2Worklet);

    std::list<std::unique_ptr<C2Work>> items;
    items.push_back(std::move(work));
    return mComponent->queue(&items);
}

status_t CCodecBufferChannel::queueInputBuffer(const sp<MediaCodecBuffer> &buffer) {
    QueueGuard guard(mSync);
    if (!guard.isRunning()) {
        ALOGW("No more buffers should be queued at current state.");
        return -ENOSYS;
    }
    return queueInputBufferInternal(buffer);
}

status_t CCodecBufferChannel::queueSecureInputBuffer(
        const sp<MediaCodecBuffer> &buffer, bool secure, const uint8_t *key,
        const uint8_t *iv, CryptoPlugin::Mode mode, CryptoPlugin::Pattern pattern,
        const CryptoPlugin::SubSample *subSamples, size_t numSubSamples,
        AString *errorDetailMsg) {
    QueueGuard guard(mSync);
    if (!guard.isRunning()) {
        ALOGW("No more buffers should be queued at current state.");
        return -ENOSYS;
    }

    if (!hasCryptoOrDescrambler()) {
        return -ENOSYS;
    }
    sp<EncryptedLinearBlockBuffer> encryptedBuffer((EncryptedLinearBlockBuffer *)buffer.get());

    ssize_t result = -1;
    if (mCrypto != nullptr) {
        ICrypto::DestinationBuffer destination;
        if (secure) {
            destination.mType = ICrypto::kDestinationTypeNativeHandle;
            destination.mHandle = encryptedBuffer->handle();
        } else {
            destination.mType = ICrypto::kDestinationTypeSharedMemory;
            destination.mSharedMemory = mDecryptDestination;
        }
        ICrypto::SourceBuffer source;
        encryptedBuffer->fillSourceBuffer(&source);
        result = mCrypto->decrypt(
                key, iv, mode, pattern, source, buffer->offset(),
                subSamples, numSubSamples, destination, errorDetailMsg);
        if (result < 0) {
            return result;
        }
        if (destination.mType == ICrypto::kDestinationTypeSharedMemory) {
            encryptedBuffer->copyDecryptedContent(mDecryptDestination, result);
        }
    } else {
        // Here we cast CryptoPlugin::SubSample to hardware::cas::native::V1_0::SubSample
        // directly, the structure definitions should match as checked in DescramblerImpl.cpp.
        hidl_vec<SubSample> hidlSubSamples;
        hidlSubSamples.setToExternal((SubSample *)subSamples, numSubSamples, false /*own*/);

        hardware::cas::native::V1_0::SharedBuffer srcBuffer;
        encryptedBuffer->fillSourceBuffer(&srcBuffer);

        DestinationBuffer dstBuffer;
        if (secure) {
            dstBuffer.type = BufferType::NATIVE_HANDLE;
            dstBuffer.secureMemory = hidl_handle(encryptedBuffer->handle());
        } else {
            dstBuffer.type = BufferType::SHARED_MEMORY;
            dstBuffer.nonsecureMemory = srcBuffer;
        }

        CasStatus status = CasStatus::OK;
        hidl_string detailedError;

        auto returnVoid = mDescrambler->descramble(
                key != NULL ? (ScramblingControl)key[0] : ScramblingControl::UNSCRAMBLED,
                hidlSubSamples,
                srcBuffer,
                0,
                dstBuffer,
                0,
                [&status, &result, &detailedError] (
                        CasStatus _status, uint32_t _bytesWritten,
                        const hidl_string& _detailedError) {
                    status = _status;
                    result = (ssize_t)_bytesWritten;
                    detailedError = _detailedError;
                });

        if (!returnVoid.isOk() || status != CasStatus::OK || result < 0) {
            ALOGE("descramble failed, trans=%s, status=%d, result=%zd",
                    returnVoid.description().c_str(), status, result);
            return UNKNOWN_ERROR;
        }

        ALOGV("descramble succeeded, %zd bytes", result);

        if (dstBuffer.type == BufferType::SHARED_MEMORY) {
            encryptedBuffer->copyDecryptedContentFromMemory(result);
        }
    }

    buffer->setRange(0, result);
    return queueInputBufferInternal(buffer);
}

void CCodecBufferChannel::feedInputBufferIfAvailable() {
    sp<MediaCodecBuffer> inBuffer;
    size_t index;
    {
        Mutexed<std::unique_ptr<InputBuffers>>::Locked buffers(mInputBuffers);
        if (!(*buffers)->requestNewBuffer(&index, &inBuffer)) {
            ALOGV("no new buffer available");
            inBuffer = nullptr;
            return;
        }
    }
    ALOGV("new input index = %zu", index);
    mCallback->onInputBufferAvailable(index, inBuffer);
}

status_t CCodecBufferChannel::renderOutputBuffer(
        const sp<MediaCodecBuffer> &buffer, int64_t timestampNs) {
    ALOGV("renderOutputBuffer");
    feedInputBufferIfAvailable();

    std::shared_ptr<C2Buffer> c2Buffer;
    {
        Mutexed<std::unique_ptr<OutputBuffers>>::Locked buffers(mOutputBuffers);
        c2Buffer = (*buffers)->releaseBuffer(buffer);
    }

    Mutexed<OutputSurface>::Locked output(mOutputSurface);
    if (output->surface == nullptr) {
        ALOGE("no surface");
        return OK;
    }
    sp<IGraphicBufferProducer> igbp = output->surface->getIGraphicBufferProducer();
    if (mOutputBufferQueue->isNewIgbp(igbp)) {
        sp<HGraphicBufferProducer> higbp = igbp->getHalInterface();
        if (!higbp) {
            higbp = new TWGraphicBufferProducer<HGraphicBufferProducer>(igbp);
        }
        // TODO: use id of block pool which is crated from component
        mComponent->setOutputSurface(C2BlockPool::PLATFORM_START, higbp);
    }

    std::vector<C2ConstGraphicBlock> blocks = c2Buffer->data().graphicBlocks();
    if (blocks.size() != 1u) {
        ALOGE("# of graphic blocks expected to be 1, but %zu", blocks.size());
        return UNKNOWN_ERROR;
    }
    const C2ConstGraphicBlock &block = blocks.front();

    native_handle_t *grallocHandle = UnwrapNativeCodec2GrallocHandle(block.handle());
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint64_t igbp_id;
    int32_t igbp_slot;
    _UnwrapNativeCodec2GrallocMetadata(
            block.handle(), &width, &height, &format, &usage, &stride,
            &igbp_id, (uint32_t *)&igbp_slot);
    ALOGV("attaching buffer (%u*%u): (%u*%u, fmt %#x, usage %#llx, stride %u)",
            block.width(),
            block.height(),
            width, height, format, (long long)usage, stride);

    status_t result = OK;
    if (mOutputBufferQueue->isNewIgbpId(igbp_id)) {
        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(
                grallocHandle,
                GraphicBuffer::CLONE_HANDLE,
                width, height, format, 1, usage, stride));
        // TODO: detach?
        graphicBuffer->setGenerationNumber(0);
        result = igbp->attachBuffer(&igbp_slot, graphicBuffer);
        if (result != OK) {
            native_handle_delete(grallocHandle);
            ALOGI("attachBuffer failed: %d", result);
            return result;
        }
        ALOGV("attach buffer from %" PRIu64 " : %d", igbp_id, igbp_slot);
    } else {
        ALOGV("dequeued buffer arrived %" PRIu64 " %d", igbp_id, igbp_slot);
    }
    native_handle_delete(grallocHandle);

    // TODO
    // revisit this after C2Fence implementation.
    // apply dataspace and scaling mode (getting scaling mode from native_window is not possible.)
    android::IGraphicBufferProducer::QueueBufferInput qbi(
            timestampNs,
            false,
            HAL_DATASPACE_UNKNOWN,
            Rect(blocks.front().width(), blocks.front().height()),
            NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW,
            0,
            Fence::NO_FENCE, 0);
    android::IGraphicBufferProducer::QueueBufferOutput qbo;
    result = igbp->queueBuffer(igbp_slot, qbi, &qbo);
    if (result != OK) {
        ALOGE("queueBuffer failed: %d", result);
        return result;
    }
    ALOGV("queue buffer successful");

    // XXX: Hack to keep C2Buffers unreleased until the consumer is done
    //      reading the content. Eventually IGBP-based C2BlockPool should handle
    //      the lifecycle.
    output->bufferRefs.push_back(c2Buffer);
    if (output->bufferRefs.size() > output->maxBufferCount + 1) {
        output->bufferRefs.pop_front();
        ALOGV("%zu buffer refs remaining", output->bufferRefs.size());
    }

    return OK;
}

status_t CCodecBufferChannel::discardBuffer(const sp<MediaCodecBuffer> &buffer) {
    ALOGV("discardBuffer: %p", buffer.get());
    {
        Mutexed<std::unique_ptr<InputBuffers>>::Locked buffers(mInputBuffers);
        (void)(*buffers)->releaseBuffer(buffer);
    }
    {
        Mutexed<std::unique_ptr<OutputBuffers>>::Locked buffers(mOutputBuffers);
        if((*buffers)->releaseBuffer(buffer)) {
            buffers.unlock();
            feedInputBufferIfAvailable();
            buffers.lock();
        }
    }
    return OK;
}

void CCodecBufferChannel::getInputBufferArray(Vector<sp<MediaCodecBuffer>> *array) {
    array->clear();
    Mutexed<std::unique_ptr<InputBuffers>>::Locked buffers(mInputBuffers);

    if (!(*buffers)->isArrayMode()) {
        *buffers = (*buffers)->toArrayMode();
    }

    (*buffers)->getArray(array);
}

void CCodecBufferChannel::getOutputBufferArray(Vector<sp<MediaCodecBuffer>> *array) {
    array->clear();
    Mutexed<std::unique_ptr<OutputBuffers>>::Locked buffers(mOutputBuffers);

    if (!(*buffers)->isArrayMode()) {
        *buffers = (*buffers)->toArrayMode();
    }

    (*buffers)->getArray(array);
}

status_t CCodecBufferChannel::start(
        const sp<AMessage> &inputFormat, const sp<AMessage> &outputFormat) {
    C2StreamFormatConfig::input iStreamFormat(0u);
    C2StreamFormatConfig::output oStreamFormat(0u);
    c2_status_t err = mComponent->query(
            { &iStreamFormat, &oStreamFormat },
            {},
            C2_DONT_BLOCK,
            nullptr);
    if (err != C2_OK) {
        return UNKNOWN_ERROR;
    }
    bool secure = mComponent->getName().find(".secure") != std::string::npos;

    if (inputFormat != nullptr) {
        Mutexed<std::unique_ptr<InputBuffers>>::Locked buffers(mInputBuffers);

        bool graphic = (iStreamFormat.value == C2FormatVideo);
        if (graphic) {
            if (mInputSurface) {
                buffers->reset(new DummyInputBuffers);
            } else {
                buffers->reset(new GraphicInputBuffers);
            }
        } else {
            if (hasCryptoOrDescrambler()) {
                if (mDealer == nullptr) {
                    mDealer = new MemoryDealer(
                            align(kLinearBufferSize, MemoryDealer::getAllocationAlignment())
                                * (kMinBufferArraySize + 1),
                            "EncryptedLinearInputBuffers");
                    mDecryptDestination = mDealer->allocate(kLinearBufferSize);
                }
                if (mCrypto != nullptr && mHeapSeqNum < 0) {
                    mHeapSeqNum = mCrypto->setHeap(mDealer->getMemoryHeap());
                } else {
                    mHeapSeqNum = -1;
                }
                buffers->reset(new EncryptedLinearInputBuffers(
                        secure, mDealer, mCrypto, mHeapSeqNum));
            } else {
                buffers->reset(new LinearInputBuffers);
            }
        }
        (*buffers)->setFormat(inputFormat);

        ALOGV("graphic = %s", graphic ? "true" : "false");
        std::shared_ptr<C2BlockPool> pool;
        if (graphic) {
            err = mComponent->getLocalBlockPool(
                    C2BlockPool::BASIC_GRAPHIC,
                    &pool);
        } else {
            /* TODO: Use BufferPool-based BlockPool
            err = mComponent->createLocalBlockPool(
                    C2PlatformAllocatorStore::ION,
                    &pool);*/
            err = mComponent->getLocalBlockPool(
                    C2BlockPool::BASIC_LINEAR,
                    &pool);
        }
        if (err == C2_OK) {
            (*buffers)->setPool(pool);
        } else {
            // TODO: error
        }
    }

    if (outputFormat != nullptr) {
        bool hasOutputSurface = false;
        {
            Mutexed<OutputSurface>::Locked output(mOutputSurface);
            hasOutputSurface = (output->surface != nullptr);
        }

        Mutexed<std::unique_ptr<OutputBuffers>>::Locked buffers(mOutputBuffers);

        bool graphic = (oStreamFormat.value == C2FormatVideo);
        if (graphic) {
            if (hasOutputSurface) {
                buffers->reset(new GraphicOutputBuffers);
            } else {
                buffers->reset(new RawGraphicOutputBuffers);
            }
        } else {
            buffers->reset(new LinearOutputBuffers);
        }
        (*buffers)->setFormat(outputFormat);
    }

    mSync.start();
    if (mInputSurface == nullptr) {
        // TODO: use proper buffer depth instead of this random value
        for (size_t i = 0; i < kMinBufferArraySize; ++i) {
            size_t index;
            sp<MediaCodecBuffer> buffer;
            {
                Mutexed<std::unique_ptr<InputBuffers>>::Locked buffers(mInputBuffers);
                if (!(*buffers)->requestNewBuffer(&index, &buffer)) {
                    if (i == 0) {
                        ALOGE("start: cannot allocate memory at all");
                        return NO_MEMORY;
                    } else {
                        ALOGV("start: cannot allocate memory, only %zu buffers allocated", i);
                    }
                    break;
                }
            }
            if (buffer) {
                mCallback->onInputBufferAvailable(index, buffer);
            }
        }
    } else {
        (void)mInputSurface->connect(mComponent);
    }
    return OK;
}

void CCodecBufferChannel::stop() {
    mSync.stop();
    mFirstValidFrameIndex = mFrameIndex.load();
    if (mInputSurface != nullptr) {
        mInputSurface->disconnect();
        mInputSurface.reset();
    }
}

void CCodecBufferChannel::flush(const std::list<std::unique_ptr<C2Work>> &flushedWork) {
    {
        Mutexed<std::unique_ptr<InputBuffers>>::Locked buffers(mInputBuffers);
        (*buffers)->flush();
    }
    {
        Mutexed<std::unique_ptr<OutputBuffers>>::Locked buffers(mOutputBuffers);
        (*buffers)->flush(flushedWork);
    }
}

void CCodecBufferChannel::onWorkDone(const std::unique_ptr<C2Work> &work) {
    if (work->input.buffers.size() == 1) {
        // TODO: Use BufferPool
        Mutexed<InputRefs>::Locked inputRefs(mInputRefs);
        auto found = inputRefs->bufferRefs.find(work->input.ordinal.frameIndex.peekull());
        if (found != inputRefs->bufferRefs.end()) {
            ALOGV("onWorkdone: removing input ref for frame=%llu",
                    work->input.ordinal.frameIndex.peekull());
            inputRefs->bufferRefs.erase(found);
        }
    }
    if (work->result != C2_OK) {
        if (work->result == C2_NOT_FOUND) {
            // TODO: Define what flushed work's result is.
            ALOGD("flushed work; ignored.");
            return;
        }
        ALOGD("work failed to complete: %d", work->result);
        mOnError(work->result, ACTION_CODE_FATAL);
        return;
    }

    // NOTE: MediaCodec usage supposedly have only one worklet
    if (work->worklets.size() != 1u) {
        ALOGE("onWorkDone: incorrect number of worklets: %zu",
                work->worklets.size());
        mOnError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        return;
    }

    const std::unique_ptr<C2Worklet> &worklet = work->worklets.front();
    if ((worklet->output.ordinal.frameIndex - mFirstValidFrameIndex.load()).peek() < 0) {
        // Discard frames from previous generation.
        return;
    }
    std::shared_ptr<C2Buffer> buffer;
    // NOTE: MediaCodec usage supposedly have only one output stream.
    if (worklet->output.buffers.size() > 1u) {
        ALOGE("onWorkDone: incorrect number of output buffers: %zu",
                worklet->output.buffers.size());
        mOnError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        return;
    } else if (worklet->output.buffers.size() == 1u) {
        buffer = worklet->output.buffers[0];
        if (!buffer) {
            ALOGW("onWorkDone: nullptr found in buffers; ignored.");
        }
    }

    const C2StreamCsdInfo::output *csdInfo = nullptr;
    for (const std::unique_ptr<C2Param> &info : worklet->output.configUpdate) {
        if (info->coreIndex() == C2StreamCsdInfo::output::CORE_INDEX) {
            ALOGV("onWorkDone: csd found");
            csdInfo = static_cast<const C2StreamCsdInfo::output *>(info.get());
        }
    }

    int32_t flags = 0;
    if (worklet->output.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= MediaCodec::BUFFER_FLAG_EOS;
        ALOGV("onWorkDone: output EOS");
    }

    sp<MediaCodecBuffer> outBuffer;
    size_t index;
    if (csdInfo != nullptr) {
        Mutexed<std::unique_ptr<OutputBuffers>>::Locked buffers(mOutputBuffers);
        if ((*buffers)->registerCsd(csdInfo, &index, &outBuffer)) {
            outBuffer->meta()->setInt64("timeUs", worklet->output.ordinal.timestamp.peek());
            outBuffer->meta()->setInt32("flags", flags | MediaCodec::BUFFER_FLAG_CODECCONFIG);
            ALOGV("onWorkDone: csd index = %zu", index);

            buffers.unlock();
            mCallback->onOutputBufferAvailable(index, outBuffer);
            buffers.lock();
        } else {
            ALOGE("onWorkDone: unable to register csd");
            buffers.unlock();
            mOnError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
            buffers.lock();
            return;
        }
    }

    if (!buffer && !flags) {
        ALOGV("onWorkDone: Not reporting output buffer");
        return;
    }

    if (buffer) {
        for (const std::shared_ptr<const C2Info> &info : buffer->info()) {
            // TODO: properly translate these to metadata
            switch (info->coreIndex().coreIndex()) {
                case C2StreamPictureTypeMaskInfo::CORE_INDEX:
                    if (((C2StreamPictureTypeMaskInfo *)info.get())->value & C2PictureTypeKeyFrame) {
                        flags |= MediaCodec::BUFFER_FLAG_SYNCFRAME;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    {
        Mutexed<std::unique_ptr<OutputBuffers>>::Locked buffers(mOutputBuffers);
        if (!(*buffers)->registerBuffer(buffer, &index, &outBuffer)) {
            ALOGE("onWorkDone: unable to register output buffer");
            // TODO
            // buffers.unlock();
            // mOnError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
            // buffers.lock();
            return;
        }
    }

    outBuffer->meta()->setInt64("timeUs", worklet->output.ordinal.timestamp.peek());
    outBuffer->meta()->setInt32("flags", flags);
    ALOGV("onWorkDone: out buffer index = %zu", index);
    mCallback->onOutputBufferAvailable(index, outBuffer);
}

status_t CCodecBufferChannel::setSurface(const sp<Surface> &newSurface) {
    if (newSurface != nullptr) {
        newSurface->setScalingMode(NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
        newSurface->setMaxDequeuedBufferCount(kMinBufferArraySize);
    }

    Mutexed<OutputSurface>::Locked output(mOutputSurface);
//    if (newSurface == nullptr) {
//        if (*surface != nullptr) {
//            ALOGW("cannot unset a surface");
//            return INVALID_OPERATION;
//        }
//        return OK;
//    }
//
//    if (*surface == nullptr) {
//        ALOGW("component was not configured with a surface");
//        return INVALID_OPERATION;
//    }

    output->surface = newSurface;
    output->bufferRefs.clear();
    // XXX: hack
    output->maxBufferCount = kMaxGraphicBufferRefCount;

    return OK;
}

}  // namespace android
