// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test/unittests/profiler/heap-snapshot-utils.h"

#include <cstring>
#include <string>
#include <string_view>

#include "src/execution/isolate.h"
#include "src/heap/heap.h"
#include "src/profiler/heap-profiler.h"

namespace v8::internal {

const HeapGraphEdge* GetNamedEdge(const HeapEntry& entry, const char* name) {
  for (int i = 0; i < entry.children_count(); ++i) {
    const HeapGraphEdge* edge = entry.child(i);
    switch (edge->type()) {
      case HeapGraphEdge::kContextVariable:
      case HeapGraphEdge::kProperty:
      case HeapGraphEdge::kInternal:
      case HeapGraphEdge::kShortcut:
      case HeapGraphEdge::kWeak:
        if (edge->name() != nullptr && strcmp(edge->name(), name) == 0) {
          return edge;
        }
        break;
      case HeapGraphEdge::kElement:
      case HeapGraphEdge::kHidden:
        break;
    }
  }
  return nullptr;
}

const HeapGraphEdge* FindFirstEdgeTo(const HeapEntry& from,
                                     const HeapEntry& to) {
  for (int i = 0; i < from.children_count(); ++i) {
    const HeapGraphEdge* edge = from.child(i);
    if (edge->is_value()) continue;
    if (edge->to() == &to) return edge;
  }
  return nullptr;
}

const HeapEntry* GetEntryByName(HeapSnapshot* snapshot, const char* name) {
  for (const HeapEntry& entry : snapshot->entries()) {
    if (strcmp(entry.name(), name) == 0) return &entry;
  }
  return nullptr;
}

bool HasNamedEdge(const HeapEntry& entry, const char* name) {
  return GetNamedEdge(entry, name) != nullptr;
}

std::optional<int> GetIntEdge(const HeapEntry* node, const char* name) {
  const HeapGraphEdge* edge = GetNamedEdge(*node, name);
  if (!edge || !edge->is_value() ||
      edge->value()->type() != HeapSnapshotValue::kInt) {
    return std::nullopt;
  }
  return edge->value()->int_value();
}

std::optional<bool> GetBoolEdge(const HeapEntry* node, const char* name) {
  const HeapGraphEdge* edge = GetNamedEdge(*node, name);
  if (!edge || !edge->is_value() ||
      edge->value()->type() != HeapSnapshotValue::kBool) {
    return std::nullopt;
  }
  return edge->value()->bool_value();
}

std::optional<std::string_view> GetStringEdge(const HeapEntry* node,
                                              const char* name) {
  const HeapGraphEdge* edge = GetNamedEdge(*node, name);
  if (!edge || !edge->is_value() ||
      edge->value()->type() != HeapSnapshotValue::kString) {
    return std::nullopt;
  }
  return edge->value()->string_value();
}

const HeapEntry* GetEntryFor(Isolate* isolate, HeapSnapshot* snapshot,
                             Tagged<HeapObject> object) {
  return GetEntryFor(isolate, snapshot, object.address());
}

const HeapEntry* GetEntryFor(Isolate* isolate, HeapSnapshot* snapshot,
                             Address addr) {
  SnapshotObjectId id =
      isolate->heap()->heap_profiler()->heap_object_map()->FindEntry(addr);
  if (id == v8::HeapProfiler::kUnknownObjectId) return nullptr;
  return snapshot->GetEntryById(id);
}

const HeapEntry* GetEntryFor(Isolate* isolate, HeapSnapshot* snapshot,
                             const cppgc::internal::HeapObjectHeader& header) {
  Address header_address = reinterpret_cast<Address>(
      const_cast<cppgc::internal::HeapObjectHeader*>(&header));
  return GetEntryFor(isolate, snapshot, header_address);
}

}  // namespace v8::internal
