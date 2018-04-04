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

#define __C2_GENERATE_GLOBAL_VARS__

#include <set>

#include <gtest/gtest.h>

#include <C2ParamDef.h>

#include <ReflectedParamUpdater.h>

namespace android {

namespace {

enum {
    kParamIndexTestStart = 0x1000,
    kParamIndexInt,
    kParamIndexString,
    kParamIndexComposite,

    kParamIndexLong = C2Param::TYPE_INDEX_VENDOR_START,
};

typedef C2GlobalParam<C2Info, C2Int32Value, kParamIndexInt> C2IntInfo;
typedef C2GlobalParam<C2Info, C2Int64Value, kParamIndexLong> C2LongInfo;

struct C2FixedSizeStringStruct {
    char value[128];

    DEFINE_AND_DESCRIBE_BASE_C2STRUCT(FixedSizeString)
    C2FIELD(value, "value")
};
typedef C2GlobalParam<C2Info, C2FixedSizeStringStruct, kParamIndexString> C2StringInfo;

struct C2CompositeStruct {
    int32_t i32;
    uint64_t u64;
    char str[64];

    DEFINE_AND_DESCRIBE_BASE_C2STRUCT(Composite)
    C2FIELD(i32, "i32")
    C2FIELD(u64, "u64")
    C2FIELD(str, "str")
};
typedef C2GlobalParam<C2Info, C2CompositeStruct, kParamIndexComposite> C2CompositeInfo;

#define SUPPORTED_TYPES   \
    C2IntInfo,            \
    C2LongInfo,           \
    C2StringInfo,         \
    C2CompositeInfo

template<typename... TYPES> struct describe_impl;
template<typename T, typename... TYPES> struct describe_impl<T, TYPES...> {
    static std::unique_ptr<C2StructDescriptor> describe(C2Param::CoreIndex index) {
        if (index == T::CORE_INDEX) {
            return std::make_unique<C2StructDescriptor>(T::CORE_INDEX, T::FieldList());
        } else {
            return describe_impl<TYPES...>::describe(index);
        }
    }
};

template<> struct describe_impl<> {
    static std::unique_ptr<C2StructDescriptor> describe(C2Param::CoreIndex) {
        return nullptr;
    }
};

template<typename T> const char *GetName()        { return nullptr; }
template<> const char *GetName<C2IntInfo>()       { return "int"; }
template<> const char *GetName<C2LongInfo>()      { return "long"; }
template<> const char *GetName<C2StringInfo>()    { return "string"; }
template<> const char *GetName<C2CompositeInfo>() { return "composite"; }

template<typename... TYPES> struct fill_descriptors_impl;
template<typename T, typename... TYPES> struct fill_descriptors_impl<T, TYPES...> {
    static void fill(std::vector<std::shared_ptr<C2ParamDescriptor>> *vec) {
        fill_descriptors_impl<TYPES...>::fill(vec);
        vec->push_back(std::make_shared<C2ParamDescriptor>(
                T::PARAM_TYPE, C2ParamDescriptor::IS_PERSISTENT, GetName<T>()));
    }
};

template<> struct fill_descriptors_impl<> {
    static void fill(std::vector<std::shared_ptr<C2ParamDescriptor>> *) {}
};

template<typename T> T *CastParam(const std::unique_ptr<C2Param> &param) {
    return (T *)param.get();
}

class ParamReflector : public C2ParamReflector {
public:
    ParamReflector() = default;
    ~ParamReflector() override = default;

    std::unique_ptr<C2StructDescriptor> describe(C2Param::CoreIndex paramIndex) const override {
        return describe_impl<SUPPORTED_TYPES>::describe(paramIndex);
    }
};

}  // namespace

class ReflectedParamUpdaterTest : public ::testing::Test {
public:
    ReflectedParamUpdaterTest() : mReflector(new ParamReflector) {
        fill_descriptors_impl<SUPPORTED_TYPES>::fill(&mDescriptors);
    }

    std::shared_ptr<C2ParamReflector> mReflector;
    std::vector<std::shared_ptr<C2ParamDescriptor>> mDescriptors;
};

TEST_F(ReflectedParamUpdaterTest, SingleValueTest) {
    ReflectedParamUpdater updater;

    sp<AMessage> msg(new AMessage);
    msg->setInt32("int.value", 12);
    msg->setInt64("vendor.long.value", 34);

    updater.addParamDesc(mReflector, mDescriptors);

    std::vector<C2Param::Index> indices;
    updater.getParamIndicesFromMessage(msg, &indices);
    ASSERT_EQ(1, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2IntInfo::PARAM_TYPE; }));
    ASSERT_EQ(1, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2LongInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2StringInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2CompositeInfo::PARAM_TYPE; }));

    std::vector<std::unique_ptr<C2Param>> params;
    params.emplace_back(new C2IntInfo);
    params.emplace_back(new C2LongInfo);
    ASSERT_EQ(0, CastParam<C2IntInfo>(params[0])->value);
    ASSERT_EQ(0, CastParam<C2LongInfo>(params[1])->value);

    updater.updateParamsFromMessage(msg, &params);
    ASSERT_EQ(12, CastParam<C2IntInfo>(params[0])->value);
    ASSERT_EQ(34, CastParam<C2LongInfo>(params[1])->value);
}

TEST_F(ReflectedParamUpdaterTest, StringTest) {
    ReflectedParamUpdater updater;

    sp<AMessage> msg(new AMessage);
    msg->setString("string.value", "56");

    updater.addParamDesc(mReflector, mDescriptors);

    std::vector<C2Param::Index> indices;
    updater.getParamIndicesFromMessage(msg, &indices);
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2IntInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2LongInfo::PARAM_TYPE; }));
    ASSERT_EQ(1, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2StringInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2CompositeInfo::PARAM_TYPE; }));

    std::vector<std::unique_ptr<C2Param>> params;
    params.emplace_back(new C2StringInfo);
    ASSERT_EQ(0, CastParam<C2StringInfo>(params[0])->value[0]);

    updater.updateParamsFromMessage(msg, &params);
    ASSERT_EQ(0, strcmp("56", CastParam<C2StringInfo>(params[0])->value));
}

TEST_F(ReflectedParamUpdaterTest, CompositeTest) {
    ReflectedParamUpdater updater;

    sp<AMessage> msg(new AMessage);
    msg->setInt32("composite.i32", 78);
    msg->setInt64("composite.u64", 910);
    msg->setString("composite.str", "1112");

    updater.addParamDesc(mReflector, mDescriptors);

    std::vector<C2Param::Index> indices;
    updater.getParamIndicesFromMessage(msg, &indices);
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2IntInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2LongInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2StringInfo::PARAM_TYPE; }));
    ASSERT_EQ(1, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2CompositeInfo::PARAM_TYPE; }));

    std::vector<std::unique_ptr<C2Param>> params;
    params.emplace_back(new C2CompositeInfo);
    ASSERT_EQ(0, CastParam<C2CompositeInfo>(params[0])->i32);
    ASSERT_EQ(0u, CastParam<C2CompositeInfo>(params[0])->u64);
    ASSERT_EQ(0, CastParam<C2CompositeInfo>(params[0])->str[0]);

    updater.updateParamsFromMessage(msg, &params);
    ASSERT_EQ(78, CastParam<C2CompositeInfo>(params[0])->i32);
    ASSERT_EQ(910u, CastParam<C2CompositeInfo>(params[0])->u64);
    ASSERT_EQ(0, strcmp("1112", CastParam<C2CompositeInfo>(params[0])->str));
}

TEST_F(ReflectedParamUpdaterTest, CompositePartialTest) {
    ReflectedParamUpdater updater;

    sp<AMessage> msg(new AMessage);
    msg->setInt32("composite.i32", 1314);
    msg->setString("composite.str", "1516");

    updater.addParamDesc(mReflector, mDescriptors);

    std::vector<C2Param::Index> indices;
    updater.getParamIndicesFromMessage(msg, &indices);
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2IntInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2LongInfo::PARAM_TYPE; }));
    ASSERT_EQ(0, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2StringInfo::PARAM_TYPE; }));
    ASSERT_EQ(1, std::count_if(indices.begin(), indices.end(),
            [](const auto &value) { return (uint32_t)value == C2CompositeInfo::PARAM_TYPE; }));

    std::vector<std::unique_ptr<C2Param>> params;
    params.emplace_back(new C2CompositeInfo);
    ASSERT_EQ(0, CastParam<C2CompositeInfo>(params[0])->i32);
    ASSERT_EQ(0u, CastParam<C2CompositeInfo>(params[0])->u64);
    ASSERT_EQ(0, CastParam<C2CompositeInfo>(params[0])->str[0]);

    updater.updateParamsFromMessage(msg, &params);
    ASSERT_EQ(1314, CastParam<C2CompositeInfo>(params[0])->i32);
    ASSERT_EQ(0u, CastParam<C2CompositeInfo>(params[0])->u64);
    ASSERT_EQ(0, strcmp("1516", CastParam<C2CompositeInfo>(params[0])->str));
}

} // namespace android
