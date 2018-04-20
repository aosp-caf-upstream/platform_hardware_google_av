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

//#define LOG_NDEBUG 0
#define LOG_TAG "Codec2Client"
#include <log/log.h>

#include <codec2/hidl/client.h>

#include <codec2/hidl/1.0/types.h>

#include <hardware/google/media/c2/1.0/IComponentListener.h>
#include <hardware/google/media/c2/1.0/IConfigurable.h>
#include <hardware/google/media/c2/1.0/IComponentInterface.h>
#include <hardware/google/media/c2/1.0/IComponent.h>
#include <hardware/google/media/c2/1.0/IComponentStore.h>

#include <C2PlatformSupport.h>
#include <C2BufferPriv.h>
#include <C2Debug.h>
#include <gui/bufferqueue/1.0/H2BGraphicBufferProducer.h>
#include <hidl/HidlSupport.h>
#include <cutils/properties.h>

#include <limits>
#include <type_traits>
#include <vector>
#include <map>

namespace android {

using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_string;
using ::android::hardware::Return;
using ::android::hardware::Void;

using namespace ::hardware::google::media::c2::V1_0;
using namespace ::hardware::google::media::c2::V1_0::utils;

namespace /* unnamed */ {

// c2_status_t value that corresponds to hwbinder transaction failure.
constexpr c2_status_t C2_TRANSACTION_FAILED = C2_CORRUPTED;

// List of known IComponentStore services.
constexpr const char* kClientNames[] = {
        "default",
        "software",
    };

typedef std::array<
        std::shared_ptr<Codec2Client>,
        std::extent<decltype(kClientNames)>::value> ClientList;

// Convenience methods to obtain known clients.
std::shared_ptr<Codec2Client> getClient(size_t index) {
    return Codec2Client::CreateFromService(kClientNames[index]);
}

ClientList getClientList() {
    ClientList list;
    for (size_t i = 0; i < list.size(); ++i) {
        list[i] = getClient(i);
    }
    return list;
}

} // unnamed

// Codec2ConfigurableClient

const C2String& Codec2ConfigurableClient::getName() const {
    return mName;
}

Codec2ConfigurableClient::Base* Codec2ConfigurableClient::base() const {
    return static_cast<Base*>(mBase.get());
}

Codec2ConfigurableClient::Codec2ConfigurableClient(
        const sp<Codec2ConfigurableClient::Base>& base) : mBase(base) {
    Return<void> transStatus = base->getName(
            [this](const hidl_string& name) {
                mName = name.c_str();
            });
    if (!transStatus.isOk()) {
        ALOGE("Cannot obtain name from IConfigurable.");
    }
}

c2_status_t Codec2ConfigurableClient::query(
        const std::vector<C2Param*> &stackParams,
        const std::vector<C2Param::Index> &heapParamIndices,
        c2_blocking_t mayBlock,
        std::vector<std::unique_ptr<C2Param>>* const heapParams) const {
    hidl_vec<ParamIndex> indices(
            stackParams.size() + heapParamIndices.size());
    size_t numIndices = 0;
    for (C2Param* const& stackParam : stackParams) {
        if (!stackParam) {
            ALOGW("query -- null stack param encountered.");
            continue;
        }
        indices[numIndices++] = static_cast<ParamIndex>(stackParam->index());
    }
    size_t numStackIndices = numIndices;
    for (const C2Param::Index& index : heapParamIndices) {
        indices[numIndices++] =
                static_cast<ParamIndex>(static_cast<uint32_t>(index));
    }
    indices.resize(numIndices);
    if (heapParams) {
        heapParams->reserve(numIndices);
        heapParams->clear();
    }
    c2_status_t status;
    Return<void> transStatus = base()->query(
            indices,
            mayBlock == C2_MAY_BLOCK,
            [&status, &numStackIndices, &stackParams, heapParams](
                    Status s, const Params& p) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    ALOGE("query -- call failed. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                std::vector<C2Param*> paramPointers;
                status = parseParamsBlob(&paramPointers, p);
                if (status != C2_OK) {
                    ALOGE("query -- error while parsing params. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                size_t i = 0;
                for (C2Param* const& paramPointer : paramPointers) {
                    if (numStackIndices > 0) {
                        --numStackIndices;
                        if (!paramPointer) {
                            ALOGW("query -- null stack param.");
                            if (numStackIndices > 0) {
                                ++i;
                            }
                            continue;
                        }
                        for (; !stackParams[i]; ++i) {
                            if (i >= stackParams.size()) {
                                ALOGE("query -- unexpected error.");
                                status = C2_CORRUPTED;
                                return;
                            }
                        }
                        if (!stackParams[i++]->updateFrom(*paramPointer)) {
                            ALOGW("query -- param update failed. index = %d",
                                    static_cast<int>(paramPointer->index()));
                        }
                    } else {
                        if (!paramPointer) {
                            ALOGW("query -- null heap param.");
                            continue;
                        }
                        if (!heapParams) {
                            ALOGW("query -- extra stack param.");
                        }
                        heapParams->emplace_back(C2Param::Copy(*paramPointer));
                    }
                }
            });
    if (!transStatus.isOk()) {
        ALOGE("query -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

c2_status_t Codec2ConfigurableClient::config(
        const std::vector<C2Param*> &params,
        c2_blocking_t mayBlock,
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    Params hidlParams;
    Status hidlStatus = createParamsBlob(&hidlParams, params);
    if (hidlStatus != Status::OK) {
        ALOGE("config -- bad input.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status;
    Return<void> transStatus = base()->config(
            hidlParams,
            mayBlock == C2_MAY_BLOCK,
            [&status, &params, failures](
                    Status s,
                    const hidl_vec<SettingResult> f,
                    const Params& o) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    ALOGE("config -- call failed. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                failures->clear();
                failures->resize(f.size());
                size_t i = 0;
                for (const SettingResult& sf : f) {
                    status = objcpy(&(*failures)[i++], sf);
                    if (status != C2_OK) {
                        ALOGE("config -- invalid returned SettingResult. "
                                "Error code = %d", static_cast<int>(status));
                        return;
                    }
                }
                status = updateParamsFromBlob(params, o);
            });
    if (!transStatus.isOk()) {
        ALOGE("config -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

c2_status_t Codec2ConfigurableClient::querySupportedParams(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* const params) const {
    // TODO: Cache and query properly!
    c2_status_t status;
    Return<void> transStatus = base()->querySupportedParams(
            std::numeric_limits<uint32_t>::min(),
            std::numeric_limits<uint32_t>::max(),
            [&status, params](
                    Status s,
                    const hidl_vec<ParamDescriptor>& p) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    ALOGE("querySupportedParams -- call failed. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                params->resize(p.size());
                size_t i = 0;
                for (const ParamDescriptor& sp : p) {
                    status = objcpy(&(*params)[i++], sp);
                    if (status != C2_OK) {
                        ALOGE("querySupportedParams -- "
                                "invalid returned ParamDescriptor. "
                                "Error code = %d", static_cast<int>(status));
                        return;
                    }
                }
            });
    if (!transStatus.isOk()) {
        ALOGE("querySupportedParams -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

c2_status_t Codec2ConfigurableClient::querySupportedValues(
        std::vector<C2FieldSupportedValuesQuery>& fields,
        c2_blocking_t mayBlock) const {
    hidl_vec<FieldSupportedValuesQuery> inFields(fields.size());
    for (size_t i = 0; i < fields.size(); ++i) {
        Status hidlStatus = objcpy(&inFields[i], fields[i]);
        if (hidlStatus != Status::OK) {
            ALOGE("querySupportedValues -- bad input");
            return C2_TRANSACTION_FAILED;
        }
    }

    c2_status_t status;
    Return<void> transStatus = base()->querySupportedValues(
            inFields,
            mayBlock == C2_MAY_BLOCK,
            [&status, &inFields, &fields](
                    Status s,
                    const hidl_vec<FieldSupportedValuesQueryResult>& r) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    ALOGE("querySupportedValues -- call failed. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                if (r.size() != fields.size()) {
                    ALOGE("querySupportedValues -- input and output lists "
                            "have different sizes.");
                    status = C2_CORRUPTED;
                    return;
                }
                for (size_t i = 0; i < fields.size(); ++i) {
                    status = objcpy(&fields[i], inFields[i], r[i]);
                    if (status != C2_OK) {
                        ALOGE("querySupportedValues -- invalid returned value. "
                                "Error code = %d", static_cast<int>(status));
                        return;
                    }
                }
            });
    if (!transStatus.isOk()) {
        ALOGE("querySupportedValues -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

// Codec2Client

Codec2Client::Base* Codec2Client::base() const {
    return static_cast<Base*>(mBase.get());
}

Codec2Client::Codec2Client(const sp<Codec2Client::Base>& base) :
    Codec2ConfigurableClient(base), mListed(false) {
}

c2_status_t Codec2Client::createComponent(
        const C2String& name,
        const std::shared_ptr<Codec2Client::Listener>& listener,
        std::shared_ptr<Codec2Client::Component>* const component) {

    // TODO: Add support for Bufferpool

    struct HidlListener : public IComponentListener {
        std::weak_ptr<Component> component;
        std::weak_ptr<Listener> base;

        virtual Return<void> onWorkDone(const WorkBundle& workBundle) override {
            std::list<std::unique_ptr<C2Work>> workItems;
            c2_status_t status = objcpy(&workItems, workBundle);
            if (status != C2_OK) {
                ALOGE("onWorkDone -- received corrupted WorkBundle. "
                        "status = %d.", static_cast<int>(status));
                return Void();
            }
            // release input buffers potentially held by the component from queue
            std::shared_ptr<Codec2Client::Component> componentStrong = component.lock();
            if (componentStrong) {
                std::vector<uint64_t> inputDone;
                for (const std::unique_ptr<C2Work> &work : workItems) {
                    if (work) {
                        inputDone.emplace_back(work->input.ordinal.frameIndex.peeku());
                    }
                }
                componentStrong->handleOnWorkDone(inputDone);
            }
            if (std::shared_ptr<Codec2Client::Listener> listener = base.lock()) {
                listener->onWorkDone(component, workItems);
            } else {
                ALOGW("onWorkDone -- listener died.");
            }
            return Void();
        }

        virtual Return<void> onTripped(
                const hidl_vec<SettingResult>& settingResults) override {
            std::vector<std::shared_ptr<C2SettingResult>> c2SettingResults(
                    settingResults.size());
            c2_status_t status;
            for (size_t i = 0; i < settingResults.size(); ++i) {
                std::unique_ptr<C2SettingResult> c2SettingResult;
                status = objcpy(&c2SettingResult, settingResults[i]);
                if (status != C2_OK) {
                    ALOGE("onTripped -- received corrupted SettingResult. "
                            "status = %d.", static_cast<int>(status));
                    return Void();
                }
                c2SettingResults[i] = std::move(c2SettingResult);
            }
            if (std::shared_ptr<Codec2Client::Listener> listener = base.lock()) {
                listener->onTripped(component, c2SettingResults);
            } else {
                ALOGW("onTripped -- listener died.");
            }
            return Void();
        }

        virtual Return<void> onError(Status s, uint32_t errorCode) override {
            ALOGE("onError -- status = %d, errorCode = %u.",
                    static_cast<int>(s),
                    static_cast<unsigned>(errorCode));
            if (std::shared_ptr<Listener> listener = base.lock()) {
                listener->onError(component, s == Status::OK ?
                        errorCode : static_cast<c2_status_t>(s));
            } else {
                ALOGW("onError -- listener died.");
            }
            return Void();
        }
    };

    c2_status_t status;
    sp<HidlListener> hidlListener = new HidlListener();
    hidlListener->base = listener;
    Return<void> transStatus = base()->createComponent(
            name,
            hidlListener,
            nullptr,
            [&status, component, hidlListener](
                    Status s,
                    const sp<IComponent>& c) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    return;
                }
                *component = std::make_shared<Codec2Client::Component>(c);
                hidlListener->component = *component;
            });
    if (!transStatus.isOk()) {
        ALOGE("createComponent -- failed transaction.");
        return C2_TRANSACTION_FAILED;
    }

    if (status != C2_OK) {
        return status;
    }

    if (!*component) {
        ALOGE("createComponent -- null component.");
        return C2_CORRUPTED;
    }

    status = (*component)->setDeathListener(*component, listener);
    if (status != C2_OK) {
        ALOGE("createComponent -- setDeathListener returned error: %d.",
                static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::createInterface(
        const C2String& name,
        std::shared_ptr<Codec2Client::Interface>* const interface) {
    c2_status_t status;
    Return<void> transStatus = base()->createInterface(
            name,
            [&status, interface](
                    Status s,
                    const sp<IComponentInterface>& i) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    ALOGE("createInterface -- call failed. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                *interface = std::make_shared<Codec2Client::Interface>(i);
            });
    if (!transStatus.isOk()) {
        ALOGE("createInterface -- failed transaction.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

c2_status_t Codec2Client::createInputSurface(
        std::shared_ptr<Codec2Client::InputSurface>* const inputSurface) {
    Return<sp<IInputSurface>> transResult = base()->createInputSurface();
    if (!transResult.isOk()) {
        ALOGE("createInputSurface -- failed transaction.");
        return C2_TRANSACTION_FAILED;
    }
    *inputSurface = std::make_shared<InputSurface>(
            static_cast<sp<IInputSurface>>(transResult));
    if (!*inputSurface) {
        ALOGE("createInputSurface -- failed to create client.");
        return C2_CORRUPTED;
    }
    return C2_OK;
}

const std::vector<C2Component::Traits>& Codec2Client::listComponents() const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mListed) {
        return mTraitsList;
    }
    Return<void> transStatus = base()->listComponents(
            [this](const hidl_vec<IComponentStore::ComponentTraits>& t) {
                mTraitsList.resize(t.size());
                mAliasesBuffer.resize(t.size());
                for (size_t i = 0; i < t.size(); ++i) {
                    c2_status_t status = objcpy(
                            &mTraitsList[i], &mAliasesBuffer[i], t[i]);
                    if (status != C2_OK) {
                        ALOGE("listComponents -- corrupted output.");
                        return;
                    }
                }
            });
    if (!transStatus.isOk()) {
        ALOGE("listComponents -- failed transaction.");
    }
    mListed = true;
    return mTraitsList;
}

c2_status_t Codec2Client::copyBuffer(
        const std::shared_ptr<C2Buffer>& src,
        const std::shared_ptr<C2Buffer>& dst) {
    // TODO: Implement?
    (void)src;
    (void)dst;
    ALOGE("copyBuffer not implemented");
    return C2_OMITTED;
}

std::shared_ptr<C2ParamReflector>
        Codec2Client::getParamReflector() {
    // TODO: this is not meant to be exposed as C2ParamReflector on the client side; instead, it
    // should reflect the HAL API.
    struct SimpleParamReflector : public C2ParamReflector {
        virtual std::unique_ptr<C2StructDescriptor> describe(C2Param::CoreIndex coreIndex) const {
            hidl_vec<ParamIndex> indices(1);
            indices[0] = static_cast<ParamIndex>(coreIndex.coreIndex());
            std::unique_ptr<C2StructDescriptor> descriptor;
            Return<void> transStatus = mBase->getStructDescriptors(
                    indices,
                    [&descriptor](
                            Status s,
                            const hidl_vec<StructDescriptor>& sd) {
                        c2_status_t status = static_cast<c2_status_t>(s);
                        if (status != C2_OK) {
                            ALOGE("getStructDescriptors -- call failed. "
                                    "Error code = %d", static_cast<int>(status));
                            descriptor.reset();
                            return;
                        }
                        if (sd.size() != 1) {
                            ALOGD("getStructDescriptors -- returned vector of size %zu.",
                                    sd.size());
                            descriptor.reset();
                            return;
                        }
                        status = objcpy(&descriptor, sd[0]);
                        if (status != C2_OK) {
                            ALOGD("getStructDescriptors -- failed to convert. "
                                    "Error code = %d", static_cast<int>(status));
                            descriptor.reset();
                            return;
                        }
                    });
            return descriptor;
        }

        SimpleParamReflector(sp<Base> base)
            : mBase(base) { }

        sp<Base> mBase;
    };

    return std::make_shared<SimpleParamReflector>(base());
};

std::shared_ptr<Codec2Client> Codec2Client::CreateFromService(
        const char* instanceName, bool waitForService) {
    if (!instanceName) {
        return nullptr;
    }
    sp<Base> baseStore = waitForService ?
            Base::getService(instanceName) :
            Base::tryGetService(instanceName);
    if (!baseStore) {
        if (waitForService) {
            ALOGE("Codec2.0 service inaccessible. Check the device manifest.");
        } else {
            ALOGW("Codec2.0 service not available right now. Try again later.");
        }
        return nullptr;
    }
    return std::make_shared<Codec2Client>(baseStore);
}

std::shared_ptr<Codec2Client::Component>
        Codec2Client::CreateComponentByName(
        const char* componentName,
        const std::shared_ptr<Listener>& listener,
        std::shared_ptr<Codec2Client>* owner) {
    c2_status_t status;
    std::shared_ptr<Component> component;

    // Cache the mapping componentName -> index of Codec2Client in
    // getClientList().
    static std::mutex component2IndexMutex;
    static std::map<std::string, size_t> component2Index;

    component2IndexMutex.lock();
    std::map<std::string, size_t>::const_iterator it =
            component2Index.find(componentName);
    if (it != component2Index.end()) {
        std::shared_ptr<Codec2Client> client = getClient(it->second);
        component2IndexMutex.unlock();
        if (client) {
            status = client->createComponent(
                    componentName,
                    listener,
                    &component);
            if (status == C2_OK) {
                if (owner) {
                    *owner = client;
                }
                return component;
            }
        }
        ALOGW("IComponentStore instance that hosted component \"%s\" "
                "failed to create the component. Retrying...", componentName);
    } else {
        component2IndexMutex.unlock();
    }

    size_t index = 0;
    for (const std::shared_ptr<Codec2Client>& client : getClientList()) {
        if (!client) {
            ++index;
            continue;
        }
        status = client->createComponent(
                componentName,
                listener,
                &component);
        if (status == C2_OK) {
            component2IndexMutex.lock();
            component2Index[componentName] = index;
            component2IndexMutex.unlock();
            if (owner) {
                *owner = client;
            }
            return component;
        } else if (status != C2_NOT_FOUND) {
            ALOGE("CreateComponentByName -- failed to create component \"%s\": "
                    " error code = %d.",
                    componentName, static_cast<int>(status));
            return nullptr;
        }
        ++index;
    }

    ALOGW("CreateComponentByName -- component \"%s\" not found.",
            componentName);
    return nullptr;
}

const std::vector<C2Component::Traits>& Codec2Client::ListComponents() {
    static std::vector<C2Component::Traits> traitsList = [](){
        std::vector<C2Component::Traits> list;
        size_t listSize = 0;
        ClientList clientList = getClientList();
        for (const std::shared_ptr<Codec2Client>& client : clientList) {
            if (!client) {
                continue;
            }
            listSize += client->listComponents().size();
        }
        list.reserve(listSize);
        for (const std::shared_ptr<Codec2Client>& client : clientList) {
            if (!client) {
                continue;
            }
            list.insert(
                    list.end(),
                    client->listComponents().begin(),
                    client->listComponents().end());
        }
        return list;
    }();

    return traitsList;
}

// Codec2Client::Listener

Codec2Client::Listener::~Listener() {
}

// Codec2Client::Component

Codec2Client::Component::Base* Codec2Client::Component::base() const {
    return static_cast<Base*>(mBase.get());
}

Codec2Client::Component::Component(const sp<Codec2Client::Component::Base>& base) :
    Codec2Client::Configurable(base) {
}

c2_status_t Codec2Client::Component::createBlockPool(
        C2Allocator::id_t id,
        C2BlockPool::local_id_t* localId,
        std::shared_ptr<Codec2Client::Configurable>* configurable) {
    c2_status_t status;
    Return<void> transStatus = base()->createBlockPool(
            static_cast<uint32_t>(id),
            [&status, localId, configurable](
                    Status s,
                    uint64_t pId,
                    const sp<IConfigurable>& c) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    ALOGE("createBlockPool -- call failed. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                *localId = static_cast<C2BlockPool::local_id_t>(pId);
                *configurable = std::make_shared<Codec2Client::Configurable>(c);
            });
    if (!transStatus.isOk()) {
        ALOGE("createBlockPool -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

void Codec2Client::Component::handleOnWorkDone(const std::vector<uint64_t> &inputDone) {
    std::lock_guard<std::mutex> lock(mInputBuffersMutex);
    for (uint64_t inputIndex : inputDone) {
        auto it = mInputBuffers.find(inputIndex);
        if (it == mInputBuffers.end()) {
            ALOGI("unknown input index %llu in onWorkDone", (long long)inputIndex);
        } else {
            ALOGV("done with input index %llu with %zu buffers",
                    (long long)inputIndex, it->second.size());
            mInputBuffers.erase(it);
        }
    }
}

c2_status_t Codec2Client::Component::queue(
        std::list<std::unique_ptr<C2Work>>* const items) {
    // remember input buffers queued to hold reference to them
    {
        std::lock_guard<std::mutex> lock(mInputBuffersMutex);
        for (const std::unique_ptr<C2Work> &work : *items) {
            if (!work) {
                continue;
            }

            uint64_t inputIndex = work->input.ordinal.frameIndex.peeku();
            auto res = mInputBuffers.emplace(inputIndex, work->input.buffers);
            if (!res.second) {
                ALOGI("duplicate input index %llu in queue", (long long)inputIndex);
                // TODO: append? - for now we are replacing
                res.first->second = work->input.buffers;
            }
            ALOGV("qeueing input index %llu with %zu buffers",
                    (long long)inputIndex, work->input.buffers.size());
        }
    }

    WorkBundle workBundle;
    Status hidlStatus = objcpy(&workBundle, *items);
    if (hidlStatus != Status::OK) {
        ALOGE("queue -- bad input.");
        return C2_TRANSACTION_FAILED;
    }
    Return<Status> transStatus = base()->queue(workBundle);
    if (!transStatus.isOk()) {
        ALOGE("queue -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("queue -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::flush(
        C2Component::flush_mode_t mode,
        std::list<std::unique_ptr<C2Work>>* const flushedWork) {
    (void)mode; // Flush mode isn't supported in HIDL yet.
    c2_status_t status;
    Return<void> transStatus = base()->flush(
            [&status, flushedWork](
                    Status s, const WorkBundle& wb) {
                status = static_cast<c2_status_t>(s);
                if (status != C2_OK) {
                    ALOGE("flush -- call failed. "
                            "Error code = %d", static_cast<int>(status));
                    return;
                }
                status = objcpy(flushedWork, wb);
            });
    if (!transStatus.isOk()) {
        ALOGE("flush -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

c2_status_t Codec2Client::Component::drain(C2Component::drain_mode_t mode) {
    Return<Status> transStatus = base()->drain(
            mode == C2Component::DRAIN_COMPONENT_WITH_EOS);
    if (!transStatus.isOk()) {
        ALOGE("drain -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("drain -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::start() {
    Return<Status> transStatus = base()->start();
    if (!transStatus.isOk()) {
        ALOGE("start -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("start -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::stop() {
    Return<Status> transStatus = base()->stop();
    if (!transStatus.isOk()) {
        ALOGE("stop -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("stop -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::reset() {
    Return<Status> transStatus = base()->reset();
    if (!transStatus.isOk()) {
        ALOGE("reset -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("reset -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::release() {
    Return<Status> transStatus = base()->release();
    if (!transStatus.isOk()) {
        ALOGE("release -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("release -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::setOutputSurface(
        uint64_t blockPoolId,
        const sp<IGraphicBufferProducer>& surface) {
    Return<Status> transStatus = base()->setOutputSurface(blockPoolId, surface);
    if (!transStatus.isOk()) {
        ALOGE("setOutputSurface -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("setOutputSurface -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::connectToInputSurface(
        const std::shared_ptr<InputSurface>& surface) {
    Return<Status> transStatus = base()->connectToInputSurface(
            surface->base());
    if (!transStatus.isOk()) {
        ALOGE("connectToInputSurface -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("connectToInputSurface -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::connectToOmxInputSurface(
        const sp<IGraphicBufferProducer>& producer,
        const sp<IGraphicBufferSource>& source) {
    Return<Status> transStatus = base()->connectToOmxInputSurface(
            producer, source);
    if (!transStatus.isOk()) {
        ALOGE("connectToOmxInputSurface -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("connectToOmxInputSurface -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::disconnectFromInputSurface() {
    Return<Status> transStatus = base()->disconnectFromInputSurface();
    if (!transStatus.isOk()) {
        ALOGE("disconnectToInputSurface -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    c2_status_t status =
            static_cast<c2_status_t>(static_cast<Status>(transStatus));
    if (status != C2_OK) {
        ALOGE("disconnectFromInputSurface -- call failed. "
                "Error code = %d", static_cast<int>(status));
    }
    return status;
}

c2_status_t Codec2Client::Component::getLocalBlockPool(
        C2BlockPool::local_id_t id,
        std::shared_ptr<C2BlockPool>* pool) const {
    pool->reset();
    if (!mBase) {
        return C2_BAD_VALUE;
    }
    // TODO support pre-registered block pools
    std::shared_ptr<C2AllocatorStore> allocatorStore = GetCodec2PlatformAllocatorStore();
    std::shared_ptr<C2Allocator> allocator;
    c2_status_t res = C2_NOT_FOUND;

    switch (id) {
    case C2BlockPool::BASIC_LINEAR:
        res = allocatorStore->fetchAllocator(C2AllocatorStore::DEFAULT_LINEAR, &allocator);
        if (res == C2_OK) {
            *pool = std::make_shared<C2BasicLinearBlockPool>(allocator);
        }
        break;
    case C2BlockPool::BASIC_GRAPHIC:
        res = allocatorStore->fetchAllocator(C2AllocatorStore::DEFAULT_GRAPHIC, &allocator);
        if (res == C2_OK) {
            *pool = std::make_shared<C2BasicGraphicBlockPool>(allocator);
        }
        break;
    default:
        break;
    }
    if (res != C2_OK) {
        ALOGE("getLocalBlockPool -- failed to get pool with id %d. "
                "Error code = %d",
                static_cast<int>(id),
                res);
    }
    return res;
}

c2_status_t Codec2Client::Component::createLocalBlockPool(
        C2PlatformAllocatorStore::id_t allocatorId,
        std::shared_ptr<C2BlockPool>* pool) const {
    pool->reset();
    if (!mBase) {
        return C2_BAD_VALUE;
    }
    // TODO: support caching block pool along with GetCodec2BlockPool.
    static std::atomic_int sBlockPoolId(C2BlockPool::PLATFORM_START);
    std::shared_ptr<C2AllocatorStore> allocatorStore = GetCodec2PlatformAllocatorStore();
    std::shared_ptr<C2Allocator> allocator;
    c2_status_t res = C2_NOT_FOUND;

    switch (allocatorId) {
    case C2PlatformAllocatorStore::ION:
        res = allocatorStore->fetchAllocator(C2AllocatorStore::DEFAULT_LINEAR, &allocator);
        if (res == C2_OK) {
            *pool = std::make_shared<C2PooledBlockPool>(allocator, sBlockPoolId++);
            if (!*pool) {
                res = C2_NO_MEMORY;
            }
        }
        break;
    case C2PlatformAllocatorStore::GRALLOC:
        // TODO: support gralloc
        break;
    default:
        break;
    }
    if (res != C2_OK) {
        ALOGE("createLocalBlockPool -- "
                "failed to create pool with allocator id %d. "
                "Error code = %d",
                static_cast<int>(allocatorId),
                res);
    }
    return res;
}

c2_status_t Codec2Client::Component::setDeathListener(
        const std::shared_ptr<Component>& component,
        const std::shared_ptr<Listener>& listener) {

    struct HidlDeathRecipient : public hardware::hidl_death_recipient {
        std::weak_ptr<Component> component;
        std::weak_ptr<Listener> base;

        virtual void serviceDied(
                uint64_t /* cookie */,
                const wp<::android::hidl::base::V1_0::IBase>& /* who */
                ) override {
            if (std::shared_ptr<Codec2Client::Listener> listener = base.lock()) {
                listener->onDeath(component);
            } else {
                ALOGW("onDeath -- listener died.");
            }
        }
    };

    sp<HidlDeathRecipient> deathRecipient = new HidlDeathRecipient();
    deathRecipient->base = listener;
    deathRecipient->component = component;

    component->mDeathRecipient = deathRecipient;
    Return<bool> transResult = component->base()->linkToDeath(
            component->mDeathRecipient, 0);
    if (!transResult.isOk()) {
        ALOGE("setDeathListener -- failed transaction: linkToDeath.");
        return C2_TRANSACTION_FAILED;
    }
    if (!static_cast<bool>(transResult)) {
        ALOGE("setDeathListener -- linkToDeath call failed.");
        return C2_CORRUPTED;
    }
    return C2_OK;
}

// Codec2Client::InputSurface

Codec2Client::InputSurface::Base* Codec2Client::InputSurface::base() const {
    return static_cast<Base*>(mBase.get());
}

Codec2Client::InputSurface::InputSurface(const sp<IInputSurface>& base) :
    mBase(base),
    mGraphicBufferProducer(new
            ::android::hardware::graphics::bufferqueue::V1_0::utils::
            H2BGraphicBufferProducer(base)) {
}

c2_status_t Codec2Client::InputSurface::connectToComponent(
        const std::shared_ptr<Codec2Client::Component>& component,
        std::shared_ptr<Connection>* connection) {
    c2_status_t status;
    Return<void> transStatus = base()->connectToComponent(
        component->base(),
        [&status, connection](
                Status s,
                const sp<IInputSurfaceConnection>& c) {
            status = static_cast<c2_status_t>(s);
            if (status != C2_OK) {
                ALOGE("connectToComponent -- call failed. "
                        "Error code = %d", static_cast<int>(status));
                return;
            }
            *connection = std::make_shared<Connection>(c);
        });
    if (!transStatus.isOk()) {
        ALOGE("connect -- transaction failed.");
        return C2_TRANSACTION_FAILED;
    }
    return status;
}

std::shared_ptr<Codec2Client::Configurable>
        Codec2Client::InputSurface::getConfigurable() const {
    Return<sp<IConfigurable>> transResult = base()->getConfigurable();
    if (!transResult.isOk()) {
        ALOGW("getConfigurable -- transaction failed.");
        return nullptr;
    }
    if (!static_cast<sp<IConfigurable>>(transResult)) {
        ALOGW("getConfigurable -- null pointer.");
        return nullptr;
    }
    return std::make_shared<Configurable>(transResult);
}

const sp<IGraphicBufferProducer>&
        Codec2Client::InputSurface::getGraphicBufferProducer() const {
    return mGraphicBufferProducer;
}

// Codec2Client::InputSurfaceConnection

Codec2Client::InputSurfaceConnection::Base*
        Codec2Client::InputSurfaceConnection::base() const {
    return static_cast<Base*>(mBase.get());
}

Codec2Client::InputSurfaceConnection::InputSurfaceConnection(
        const sp<Codec2Client::InputSurfaceConnection::Base>& base) :
    mBase(base) {
}

c2_status_t Codec2Client::InputSurfaceConnection::disconnect() {
    Return<Status> transResult = base()->disconnect();
    return static_cast<c2_status_t>(static_cast<Status>(transResult));
}

}  // namespace android

