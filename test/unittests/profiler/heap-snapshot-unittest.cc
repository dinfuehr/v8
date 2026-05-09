// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <json/json.h>

#include <string>
#include <string_view>

#include "src/profiler/heap-profiler.h"
#include "src/profiler/heap-snapshot-generator.h"
#include "test/unittests/profiler/heap-snapshot-utils.h"
#include "test/unittests/test-utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8::internal {

template <typename TMixin>
class WithHeapSnapshot : public TMixin {
 public:
  HeapSnapshot* TakeHeapSnapshot() {
    HeapProfiler* heap_profiler = TMixin::i_isolate()->heap()->heap_profiler();
    v8::HeapProfiler::HeapSnapshotOptions options;
    return heap_profiler->TakeSnapshot(options);
  }

  const Json::Value TakeHeapSnapshotJson() {
    v8::HeapProfiler::HeapSnapshotOptions options =
        HeapProfiler::GetDefaultHeapSnapshotOptionsForTestingUsage();
    HeapProfiler* heap_profiler = TMixin::i_isolate()->heap()->heap_profiler();
    std::string raw_json = heap_profiler->TakeSnapshotToString(options);

    Json::Value root;
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    CHECK(reader->parse(raw_json.c_str(), raw_json.c_str() + raw_json.size(),
                        &root, nullptr));
    return root;
  }

 protected:
  void TestMergedWrapperNode(v8::HeapProfiler::HeapSnapshotMode snapshot_mode);
};

using HeapSnapshotTest = WithHeapSnapshot<TestWithContext>;

namespace {

int FindJsonStringIndex(const Json::Value& array, const char* value) {
  for (Json::ArrayIndex i = 0; i < array.size(); ++i) {
    if (array[i].asString() == value) return static_cast<int>(i);
  }
  return -1;
}

std::string ToString(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value utf8(isolate, value);
  return *utf8;
}

std::string ToString(v8::Isolate* isolate, v8::Local<v8::String> value) {
  v8::String::Utf8Value utf8(isolate, value);
  return *utf8;
}

const v8::HeapGraphEdge* GetPublicEdgeByName(v8::Isolate* isolate,
                                             const v8::HeapGraphNode* node,
                                             v8::HeapGraphEdge::Type type,
                                             const char* name) {
  for (int i = 0; i < node->GetChildrenCount(); ++i) {
    const v8::HeapGraphEdge* edge = node->GetChild(i);
    if (edge->GetType() == type && ToString(isolate, edge->GetName()) == name) {
      return edge;
    }
  }
  return nullptr;
}

const v8::HeapGraphNode* GetPublicChildByEdgeName(v8::Isolate* isolate,
                                                  const v8::HeapGraphNode* node,
                                                  const char* name) {
  const v8::HeapGraphEdge* edge =
      GetPublicEdgeByName(isolate, node, v8::HeapGraphEdge::kInternal, name);
  return edge ? edge->GetToNode() : nullptr;
}

const v8::HeapGraphNode* GetPublicNativeContext(
    v8::Isolate* isolate, const v8::HeapSnapshot* snapshot) {
  for (int i = 0; i < snapshot->GetNodesCount(); ++i) {
    const v8::HeapGraphNode* node = snapshot->GetNode(i);
    if (ToString(isolate, node->GetName())
            .starts_with("system / NativeContext")) {
      return node;
    }
  }
  return nullptr;
}

const v8::HeapGraphNode* GetPublicGlobalObject(
    v8::Isolate* isolate, const v8::HeapSnapshot* snapshot) {
  const v8::HeapGraphNode* native_context =
      GetPublicNativeContext(isolate, snapshot);
  if (native_context == nullptr) return nullptr;
  return GetPublicChildByEdgeName(isolate, native_context, "global_object");
}

}  // namespace

TEST_F(HeapSnapshotTest, Empty) {
  Json::Value root = TakeHeapSnapshotJson();

  Json::Value meta = root["snapshot"]["meta"];
  CHECK_EQ(meta["node_fields"].size(), meta["node_types"].size());
  CHECK_EQ(meta["edge_fields"].size(), meta["edge_types"].size());
  CHECK_EQ(meta["value_fields"].size(), meta["value_types"].size());
  CHECK(root["values"].isArray());
}

TEST_F(HeapSnapshotTest, ValueTargetsUseValueTable) {
  Json::Value root = TakeHeapSnapshotJson();
  Json::Value meta = root["snapshot"]["meta"];

  const int edge_type_offset = FindJsonStringIndex(meta["edge_fields"], "type");
  const int edge_target_offset =
      FindJsonStringIndex(meta["edge_fields"], "to_node");
  const int value_type_offset =
      FindJsonStringIndex(meta["value_fields"], "type");
  const int value_value_offset =
      FindJsonStringIndex(meta["value_fields"], "value");
  const int value_fields_count = meta["value_fields"].size();

  ASSERT_GE(edge_type_offset, 0);
  ASSERT_GE(edge_target_offset, 0);
  ASSERT_GE(value_type_offset, 0);
  ASSERT_GE(value_value_offset, 0);
  ASSERT_GT(value_fields_count, 0);
  ASSERT_GT(root["values"].size(), 0u);

  EXPECT_EQ(-1,
            FindJsonStringIndex(meta["edge_types"][edge_type_offset], "value"));

  EXPECT_EQ("string", meta["value_types"][value_value_offset].asString());
  EXPECT_GE(FindJsonStringIndex(meta["value_types"][value_type_offset], "int"),
            0);
  EXPECT_GE(FindJsonStringIndex(meta["value_types"][value_type_offset], "bool"),
            0);
  EXPECT_GE(
      FindJsonStringIndex(meta["value_types"][value_type_offset], "double"), 0);
  EXPECT_GE(
      FindJsonStringIndex(meta["value_types"][value_type_offset], "string"), 0);

  const Json::Value edges = root["edges"];
  const int edge_fields_count = meta["edge_fields"].size();
  const int serialized_nodes_length = root["nodes"].size();
  for (Json::ArrayIndex i = 0; i < edges.size(); i += edge_fields_count) {
    const int target_index = edges[i + edge_target_offset].asInt();
    if (target_index < serialized_nodes_length) continue;
    const int value_index = target_index - serialized_nodes_length;
    EXPECT_EQ(0, value_index % value_fields_count);
    EXPECT_LT(value_index, static_cast<int>(root["values"].size()));
    EXPECT_TRUE(root["values"][value_index + value_value_offset].isString());
    return;
  }
  FAIL() << "Expected at least one value target";
}

TEST_F(HeapSnapshotTest, SmiTargetsUseValueTable) {
  RunJS("globalThis.smi = 1;");

  Json::Value root = TakeHeapSnapshotJson();
  Json::Value meta = root["snapshot"]["meta"];

  const int edge_target_offset =
      FindJsonStringIndex(meta["edge_fields"], "to_node");
  const int value_type_offset =
      FindJsonStringIndex(meta["value_fields"], "type");
  const int value_value_offset =
      FindJsonStringIndex(meta["value_fields"], "value");
  const int value_fields_count = meta["value_fields"].size();

  ASSERT_GE(edge_target_offset, 0);
  ASSERT_GE(value_type_offset, 0);
  ASSERT_GE(value_value_offset, 0);
  ASSERT_GT(value_fields_count, 0);
  ASSERT_GT(root["values"].size(), 0u);

  EXPECT_EQ("string", meta["value_types"][value_value_offset].asString());
  const int smi_value_type =
      FindJsonStringIndex(meta["value_types"][value_type_offset], "smi");
  ASSERT_GE(smi_value_type, 0);

  const Json::Value edges = root["edges"];
  const int edge_fields_count = meta["edge_fields"].size();
  const int serialized_nodes_length = root["nodes"].size();
  for (Json::ArrayIndex i = 0; i < edges.size(); i += edge_fields_count) {
    const int target_index = edges[i + edge_target_offset].asInt();
    if (target_index < serialized_nodes_length) continue;
    const int value_index = target_index - serialized_nodes_length;
    EXPECT_EQ(0, value_index % value_fields_count);
    EXPECT_LT(value_index, static_cast<int>(root["values"].size()));
    if (root["values"][value_index + value_type_offset].asInt() !=
        smi_value_type) {
      continue;
    }
    if (root["values"][value_index + value_value_offset].asString() == "1") {
      return;
    }
  }
  FAIL() << "Expected Smi value 1 as a value table entry";
}

TEST_F(HeapSnapshotTest, HeapNumberValueUsesDoubleValueTableEntry) {
  RunJS("globalThis.heap_number = 1.25;");

  Json::Value root = TakeHeapSnapshotJson();
  Json::Value meta = root["snapshot"]["meta"];

  const int value_type_offset =
      FindJsonStringIndex(meta["value_fields"], "type");
  const int value_value_offset =
      FindJsonStringIndex(meta["value_fields"], "value");
  const int value_fields_count = meta["value_fields"].size();

  ASSERT_GE(value_type_offset, 0);
  ASSERT_GE(value_value_offset, 0);
  ASSERT_GT(value_fields_count, 0);

  const int double_value_type =
      FindJsonStringIndex(meta["value_types"][value_type_offset], "double");
  ASSERT_GE(double_value_type, 0);

  const Json::Value values = root["values"];
  for (Json::ArrayIndex i = 0; i < values.size(); i += value_fields_count) {
    if (values[i + value_type_offset].asInt() == double_value_type &&
        values[i + value_value_offset].asString() == "1.25") {
      return;
    }
  }
  FAIL() << "Expected heap number value 1.25 as a double value table entry";
}

TEST_F(HeapSnapshotTest, PrimitiveValueApi) {
  v8::HandleScope scope(v8_isolate());
  const uint16_t two_byte_chars[] = {'a', 'b', 'c', 0x1234};
  v8::Local<v8::String> two_byte =
      v8::String::NewFromTwoByte(v8_isolate(), two_byte_chars,
                                 v8::NewStringType::kNormal,
                                 arraysize(two_byte_chars))
          .ToLocalChecked();
  v8_context()
      ->Global()
      ->Set(v8_context(), NewString("two_byte"), two_byte)
      .FromJust();
  RunJS("globalThis.obj = {};");

  const v8::HeapSnapshot* snapshot =
      v8_isolate()->GetHeapProfiler()->TakeHeapSnapshot();
  const v8::HeapGraphNode* global =
      GetPublicGlobalObject(v8_isolate(), snapshot);
  ASSERT_NE(nullptr, global);

  const v8::HeapGraphEdge* two_byte_edge = GetPublicEdgeByName(
      v8_isolate(), global, v8::HeapGraphEdge::kProperty, "two_byte");
  ASSERT_NE(nullptr, two_byte_edge);
  const v8::HeapGraphNode* two_byte_node = two_byte_edge->GetToNode();
  ASSERT_NE(nullptr, two_byte_node);

  const v8::HeapGraphEdge* length_edge = GetPublicEdgeByName(
      v8_isolate(), two_byte_node, v8::HeapGraphEdge::kInternal, "length");
  ASSERT_NE(nullptr, length_edge);
  EXPECT_EQ(nullptr, length_edge->GetToNode());
  const v8::HeapSnapshotValue* length_value = length_edge->GetValue();
  ASSERT_NE(nullptr, length_value);
  EXPECT_EQ(v8::HeapSnapshotValue::kInt, length_value->GetType());
  EXPECT_EQ(4, length_value->GetInt());

  const v8::HeapGraphEdge* two_byte_representation_edge = GetPublicEdgeByName(
      v8_isolate(), two_byte_node, v8::HeapGraphEdge::kInternal,
      "two_byte_representation");
  ASSERT_NE(nullptr, two_byte_representation_edge);
  EXPECT_EQ(nullptr, two_byte_representation_edge->GetToNode());
  const v8::HeapSnapshotValue* two_byte_representation_value =
      two_byte_representation_edge->GetValue();
  ASSERT_NE(nullptr, two_byte_representation_value);
  EXPECT_EQ(v8::HeapSnapshotValue::kBool,
            two_byte_representation_value->GetType());
  EXPECT_TRUE(two_byte_representation_value->GetBool());

  const v8::HeapGraphEdge* obj_edge = GetPublicEdgeByName(
      v8_isolate(), global, v8::HeapGraphEdge::kProperty, "obj");
  ASSERT_NE(nullptr, obj_edge);
  const v8::HeapGraphNode* obj = obj_edge->GetToNode();
  ASSERT_NE(nullptr, obj);

  const v8::HeapGraphEdge* map_edge = GetPublicEdgeByName(
      v8_isolate(), obj, v8::HeapGraphEdge::kInternal, "map");
  ASSERT_NE(nullptr, map_edge);
  EXPECT_EQ(nullptr, map_edge->GetValue());
  const v8::HeapGraphNode* map = map_edge->GetToNode();
  ASSERT_NE(nullptr, map);

  const v8::HeapGraphEdge* instance_type_name_edge = GetPublicEdgeByName(
      v8_isolate(), map, v8::HeapGraphEdge::kInternal, "instance_type_name");
  ASSERT_NE(nullptr, instance_type_name_edge);
  EXPECT_EQ(nullptr, instance_type_name_edge->GetToNode());
  const v8::HeapSnapshotValue* instance_type_name_value =
      instance_type_name_edge->GetValue();
  ASSERT_NE(nullptr, instance_type_name_value);
  EXPECT_EQ(v8::HeapSnapshotValue::kString,
            instance_type_name_value->GetType());
  EXPECT_EQ("JS_OBJECT_TYPE",
            ToString(v8_isolate(), instance_type_name_value->GetString()));
}

TEST_F(HeapSnapshotTest, StringTableRootsAreWeak) {
  HeapSnapshot* snapshot = TakeHeapSnapshot();
  const HeapEntry* string_table_entry =
      snapshot->gc_subroot(Root::kStringTable);
  ASSERT_NE(nullptr, string_table_entry);
  ASSERT_GT(string_table_entry->children_count(), 0);
  // All edges from the string table should be weak.
  for (int i = 0; i < string_table_entry->children_count(); ++i) {
    const HeapGraphEdge* edge = string_table_entry->child(i);
    EXPECT_EQ(HeapGraphEdge::kWeak, edge->type());
  }
}

namespace {}  // namespace

TEST_F(HeapSnapshotTest, PositionOnSharedFunctionInfo) {
  RunJS(
      "globalThis.lazy = function lazy(x) { return x - 1; }\n"
      "globalThis.compiled = function compiled(x) { ()=>x; return x + 1; }\n"
      "compiled(1)");
  HeapSnapshot* snapshot = TakeHeapSnapshot();

  const HeapEntry* compiled = nullptr;
  const HeapEntry* lazy = nullptr;
  for (const HeapEntry& entry : snapshot->entries()) {
    if (entry.type() == HeapEntry::kClosure) {
      if (strcmp(entry.name(), "compiled") == 0) {
        compiled = &entry;
      } else if (strcmp(entry.name(), "lazy") == 0) {
        lazy = &entry;
      }
    }
  }
  CHECK_NOT_NULL(compiled);
  CHECK_NOT_NULL(lazy);

  // Find references to shared function info.
  const HeapGraphEdge* compiled_sfi_edge = GetNamedEdge(*compiled, "shared");
  CHECK_NOT_NULL(compiled_sfi_edge);
  const HeapEntry* compiled_sfi = compiled_sfi_edge->to();

  const HeapGraphEdge* lazy_sfi_edge = GetNamedEdge(*lazy, "shared");
  CHECK_NOT_NULL(lazy_sfi_edge);
  const HeapEntry* lazy_sfi = lazy_sfi_edge->to();

  // Check that start_position and end_position edges exist on compiled_sfi.
  std::optional<int> start_position =
      GetIntEdge(compiled_sfi, "start_position");
  EXPECT_TRUE(start_position.has_value());
  EXPECT_EQ(92, start_position.value());

  std::optional<int> end_position = GetIntEdge(compiled_sfi, "end_position");
  EXPECT_TRUE(end_position.has_value());
  EXPECT_EQ(120, end_position.value());

  std::optional<int> lazy_start_position =
      GetIntEdge(lazy_sfi, "start_position");
  EXPECT_TRUE(lazy_start_position.has_value());
  EXPECT_EQ(31, lazy_start_position.value());

  std::optional<int> lazy_end_position = GetIntEdge(lazy_sfi, "end_position");
  EXPECT_TRUE(lazy_end_position.has_value());
  EXPECT_EQ(52, lazy_end_position.value());
}

TEST_F(HeapSnapshotTest, ContextGroupingByScopeInfo) {
  RunJS(
      "function factory() {\n"
      "  let x = 1;\n"
      "  return function closure() { return x; };\n"
      "}\n"
      "globalThis.f1 = factory();\n"
      "globalThis.f2 = factory();\n");
  HeapSnapshot* snapshot = TakeHeapSnapshot();

  const HeapEntry* closure = nullptr;
  for (const HeapEntry& entry : snapshot->entries()) {
    if (entry.type() == HeapEntry::kClosure &&
        strcmp(entry.name(), "closure") == 0) {
      closure = &entry;
      break;
    }
  }
  ASSERT_NE(nullptr, closure);

  const HeapGraphEdge* context_edge = GetNamedEdge(*closure, "context");
  ASSERT_NE(nullptr, context_edge);
  const HeapEntry* context_entry = context_edge->to();

  const HeapGraphEdge* scope_info_edge =
      GetNamedEdge(*context_entry, "scope_info");
  ASSERT_NE(nullptr, scope_info_edge);
  const HeapEntry* scope_info_entry = scope_info_edge->to();

  char expected_name[100];
  snprintf(expected_name, sizeof(expected_name), "system / Context / scope @%u",
           scope_info_entry->id());

  int count = 0;
  for (const HeapEntry& entry : snapshot->entries()) {
    if (strcmp(entry.name(), expected_name) == 0) {
      count++;
    }
  }
  EXPECT_EQ(count, 2);
}

TEST_F(HeapSnapshotTest, ScopeInfoProperties) {
  RunJS(
      "globalThis.f = function f_func(a, b) {\n"
      "  let x = a + b;\n"
      "  globalThis.g = function g_func(p1) { return x + p1; };\n"
      "}\n"
      "f(1, 2);\n"
      "g(3);");
  HeapSnapshot* snapshot = TakeHeapSnapshot();

  const HeapEntry* g_closure = nullptr;
  const HeapEntry* f_closure = nullptr;
  for (const HeapEntry& entry : snapshot->entries()) {
    if (entry.type() == HeapEntry::kClosure) {
      if (strcmp(entry.name(), "g_func") == 0) {
        g_closure = &entry;
      } else if (strcmp(entry.name(), "f_func") == 0) {
        f_closure = &entry;
      }
    }
  }
  ASSERT_NE(nullptr, g_closure);
  ASSERT_NE(nullptr, f_closure);

  // Check g_func's ScopeInfo
  {
    const HeapGraphEdge* sfi_edge = GetNamedEdge(*g_closure, "shared");
    ASSERT_NE(nullptr, sfi_edge);
    const HeapEntry* sfi = sfi_edge->to();

    const HeapGraphEdge* scope_info_edge =
        GetNamedEdge(*sfi, "name_or_scope_info");
    ASSERT_NE(nullptr, scope_info_edge);
    const HeapEntry* scope_info = scope_info_edge->to();

    std::optional<int> scope_type = GetIntEdge(scope_info, "scope_type");
    ASSERT_TRUE(scope_type.has_value());
    EXPECT_EQ(v8::internal::FUNCTION_SCOPE, scope_type.value());

    const HeapGraphEdge* scope_type_name_edge =
        GetNamedEdge(*scope_info, "scope_type_name");
    ASSERT_NE(nullptr, scope_type_name_edge);
    std::optional<std::string_view> scope_type_name =
        GetStringEdge(scope_info, "scope_type_name");
    ASSERT_TRUE(scope_type_name.has_value());
    EXPECT_EQ("FUNCTION_SCOPE", scope_type_name.value());

    std::optional<int> parameter_count =
        GetIntEdge(scope_info, "parameter_count");
    ASSERT_TRUE(parameter_count.has_value());
    EXPECT_EQ(1, parameter_count.value());

    std::optional<int> start_position =
        GetIntEdge(scope_info, "start_position");
    ASSERT_TRUE(start_position.has_value());
    EXPECT_EQ(88, start_position.value());

    std::optional<int> end_position = GetIntEdge(scope_info, "end_position");
    ASSERT_TRUE(end_position.has_value());
    EXPECT_EQ(111, end_position.value());

    std::optional<int> context_local_count =
        GetIntEdge(scope_info, "context_local_count");
    ASSERT_TRUE(context_local_count.has_value());
    EXPECT_EQ(0, context_local_count.value());

    // Check outer_scope_info points from g's ScopeInfo to f's ScopeInfo
    const HeapGraphEdge* f_sfi_edge = GetNamedEdge(*f_closure, "shared");
    ASSERT_NE(nullptr, f_sfi_edge);
    const HeapEntry* f_scope_info =
        GetNamedEdge(*f_sfi_edge->to(), "name_or_scope_info")->to();

    const HeapGraphEdge* outer_edge =
        GetNamedEdge(*scope_info, "outer_scope_info");
    ASSERT_NE(nullptr, outer_edge);
    EXPECT_EQ(f_scope_info, outer_edge->to());
  }

  // Check f_func's ScopeInfo
  {
    const HeapGraphEdge* sfi_edge = GetNamedEdge(*f_closure, "shared");
    ASSERT_NE(nullptr, sfi_edge);
    const HeapEntry* sfi = sfi_edge->to();

    const HeapGraphEdge* scope_info_edge =
        GetNamedEdge(*sfi, "name_or_scope_info");
    ASSERT_NE(nullptr, scope_info_edge);
    const HeapEntry* scope_info = scope_info_edge->to();

    std::optional<int> scope_type = GetIntEdge(scope_info, "scope_type");
    ASSERT_TRUE(scope_type.has_value());
    EXPECT_EQ(v8::internal::FUNCTION_SCOPE, scope_type.value());

    const HeapGraphEdge* scope_type_name_edge =
        GetNamedEdge(*scope_info, "scope_type_name");
    ASSERT_NE(nullptr, scope_type_name_edge);
    std::optional<std::string_view> scope_type_name =
        GetStringEdge(scope_info, "scope_type_name");
    ASSERT_TRUE(scope_type_name.has_value());
    EXPECT_EQ("FUNCTION_SCOPE", scope_type_name.value());

    std::optional<int> parameter_count =
        GetIntEdge(scope_info, "parameter_count");
    ASSERT_TRUE(parameter_count.has_value());
    EXPECT_EQ(2, parameter_count.value());

    std::optional<int> start_position =
        GetIntEdge(scope_info, "start_position");
    ASSERT_TRUE(start_position.has_value());
    EXPECT_EQ(30, start_position.value());

    std::optional<int> end_position = GetIntEdge(scope_info, "end_position");
    ASSERT_TRUE(end_position.has_value());
    EXPECT_EQ(114, end_position.value());

    std::optional<int> context_local_count =
        GetIntEdge(scope_info, "context_local_count");
    ASSERT_TRUE(context_local_count.has_value());
  }
}

TEST_F(HeapSnapshotTest, RemovedObjectIsDroppedFromHeapObjectsMapOnGC) {
  v8_flags.heap_snapshot_on_gc = 0;
  v8_flags.heap_snapshot_path = "/dev/null";

  HeapProfiler* heap_profiler = i_isolate()->heap()->heap_profiler();
  Factory* factory = i_isolate()->factory();
  DisableConservativeStackScanningScopeForTesting no_stack_scanning(
      i_isolate()->heap());

  Address tracked_address;
  SnapshotObjectId tracked_id;
  {
    HandleScope scope(i_isolate());
    IndirectHandle<JSObject> tracked =
        factory->NewJSObject(i_isolate()->object_function());

    InvokeMajorGC();
    tracked_address = tracked->address();
    tracked_id = heap_profiler->GetSnapshotObjectId(tracked);

    EXPECT_NE(v8::HeapProfiler::kUnknownObjectId, tracked_id);
    EXPECT_EQ(tracked_id,
              heap_profiler->heap_object_map()->FindEntry(tracked_address));
  }

  InvokeMajorGC();
  EXPECT_FALSE(heap_profiler->heap_object_map()->ContainsEntryWithIdForTesting(
      tracked_id));
  EXPECT_EQ(v8::HeapProfiler::kUnknownObjectId,
            heap_profiler->heap_object_map()->FindEntry(tracked_address));
}

TEST_F(HeapSnapshotTest,
       RemovedObjectIsDroppedFromHeapObjectsMapOnTakeHeapSnapshot) {
  HeapProfiler* heap_profiler = i_isolate()->heap()->heap_profiler();
  Factory* factory = i_isolate()->factory();
  DisableConservativeStackScanningScopeForTesting no_stack_scanning(
      i_isolate()->heap());

  Address tracked_address;
  SnapshotObjectId tracked_id;
  {
    HandleScope scope(i_isolate());
    IndirectHandle<JSObject> tracked =
        factory->NewJSObject(i_isolate()->object_function());

    TakeHeapSnapshot();
    tracked_address = tracked->address();
    tracked_id = heap_profiler->GetSnapshotObjectId(tracked);

    EXPECT_NE(v8::HeapProfiler::kUnknownObjectId, tracked_id);
    EXPECT_EQ(tracked_id,
              heap_profiler->heap_object_map()->FindEntry(tracked_address));
  }

  TakeHeapSnapshot();
  EXPECT_FALSE(heap_profiler->heap_object_map()->ContainsEntryWithIdForTesting(
      tracked_id));
  EXPECT_EQ(v8::HeapProfiler::kUnknownObjectId,
            heap_profiler->heap_object_map()->FindEntry(tracked_address));
}

TEST_F(HeapSnapshotTest, StringProperties) {
  v8_flags.heap_snapshot_string_limit = 512;
  // Re-initialize StringsStorage to pick up the new flag value.
  i_isolate()->heap()->heap_profiler()->DeleteAllSnapshots();

  Factory* factory = i_isolate()->factory();
  HandleScope scope(i_isolate());

  Handle<String> one_byte_str = factory->NewStringFromAsciiChecked("abc");

  base::uc16 two_byte_chars[] = {'a', 'b', 'c', 0x1234};
  Handle<String> two_byte_str =
      factory
          ->NewStringFromTwoByte(base::Vector<const base::uc16>(
              two_byte_chars, arraysize(two_byte_chars)))
          .ToHandleChecked();

  std::vector<uint8_t> long_chars(600, 'a');
  Handle<String> truncated_str =
      factory
          ->NewStringFromOneByte(
              base::Vector<const uint8_t>(long_chars.data(), long_chars.size()))
          .ToHandleChecked();

  std::vector<uint8_t> cons_chars_a(100, 'a');
  std::vector<uint8_t> cons_chars_b(100, 'b');
  Handle<String> cons_part_a =
      factory
          ->NewStringFromOneByte(base::Vector<const uint8_t>(
              cons_chars_a.data(), cons_chars_a.size()))
          .ToHandleChecked();
  Handle<String> cons_part_b =
      factory
          ->NewStringFromOneByte(base::Vector<const uint8_t>(
              cons_chars_b.data(), cons_chars_b.size()))
          .ToHandleChecked();
  Handle<String> cons_str =
      factory->NewConsString(cons_part_a, cons_part_b).ToHandleChecked();

  Handle<String> sliced_parent =
      factory->NewStringFromAsciiChecked("abcdefghijklmnopqrstuvwxyz");
  Handle<String> sliced_str = factory->NewProperSubString(sliced_parent, 2, 17);

  HeapSnapshot* snapshot = TakeHeapSnapshot();

  const HeapEntry* one_byte = GetEntryFor(i_isolate(), snapshot, *one_byte_str);
  const HeapEntry* two_byte = GetEntryFor(i_isolate(), snapshot, *two_byte_str);
  const HeapEntry* truncated =
      GetEntryFor(i_isolate(), snapshot, *truncated_str);
  const HeapEntry* cons = GetEntryFor(i_isolate(), snapshot, *cons_str);
  const HeapEntry* sliced = GetEntryFor(i_isolate(), snapshot, *sliced_str);

  ASSERT_NE(nullptr, one_byte);
  EXPECT_EQ(3, GetIntEdge(one_byte, "length"));
  EXPECT_FALSE(GetBoolEdge(one_byte, "truncated").has_value());
  EXPECT_FALSE(GetBoolEdge(one_byte, "two_byte_representation").has_value());

  ASSERT_NE(nullptr, two_byte);
  EXPECT_EQ(4, GetIntEdge(two_byte, "length"));
  EXPECT_TRUE(GetBoolEdge(two_byte, "two_byte_representation").value());

  ASSERT_NE(nullptr, truncated);
  EXPECT_EQ(600, GetIntEdge(truncated, "length"));
  EXPECT_TRUE(GetBoolEdge(truncated, "truncated").value());
  EXPECT_EQ(512u, strlen(truncated->name()));

  ASSERT_NE(nullptr, cons);
  EXPECT_EQ(200, GetIntEdge(cons, "length"));
  EXPECT_EQ(HeapEntry::kConsString, cons->type());
  EXPECT_TRUE(HasNamedEdge(*cons, "first"));
  EXPECT_TRUE(HasNamedEdge(*cons, "second"));

  ASSERT_NE(nullptr, sliced);
  EXPECT_EQ(15, GetIntEdge(sliced, "length"));
  EXPECT_EQ(HeapEntry::kSlicedString, sliced->type());
  EXPECT_TRUE(HasNamedEdge(*sliced, "parent"));
}

TEST_F(HeapSnapshotTest, ScriptName) {
  v8::HandleScope scope(v8_isolate());
  v8::Local<v8::Context> context = v8_context();

  // 1. Anonymous script
  RunJS("const anonymous_script_var = 1;");

  // 2. Named script
  v8::Local<v8::String> source = NewString("const named_script_var = 2;");
  v8::Local<v8::String> origin_url = NewString("foo.js");
  v8::Local<v8::Script> v8_script =
      CompileWithOrigin(source, origin_url, false);
  std::ignore = v8_script->Run(context);

  // Extract the internal Script object for the named script.
  i::Handle<i::JSFunction> fun = v8::Utils::OpenHandle(*v8_script);
  Tagged<Object> script_obj = fun->shared()->script();
  ASSERT_TRUE(IsScript(script_obj));
  Tagged<Script> script = Cast<Script>(script_obj);

  HeapSnapshot* snapshot = TakeHeapSnapshot();

  // Verify the named Script entry in the snapshot.
  const HeapEntry* entry = GetEntryFor(i_isolate(), snapshot, script);
  ASSERT_NE(nullptr, entry);
  EXPECT_EQ(HeapEntry::kCode, entry->type());
  EXPECT_STREQ("system / Script / foo.js", entry->name());

  // Verify that at least one anonymous Script entry exists.
  const HeapEntry* anonymous_entry =
      GetEntryByName(snapshot, "system / Script");
  ASSERT_NE(nullptr, anonymous_entry);
  EXPECT_EQ(HeapEntry::kCode, anonymous_entry->type());
}

TEST_F(HeapSnapshotTest, LongScriptName) {
  v8::HandleScope scope(v8_isolate());
  v8::Local<v8::Context> context = v8_context();

  // Set string limit to unlimited to allow very long names.
  v8_flags.heap_snapshot_string_limit = 0;
  // Re-initialize StringsStorage to pick up the new flag value.
  i_isolate()->heap()->heap_profiler()->DeleteAllSnapshots();

  // Create a 5000-character script name.
  std::string long_name_str(5000, 'a');
  v8::Local<v8::String> source = NewString("const long_script_var = 3;");
  v8::Local<v8::String> origin_url = NewString(long_name_str.c_str());
  v8::Local<v8::Script> v8_script =
      CompileWithOrigin(source, origin_url, false);
  std::ignore = v8_script->Run(context);

  // Extract the internal Script object.
  i::Handle<i::JSFunction> fun = v8::Utils::OpenHandle(*v8_script);
  Tagged<Object> script_obj = fun->shared()->script();
  ASSERT_TRUE(IsScript(script_obj));
  Tagged<Script> script = Cast<Script>(script_obj);

  HeapSnapshot* snapshot = TakeHeapSnapshot();

  // Verify the Script entry in the snapshot.
  const HeapEntry* entry = GetEntryFor(i_isolate(), snapshot, script);
  ASSERT_NE(nullptr, entry);
  EXPECT_EQ(HeapEntry::kCode, entry->type());

  // The name should start with the prefix, but be truncated to 4095 characters.
  std::string expected_prefix = "system / Script / ";
  EXPECT_EQ(0, strncmp(entry->name(), expected_prefix.c_str(),
                       expected_prefix.length()));
  EXPECT_EQ(4095u, strlen(entry->name()));
  // It should NOT be the raw format string.
  EXPECT_STRNE("%s / %s", entry->name());
}

}  // namespace v8::internal
