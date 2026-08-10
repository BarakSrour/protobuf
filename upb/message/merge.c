#include "upb/message/merge.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "upb/base/descriptor_constants.h"
#include "upb/base/string_view.h"
#include "upb/mem/arena.h"
#include "upb/message/accessors.h"
#include "upb/message/array.h"
#include "upb/message/copy.h"
#include "upb/message/internal/accessors.h"
#include "upb/message/internal/array.h"
#include "upb/message/internal/extension.h"
#include "upb/message/internal/message.h"
#include "upb/message/internal/types.h"
#include "upb/message/map.h"
#include "upb/message/message.h"
#include "upb/message/unknown_fields.h"
#include "upb/mini_table/extension.h"
#include "upb/mini_table/extension_registry.h"
#include "upb/mini_table/field.h"
#include "upb/mini_table/message.h"
#include "upb/wire/decode.h"

// Must be last.
#include "upb/port/def.inc"

static bool upb_Merge_CloneStringView(upb_StringView str,
                                      upb_StringView* cloned,
                                      upb_Arena* arena) {
  if (str.size == 0) {
    *cloned = upb_StringView_FromDataAndSize(NULL, 0);
    return true;
  }
  void* cloned_data = upb_Arena_Malloc(arena, str.size);
  if (cloned_data == NULL) {
    return false;
  }
  memcpy(cloned_data, str.data, str.size);
  *cloned = upb_StringView_FromDataAndSize(cloned_data, str.size);
  return true;
}

static bool upb_Message_Merge_CloneMessageValue(upb_MessageValue* value,
                                                upb_CType value_type,
                                                const upb_MiniTable* sub,
                                                upb_Arena* arena) {
  switch (value_type) {
    case kUpb_CType_Bool:
    case kUpb_CType_Float:
    case kUpb_CType_Int32:
    case kUpb_CType_UInt32:
    case kUpb_CType_Enum:
    case kUpb_CType_Double:
    case kUpb_CType_Int64:
    case kUpb_CType_UInt64:
      return true;
    case kUpb_CType_String:
    case kUpb_CType_Bytes: {
      upb_StringView cloned;
      if (!upb_Merge_CloneStringView(value->str_val, &cloned, arena)) {
        return false;
      }
      value->str_val = cloned;
      return true;
    } break;
    case kUpb_CType_Message: {
      UPB_ASSERT(sub);
      const upb_Message* source = value->msg_val;
      if (source == NULL) {
        value->msg_val = NULL;
        return true;
      }
      upb_Message* clone = upb_Message_DeepClone(source, sub, arena);
      value->msg_val = clone;
      return clone != NULL;
    } break;
  }
  UPB_UNREACHABLE();
}

static bool upb_Message_MergeFromInternal(upb_Message* dst,
                                          const upb_Message* src,
                                          const upb_MiniTable* mt,
                                          const upb_ExtensionRegistry* extreg,
                                          upb_Arena* arena, int depth);

static bool upb_Array_Merge(upb_Array** dst_arr_ptr, const upb_Array* src_arr,
                            const upb_MiniTableField* field,
                            const upb_MiniTable* sub_mt, upb_Arena* arena) {
  if (src_arr == NULL) return true;

  size_t src_size = upb_Array_Size(src_arr);
  if (src_size == 0) return true;

  upb_Array* dst_arr = *dst_arr_ptr;
  if (dst_arr == src_arr) return false;
  if (dst_arr == NULL) {
    upb_Array* cloned_arr = upb_Array_DeepClone(
        src_arr, upb_MiniTableField_CType(field), sub_mt, arena);
    if (cloned_arr == NULL) return false;
    *dst_arr_ptr = cloned_arr;
  } else {
    size_t dst_size = upb_Array_Size(dst_arr);
    if (!upb_Array_Resize(dst_arr, dst_size + src_size, arena)) {
      return false;
    }
    upb_CType type = upb_MiniTableField_CType(field);
    for (size_t k = 0; k < src_size; ++k) {
      upb_MessageValue val = upb_Array_Get(src_arr, k);
      if (!upb_Message_Merge_CloneMessageValue(&val, type, sub_mt, arena)) {
        return false;
      }
      upb_Array_Set(dst_arr, dst_size + k, val);
    }
  }
  return true;
}

static bool upb_Message_MergeExtension(upb_Message* dst,
                                       const upb_MiniTable* mt,
                                       const upb_MiniTableExtension* ext_def,
                                       upb_MessageValue msg_ext_val,
                                       const upb_ExtensionRegistry* extreg,
                                       upb_Arena* arena, int depth,
                                       upb_TaggedAuxType tag) {
  const upb_MiniTableField* ext_field = &ext_def->UPB_PRIVATE(field);

  const upb_MiniTableExtension* dst_ext_def = NULL;
  if (tag == kUpb_TaggedAuxType_NonCanonicalExtension) {
    if (extreg != NULL) {
      dst_ext_def = upb_ExtensionRegistry_Lookup(
          extreg, mt, upb_MiniTableExtension_Number(ext_def));
    }
  } else {
    dst_ext_def = ext_def;
  }

  upb_Extension* dst_ext = NULL;
  upb_TaggedAuxType dst_tag = tag;

  if (dst_ext_def != NULL) {
    dst_tag = kUpb_TaggedAuxType_CanonicalExtension;
    const upb_Extension* existing_ext_const =
        UPB_PRIVATE(_upb_Message_Getext)(dst, dst_ext_def);
    dst_ext = (upb_Extension*)existing_ext_const;

    if (dst_ext == NULL) {
      dst_ext = UPB_PRIVATE(_upb_Message_GetOrCreateExtensionWithTag)(
          dst, dst_ext_def, arena, dst_tag);
      if (!dst_ext) return false;

      if (upb_MiniTableField_IsScalar(ext_field)) {
        dst_ext->data = msg_ext_val;
        const upb_MiniTable* sub_mt =
            upb_MiniTableExtension_GetSubMessage(dst_ext_def);
        if (!upb_Message_Merge_CloneMessageValue(
                &dst_ext->data, upb_MiniTableField_CType(ext_field), sub_mt,
                arena)) {
          return false;
        }
      } else {
        upb_Array* dst_arr = NULL;
        const upb_MiniTable* sub_mt =
            upb_MiniTableExtension_GetSubMessage(dst_ext_def);
        if (!upb_Array_Merge(&dst_arr, (const upb_Array*)msg_ext_val.array_val,
                             ext_field, sub_mt, arena)) {
          return false;
        }
        dst_ext->data.array_val = dst_arr;
      }
    } else {
      if (upb_MiniTableField_IsScalar(ext_field)) {
        if (upb_MiniTableField_CType(ext_field) == kUpb_CType_Message) {
          const upb_MiniTable* sub_mt =
              upb_MiniTableExtension_GetSubMessage(dst_ext_def);
          if (dst_ext->data.msg_val != NULL) {
            if (!upb_Message_MergeFromInternal(
                    (upb_Message*)dst_ext->data.msg_val, msg_ext_val.msg_val,
                    sub_mt, extreg, arena, depth)) {
              return false;
            }
          } else if (msg_ext_val.msg_val != NULL) {
            upb_Message* cloned_sub =
                upb_Message_DeepClone(msg_ext_val.msg_val, sub_mt, arena);
            if (!cloned_sub) return false;
            dst_ext->data.msg_val = cloned_sub;
          }
        } else if (upb_MiniTableField_CType(ext_field) == kUpb_CType_String ||
                   upb_MiniTableField_CType(ext_field) == kUpb_CType_Bytes) {
          upb_StringView cloned_str;
          if (!upb_Merge_CloneStringView(msg_ext_val.str_val, &cloned_str,
                                         arena)) {
            return false;
          }
          dst_ext->data.str_val = cloned_str;
        } else {
          dst_ext->data = msg_ext_val;
        }
      } else {
        upb_Array* dst_arr = (upb_Array*)dst_ext->data.array_val;
        const upb_MiniTable* sub_mt =
            upb_MiniTableExtension_GetSubMessage(dst_ext_def);
        if (!upb_Array_Merge(&dst_arr, (const upb_Array*)msg_ext_val.array_val,
                             ext_field, sub_mt, arena)) {
          return false;
        }
        dst_ext->data.array_val = dst_arr;
      }
    }
  } else {
    dst_ext = UPB_PRIVATE(_upb_Message_GetOrCreateExtensionWithTag)(
        dst, ext_def, arena, dst_tag);
    if (!dst_ext) return false;

    if (upb_MiniTableField_IsScalar(ext_field)) {
      dst_ext->data = msg_ext_val;
      const upb_MiniTable* sub_mt =
          upb_MiniTableExtension_GetSubMessage(ext_def);
      if (!upb_Message_Merge_CloneMessageValue(
              &dst_ext->data, upb_MiniTableField_CType(ext_field), sub_mt,
              arena)) {
        return false;
      }
    } else {
      upb_Array* dst_arr = NULL;
      const upb_MiniTable* sub_mt =
          upb_MiniTableExtension_GetSubMessage(ext_def);
      if (!upb_Array_Merge(&dst_arr, (const upb_Array*)msg_ext_val.array_val,
                           ext_field, sub_mt, arena)) {
        return false;
      }
      dst_ext->data.array_val = dst_arr;
    }
  }

  return true;
}

static bool upb_Message_MergeArray(upb_Message* dst, const upb_Message* src,
                                   const upb_MiniTableField* field,
                                   upb_Arena* arena) {
  const upb_Array* src_arr = upb_Message_GetArray(src, field);
  UPB_ASSERT(src_arr != NULL);
  upb_Array* dst_arr = upb_Message_GetMutableArray(dst, field);
  const upb_MiniTable* sub_mt =
      upb_MiniTableField_CType(field) == kUpb_CType_Message
          ? upb_MiniTable_GetSubMessageTable(field)
          : NULL;

  if (!upb_Array_Merge(&dst_arr, src_arr, field, sub_mt, arena)) {
    return false;
  }

  if (upb_Message_GetArray(dst, field) == NULL && dst_arr != NULL) {
    upb_Message_SetBaseFieldArray(dst, field, dst_arr, sub_mt);
  }
  return true;
}

static bool upb_Message_MergeMap(upb_Message* dst, const upb_Message* src,
                                 const upb_MiniTableField* field,
                                 const upb_ExtensionRegistry* extreg,
                                 upb_Arena* arena, int depth) {
  const upb_Map* src_map = upb_Message_GetMap(src, field);
  UPB_ASSERT(src_map != NULL && upb_Map_Size(src_map) > 0);
  const upb_MiniTable* map_entry_mt = upb_MiniTable_MapEntrySubMessage(field);
  const upb_MiniTableField* value_field = upb_MiniTable_MapValue(map_entry_mt);
  upb_CType value_type = upb_MiniTableField_CType(value_field);
  const upb_MiniTable* value_sub =
      value_type == kUpb_CType_Message
          ? upb_MiniTable_GetSubMessageTable(value_field)
          : NULL;

  upb_Map* dst_map = upb_Message_GetMutableMap(dst, field);
  if (dst_map == src_map) return false;
  if (dst_map == NULL) {
    const upb_MiniTableField* key_field = upb_MiniTable_MapKey(map_entry_mt);
    upb_Map* cloned_map =
        upb_Map_DeepClone(src_map, upb_MiniTableField_CType(key_field),
                          value_type, map_entry_mt, arena);
    if (cloned_map == NULL) return false;
    upb_Message_SetBaseFieldMap(dst, field, cloned_map, map_entry_mt);
  } else {
    upb_MessageValue key, val;
    size_t iter = kUpb_Map_Begin;
    while (upb_Map_Next(src_map, &key, &val, &iter)) {
      upb_MessageValue dst_val;
      if (upb_Map_Get(dst_map, key, &dst_val)) {
        if (value_type == kUpb_CType_Message) {
          upb_Message* dst_sub = (upb_Message*)dst_val.msg_val;
          if (dst_sub == NULL) {
            upb_MessageValue cloned_val = val;
            if (!upb_Message_Merge_CloneMessageValue(&cloned_val, value_type,
                                                     value_sub, arena)) {
              return false;
            }
            if (!upb_Map_Set(dst_map, key, cloned_val, arena)) {
              return false;
            }
          } else {
            if (!upb_Message_MergeFromInternal(dst_sub, val.msg_val, value_sub,
                                               extreg, arena, depth)) {
              return false;
            }
          }
        } else {
          upb_MessageValue cloned_val = val;
          if (!upb_Message_Merge_CloneMessageValue(&cloned_val, value_type,
                                                   value_sub, arena)) {
            return false;
          }
          if (!upb_Map_Set(dst_map, key, cloned_val, arena)) {
            return false;
          }
        }
      } else {
        upb_MessageValue cloned_val = val;
        if (!upb_Message_Merge_CloneMessageValue(&cloned_val, value_type,
                                                 value_sub, arena)) {
          return false;
        }
        if (!upb_Map_Set(dst_map, key, cloned_val, arena)) {
          return false;
        }
      }
    }
  }
  return true;
}

static bool upb_Message_MergeFromInternal(upb_Message* dst,
                                          const upb_Message* src,
                                          const upb_MiniTable* mt,
                                          const upb_ExtensionRegistry* extreg,
                                          upb_Arena* arena, int depth) {
  // Self-merge is disallowed.
  if (dst == NULL || dst == src) return false;
  // If src is NULL, we don't need to merge anything.
  if (src == NULL) return true;
  if (--depth == 0) return false;

  const upb_MiniTableField* field = NULL;
  uintptr_t iter = kUpb_Message_SerializableFieldBegin;
  while (upb_Message_NextSerializableField(src, mt, &field, &iter)) {
    if (upb_MiniTableField_IsScalar(field)) {
      if (upb_MiniTableField_CType(field) == kUpb_CType_Message) {
        const upb_Message* src_sub = upb_Message_GetMessage(src, field);
        if (src_sub != NULL) {
          const upb_MiniTable* sub_mt = upb_MiniTable_GetSubMessageTable(field);
          upb_Message* dst_sub = upb_Message_GetMutableMessage(dst, field);
          if (dst_sub != NULL) {
            if (!upb_Message_MergeFromInternal(dst_sub, src_sub, sub_mt, extreg,
                                               arena, depth)) {
              return false;
            }
          } else {
            upb_Message* cloned_sub =
                upb_Message_DeepClone(src_sub, sub_mt, arena);
            if (!cloned_sub) return false;
            upb_Message_SetBaseFieldMessage(dst, field, cloned_sub);
          }
        }
      } else if (upb_MiniTableField_CType(field) == kUpb_CType_String ||
                 upb_MiniTableField_CType(field) == kUpb_CType_Bytes) {
        upb_StringView empty_string = upb_StringView_FromDataAndSize(NULL, 0);
        upb_StringView str = upb_Message_GetString(src, field, empty_string);
        upb_StringView cloned_str;
        if (!upb_Merge_CloneStringView(str, &cloned_str, arena)) {
          return false;
        }
        upb_Message_SetBaseFieldString(dst, field, cloned_str);
      } else {
        UPB_PRIVATE(_upb_MiniTableField_DataCopy)
        (field, UPB_PRIVATE(_upb_Message_MutableDataPtr)(dst, field),
         UPB_PRIVATE(_upb_Message_DataPtr)(src, field));
        if (upb_MiniTableField_HasPresence(field)) {
          UPB_PRIVATE(_upb_Message_SetPresence)(dst, field);
        }
      }
    } else if (upb_MiniTableField_IsArray(field)) {
      if (!upb_Message_MergeArray(dst, src, field, arena)) {
        return false;
      }
    } else if (upb_MiniTableField_IsMap(field)) {
      if (!upb_Message_MergeMap(dst, src, field, extreg, arena, depth)) {
        return false;
      }
    }
  }

  // Merge extensions (canonical extensions only).
  const upb_MiniTableExtension* ext = NULL;
  upb_MessageValue ext_val;
  uintptr_t ext_iter = kUpb_Message_ExtensionBegin;
  while (upb_Message_NextExtension(src, &ext, &ext_val, &ext_iter)) {
    if (!upb_Message_MergeExtension(dst, mt, ext, ext_val, extreg, arena, depth,
                                    kUpb_TaggedAuxType_CanonicalExtension)) {
      return false;
    }
  }

  // Merge unknown fields & non-canonical extensions.
  uintptr_t unknown_iter = kUpb_Message_UnknownBegin;
  upb_MessageUnknown unknown;
  while (upb_Message_NextUnknown2(src, &unknown, &unknown_iter)) {
    if (unknown.type == kUpb_MessageUnknownType_StringView) {
      upb_StringView data = unknown.value.bytes;
      upb_DecodeStatus d_status = upb_Decode(data.data, data.size, dst, mt,
                                             extreg, /*options=*/0, arena);
      if (d_status != kUpb_DecodeStatus_Ok) {
        return false;
      }
    } else {
      UPB_ASSERT(unknown.type == kUpb_MessageUnknownType_NonCanonicalExtension);
      const upb_Extension* ext = unknown.value.extension;
      if (!upb_Message_MergeExtension(
              dst, mt, ext->ext, ext->data, extreg, arena, depth,
              kUpb_TaggedAuxType_NonCanonicalExtension)) {
        return false;
      }
    }
  }
  return true;
}

bool upb_Message_MergeFrom(upb_Message* dst, const upb_Message* src,
                           const upb_MiniTable* mt,
                           const upb_ExtensionRegistry* extreg,
                           upb_Arena* arena) {
  return upb_Message_MergeFromInternal(dst, src, mt, extreg, arena,
                                       /*depth=*/100);
}
