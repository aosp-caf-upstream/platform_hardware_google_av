/*
 * Copyright (C) 2016 The Android Open Source Project
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
#define LOG_TAG "C2UtilTest"

#include <gtest/gtest.h>

#include <C2Config.h>

#define __C2_GENERATE_GLOBAL_VARS__
#include <util/_C2MacroUtils.h>
#include <util/C2ParamUtils.h>

/** \file
 * Tests for vndk/util.
 */

/* --------------------------------------- _C2MacroUtils --------------------------------------- */

static_assert(0 == _C2_ARGC(), "should be 0");
static_assert(1 == _C2_ARGC(1), "should be 1");
static_assert(2 == _C2_ARGC(1, 2), "should be 2");
static_assert(64 == _C2_ARGC(
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
        49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64), "should be 64");

static_assert(0 == _C2_ARGC(,), "should be 0");
static_assert(1 == _C2_ARGC(1,), "should be 1");
static_assert(2 == _C2_ARGC(1, 2,), "should be 2");
static_assert(64 == _C2_ARGC(
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
        26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
        49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,), "should be 64");

/* ---------------------------------------- C2ParamUtils ---------------------------------------- */

class C2UtilTest : public ::testing::Test {
};

C2ENUM(Enum1, uint32_t,
    Enum1Value1,
    Enum1Value2,
    Enum1Value4 = Enum1Value2 + 2,
);

C2ENUM_CUSTOM_PREFIX(Enum2, uint32_t, "Enum",
    Enum2Value1,
    Enum2Value2,
    Enum2Value4 = Enum1Value2 + 2,
);

C2ENUM_CUSTOM_NAMES(Enum3, uint8_t,
    ({ { "value1", Enum3Value1 },
       { "value2", Enum3Value2 },
       { "value4", Enum3Value4 },
       { "invalid", Invalid } }),
    Enum3Value1,
    Enum3Value2,
    Enum3Value4 = Enum3Value2 + 2,
    Invalid,
);

TEST_F(C2UtilTest, EnumUtilsTest) {
    std::vector<std::pair<C2String, Enum3>> pairs ( { { "value1", Enum3Value1 },
      { "value2", Enum3Value2 },
      { "value4", Enum3Value4 },
      { "invalid", Invalid } });
    Enum3 e3;
    C2FieldDescriptor::namedValuesFor(e3);
}

TEST_F(C2UtilTest, ParamUtilsTest) {
    // upper case
    EXPECT_EQ("yes", C2ParamUtils::camelCaseToDashed("YES"));
    EXPECT_EQ("no", C2ParamUtils::camelCaseToDashed("NO"));
    EXPECT_EQ("yes-no", C2ParamUtils::camelCaseToDashed("YES_NO"));
    EXPECT_EQ("yes-no", C2ParamUtils::camelCaseToDashed("YES__NO"));
    EXPECT_EQ("a2dp", C2ParamUtils::camelCaseToDashed("A2DP"));
    EXPECT_EQ("mp2-ts", C2ParamUtils::camelCaseToDashed("MP2_TS"));
    EXPECT_EQ("block-2d", C2ParamUtils::camelCaseToDashed("BLOCK_2D"));
    EXPECT_EQ("mpeg-2-ts", C2ParamUtils::camelCaseToDashed("MPEG_2_TS"));
    EXPECT_EQ("_hidden-value", C2ParamUtils::camelCaseToDashed("_HIDDEN_VALUE"));
    EXPECT_EQ("__hidden-value2", C2ParamUtils::camelCaseToDashed("__HIDDEN_VALUE2"));
    EXPECT_EQ("__hidden-value-2", C2ParamUtils::camelCaseToDashed("__HIDDEN_VALUE_2"));

    // camel case
    EXPECT_EQ("yes", C2ParamUtils::camelCaseToDashed("Yes"));
    EXPECT_EQ("no", C2ParamUtils::camelCaseToDashed("No"));
    EXPECT_EQ("yes-no", C2ParamUtils::camelCaseToDashed("YesNo"));
    EXPECT_EQ("yes-no", C2ParamUtils::camelCaseToDashed("Yes_No"));
    EXPECT_EQ("mp2-ts", C2ParamUtils::camelCaseToDashed("MP2Ts"));
    EXPECT_EQ("block-2d", C2ParamUtils::camelCaseToDashed("Block2D"));
    EXPECT_EQ("mpeg-2-ts", C2ParamUtils::camelCaseToDashed("Mpeg2ts"));
    EXPECT_EQ("_hidden-value", C2ParamUtils::camelCaseToDashed("_HiddenValue"));
    EXPECT_EQ("__hidden-value-2", C2ParamUtils::camelCaseToDashed("__HiddenValue2"));

    // mixed case
    EXPECT_EQ("mp2t-s", C2ParamUtils::camelCaseToDashed("MP2T_s"));
    EXPECT_EQ("block-2d", C2ParamUtils::camelCaseToDashed("Block_2D"));
    EXPECT_EQ("block-2-d", C2ParamUtils::camelCaseToDashed("Block2_D"));
    EXPECT_EQ("mpeg-2-ts", C2ParamUtils::camelCaseToDashed("Mpeg_2ts"));
    EXPECT_EQ("mpeg-2-ts", C2ParamUtils::camelCaseToDashed("Mpeg_2_TS"));
    EXPECT_EQ("_hidden-value", C2ParamUtils::camelCaseToDashed("_Hidden__VALUE"));
    EXPECT_EQ("__hidden-value-2", C2ParamUtils::camelCaseToDashed("__HiddenValue_2"));
    EXPECT_EQ("_2", C2ParamUtils::camelCaseToDashed("_2"));
    EXPECT_EQ("__23", C2ParamUtils::camelCaseToDashed("__23"));
}

