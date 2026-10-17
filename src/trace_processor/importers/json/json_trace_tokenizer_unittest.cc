/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/json/json_trace_tokenizer.h"

#include <cstring>
#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(JsonTraceTokenizerTest, ReadKeyIntValue) {
  const char* start = R"("Test": 01234, )";
  const char* middle = start + strlen(R"("Test": )");
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next), ReadKeyRes::kFoundKey);
  ASSERT_EQ(next, middle);
  ASSERT_EQ(key, "Test");
}

TEST(JsonTraceTokenizerTest, ReadKeyArrayValue) {
  const char* start = R"(, "key": [test], )";
  const char* middle = start + strlen(R"(, "key": )");
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next), ReadKeyRes::kFoundKey);
  ASSERT_EQ(next, middle);
  ASSERT_EQ(key, "key");
}

TEST(JsonTraceTokenizerTest, ReadKeyDictValue) {
  const char* start = R"("key2": {}})";
  const char* middle = start + strlen(R"("key2": )");
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next), ReadKeyRes::kFoundKey);
  ASSERT_EQ(next, middle);
  ASSERT_EQ(key, "key2");
}

TEST(JsonTraceTokenizerTest, ReadKeyEscaped) {
  const char* start = R"("key\n2": {}})";
  const char* middle = start + strlen(R"("key\n2": )");
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next), ReadKeyRes::kFoundKey);
  ASSERT_EQ(next, middle);
  ASSERT_EQ(key, "key\n2");
}

TEST(JsonTraceTokenizerTest, ReadKeyNeedMoreDataStartString) {
  const char* start = R"(")";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next),
            ReadKeyRes::kNeedsMoreData);
  ASSERT_EQ(next, nullptr);
}

TEST(JsonTraceTokenizerTest, ReadKeyNeedMoreDataMiddleString) {
  const char* start = R"("key)";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next),
            ReadKeyRes::kNeedsMoreData);
  ASSERT_EQ(next, nullptr);
}

TEST(JsonTraceTokenizerTest, ReadKeyNeedMoreDataNoValue) {
  const char* start = R"("key": )";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next),
            ReadKeyRes::kNeedsMoreData);
  ASSERT_EQ(next, nullptr);
}

TEST(JsonTraceTokenizerTest, ReadKeyEndOfDict) {
  const char* start = R"(      })";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string key;

  ASSERT_EQ(ReadOneJsonKey(start, end, &key, &next),
            ReadKeyRes::kEndOfDictionary);
  ASSERT_EQ(next, end);
}

TEST(JsonTraceTokenizerTest, ReadSystraceLine) {
  const char* start = R"(test one two\n   test again\n)";
  const char* middle = start + strlen(R"(test one two\n)");
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string line;

  ASSERT_EQ(ReadOneSystemTraceLine(start, end, &line, &next),
            ReadSystemLineRes::kFoundLine);
  ASSERT_EQ(next, middle);
  ASSERT_EQ(line, "test one two");
}

TEST(JsonTraceTokenizerTest, ReadSystraceLineEscaped) {
  const char* start = R"(test\t one two\n   test again\n)";
  const char* middle = start + strlen(R"(test\t one two\n)");
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string line;

  ASSERT_EQ(ReadOneSystemTraceLine(start, end, &line, &next),
            ReadSystemLineRes::kFoundLine);
  ASSERT_EQ(next, middle);
  ASSERT_EQ(line, "test\t one two");
}

TEST(JsonTraceTokenizerTest, ReadSystraceNeedMoreDataOnlyEscape) {
  const char* start = R"(test one two\)";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string line;

  ASSERT_EQ(ReadOneSystemTraceLine(start, end, &line, &next),
            ReadSystemLineRes::kNeedsMoreData);
  ASSERT_EQ(next, nullptr);
}

TEST(JsonTraceTokenizerTest, ReadSystraceEndOfData) {
  const char* start = R"(")";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  std::string line;

  ASSERT_EQ(ReadOneSystemTraceLine(start, end, &line, &next),
            ReadSystemLineRes::kEndOfSystemTrace);
  ASSERT_EQ(next, end);
}

TEST(JsonTraceTokenizerTest, MinifyJsonDropsWhitespaceOutsideStrings) {
  std::string out;
  ASSERT_TRUE(
      MinifyJson("{\n  \"a b\" : 1 ,\r\n\t\"c\": [ 1.5, true ,null ],"
                 " \"d\": { \"e\" : \" x \\\" y \" }, \"f\": false }",
                 &out));
  ASSERT_EQ(out,
            R"({"a b":1,"c":[1.5,true,null],"d":{"e":" x \" y "},"f":false})");

  ASSERT_TRUE(MinifyJson("{ }", &out));
  ASSERT_EQ(out, "{}");

  ASSERT_TRUE(MinifyJson(R"({"a":"\\", "b":"\\\"" , "c" :"\u00e9 "})", &out));
  ASSERT_EQ(out, R"({"a":"\\","b":"\\\"","c":"\u00e9 "})");
}

TEST(JsonTraceTokenizerTest, MinifyJsonMalformedJson) {
  std::string out;
  // Whitespace between two tokens which would otherwise merge is kept.
  ASSERT_TRUE(MinifyJson(R"({"a": [1  2, x  y]})", &out));
  ASSERT_EQ(out, R"({"a":[1 2,x y]})");
  ASSERT_TRUE(MinifyJson(R"({"a" 1, "b": 2 "c"})", &out));
  ASSERT_EQ(out, R"({"a"1,"b":2"c"})");
  // Invalid literals, which the parser rejects with an error depending on the
  // length of the input left, and unterminated strings are not minified.
  ASSERT_FALSE(MinifyJson(R"({"a": tr ue})", &out));
  ASSERT_FALSE(MinifyJson(R"({"a": f  })", &out));
  ASSERT_FALSE(MinifyJson(R"({"a": nul })", &out));
  ASSERT_FALSE(MinifyJson(R"({"a": truex })", &out));
  ASSERT_FALSE(MinifyJson(R"({"a": "x })", &out));
}

}  // namespace
}  // namespace perfetto::trace_processor
