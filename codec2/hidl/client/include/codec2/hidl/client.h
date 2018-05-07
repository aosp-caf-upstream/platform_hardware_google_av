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

#ifndef CODEC2_HIDL_CLIENT_H_
#define CODEC2_HIDL_CLIENT_H_

#include <C2PlatformSupport.h>
#include <C2Component.h>
#include <C2Buffer.h>
#include <C2Param.h>
#include <C2.h>

#include <hidl/HidlSupport.h>
#include <utils/StrongPointer.h>

#include <map>
#include <memory>
#include <mutex>

/**
 * This file contains minimal interfaces for the framework to access Codec2.0.
 *
 * Codec2Client is the main class that contains the following inner classes:
 * - Listener
 * - Configurable
 * - Interface
 * - Component
 *
 * Classes in Codec2Client, interfaces in Codec2.0, and  HIDL interfaces are
 * related as follows:
 * - Codec2Client <==> C2ComponentStore <==> IComponentStore
 * - Codec2Client::Listener <==> C2Component::Listener <==> IComponentListener
 * - Codec2Client::Configurable <==> [No equivalent] <==> IConfigurable
 * - Codec2Client::Interface <==> C2ComponentInterface <==> IComponentInterface
 * - Codec2Client::Component <==> C2Component <==> IComponent
 *
 * The entry point is Codec2Client::CreateFromService(), which creates a
 * Codec2Client object. From Codec2Client, Interface and Component objects can
 * be created by calling createComponent() and createInterface().
 *
 * createComponent() takes a Listener object, which must be implemented by the
 * user.
 *
 * At the present, createBlockPool() is the only method that yields a
 * Configurable object. Note, however, that Interface, Component and
 * Codec2Client are all subclasses of Configurable.
 */

// Forward declaration of Codec2.0 HIDL interfaces
namespace hardware {
namespace google {
namespace media {
namespace c2 {
namespace V1_0 {
struct IConfigurable;
struct IComponentInterface;
struct IComponent;
struct IComponentStore;
struct IInputSurface;
struct IInputSurfaceConnection;
} // namespace V1_0
} // namespace c2
} // namespace media
} // namespace google
} // namespace hardware

// Forward declarations of other classes
namespace android {
class IGraphicBufferProducer;
namespace hardware {
namespace graphics {
namespace bufferqueue {
namespace V1_0 {
struct IGraphicBufferProducer;
} // namespace V1_0
} // namespace bufferqueue
} // namespace graphics
namespace media {
namespace omx {
namespace V1_0 {
struct IGraphicBufferSource;
} // namespace V1_0
} // namespace omx
} // namespace media
} // namespace hardware
} // namespace android

namespace android {

// This class is supposed to be called Codec2Client::Configurable, but forward
// declaration of an inner class is not possible.
struct Codec2ConfigurableClient {

    typedef ::hardware::google::media::c2::V1_0::IConfigurable Base;

    const C2String& getName() const;

    c2_status_t query(
            const std::vector<C2Param*>& stackParams,
            const std::vector<C2Param::Index> &heapParamIndices,
            c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2Param>>* const heapParams) const;

    c2_status_t config(
            const std::vector<C2Param*> &params,
            c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2SettingResult>>* const failures);

    c2_status_t querySupportedParams(
            std::vector<std::shared_ptr<C2ParamDescriptor>>* const params
            ) const;

    c2_status_t querySupportedValues(
            std::vector<C2FieldSupportedValuesQuery>& fields,
            c2_blocking_t mayBlock) const;

    // base cannot be null.
    Codec2ConfigurableClient(const sp<Base>& base);

protected:
    C2String mName;
    sp<Base> mBase;

    Base* base() const;

    friend struct Codec2Client;
};

struct Codec2Client : public Codec2ConfigurableClient {

    typedef ::hardware::google::media::c2::V1_0::IComponentStore Base;

    struct Listener;

    typedef Codec2ConfigurableClient Configurable;

    typedef Configurable Interface; // These two types may diverge in the future.

    struct Component;

    struct InputSurface;

    struct InputSurfaceConnection;

    typedef Codec2Client Store;

    c2_status_t createComponent(
            const C2String& name,
            const std::shared_ptr<Listener>& listener,
            std::shared_ptr<Component>* const component);

    c2_status_t createInterface(
            const C2String& name,
            std::shared_ptr<Interface>* const interface);

    c2_status_t createInputSurface(
            std::shared_ptr<InputSurface>* const inputSurface);

    const std::vector<C2Component::Traits>& listComponents() const;

    c2_status_t copyBuffer(
            const std::shared_ptr<C2Buffer>& src,
            const std::shared_ptr<C2Buffer>& dst);

    std::shared_ptr<C2ParamReflector> getParamReflector();

    static std::shared_ptr<Codec2Client> CreateFromService(
            const char* instanceName,
            bool waitForService = true);

    // Try to create a component with a given name from all known
    // IComponentStore services.
    static std::shared_ptr<Component> CreateComponentByName(
            const char* componentName,
            const std::shared_ptr<Listener>& listener,
            std::shared_ptr<Codec2Client>* owner = nullptr);

    // List traits from all known IComponentStore services.
    static const std::vector<C2Component::Traits>& ListComponents();

    // base cannot be null.
    Codec2Client(const sp<Base>& base);

protected:
    Base* base() const;

    mutable std::mutex mMutex;
    mutable bool mListed;
    mutable std::vector<C2Component::Traits> mTraitsList;
    mutable std::vector<std::unique_ptr<std::vector<std::string>>>
            mAliasesBuffer;
};

struct Codec2Client::Listener {

    virtual void onWorkDone(
            const std::weak_ptr<Component>& comp,
            std::list<std::unique_ptr<C2Work>>& workItems) = 0;

    virtual void onTripped(
            const std::weak_ptr<Component>& comp,
            const std::vector<std::shared_ptr<C2SettingResult>>& settingResults
            ) = 0;

    virtual void onError(
            const std::weak_ptr<Component>& comp,
            uint32_t errorCode) = 0;

    virtual void onDeath(
            const std::weak_ptr<Component>& comp) = 0;

    virtual ~Listener();

};

struct Codec2Client::Component : public Codec2Client::Configurable {

    typedef ::hardware::google::media::c2::V1_0::IComponent Base;

    c2_status_t createBlockPool(
            C2Allocator::id_t id,
            C2BlockPool::local_id_t* localId,
            std::shared_ptr<Configurable>* configurable);

    c2_status_t queue(
            std::list<std::unique_ptr<C2Work>>* const items);

    c2_status_t flush(
            C2Component::flush_mode_t mode,
            std::list<std::unique_ptr<C2Work>>* const flushedWork);

    c2_status_t drain(C2Component::drain_mode_t mode);

    c2_status_t start();

    c2_status_t stop();

    c2_status_t reset();

    c2_status_t release();

    typedef ::android::hardware::graphics::bufferqueue::V1_0::
            IGraphicBufferProducer IGraphicBufferProducer;
    typedef ::android::hardware::media::omx::V1_0::
            IGraphicBufferSource IGraphicBufferSource;

    // Output surface
    c2_status_t setOutputSurface(
            uint64_t blockPoolId,
            const sp<IGraphicBufferProducer>& surface);

    // Input surface
    c2_status_t connectToInputSurface(
            const std::shared_ptr<InputSurface>& surface);

    c2_status_t connectToOmxInputSurface(
            const sp<IGraphicBufferProducer>& producer,
            const sp<IGraphicBufferSource>& source);

    c2_status_t disconnectFromInputSurface();

    // Platform BlockPool support
    c2_status_t getLocalBlockPool(
            C2BlockPool::local_id_t id,
            std::shared_ptr<C2BlockPool>* pool) const;

    c2_status_t createLocalBlockPool(
            C2PlatformAllocatorStore::id_t allocatorId,
            std::shared_ptr<C2BlockPool>* pool) const;

    void handleOnWorkDone(const std::vector<uint64_t> &inputDone);

    // base cannot be null.
    Component(const sp<Base>& base);

protected:
    mutable std::mutex mInputBuffersMutex;
    mutable std::map<uint64_t, std::vector<std::shared_ptr<C2Buffer>>> mInputBuffers;

    Base* base() const;

    static c2_status_t setDeathListener(
            const std::shared_ptr<Component>& component,
            const std::shared_ptr<Listener>& listener);
    sp<::android::hardware::hidl_death_recipient> mDeathRecipient;

    friend struct Codec2Client;
};

struct Codec2Client::InputSurface {
public:
    typedef ::hardware::google::media::c2::V1_0::IInputSurface Base;

    typedef ::hardware::google::media::c2::V1_0::IInputSurfaceConnection
            ConnectionBase;

    typedef Codec2Client::InputSurfaceConnection Connection;

    typedef ::android::IGraphicBufferProducer IGraphicBufferProducer;

    c2_status_t connectToComponent(
            const std::shared_ptr<Component>& component,
            std::shared_ptr<Connection>* connection);

    std::shared_ptr<Configurable> getConfigurable() const;

    const sp<IGraphicBufferProducer>& getGraphicBufferProducer() const;

    // base cannot be null.
    InputSurface(const sp<Base>& base);

protected:
    Base* base() const;
    sp<Base> mBase;

    sp<IGraphicBufferProducer> mGraphicBufferProducer;

    friend struct Codec2Client;
    friend struct Component;
};

struct Codec2Client::InputSurfaceConnection {

    typedef ::hardware::google::media::c2::V1_0::IInputSurfaceConnection Base;

    c2_status_t disconnect();

    // base cannot be null.
    InputSurfaceConnection(const sp<Base>& base);

protected:
    Base* base() const;
    sp<Base> mBase;

    friend struct Codec2Client::InputSurface;
};

}  // namespace android

#endif  // CODEC2_HIDL_CLIENT_H_

