#ifndef GOOGLE_UPB_UPB_MESSAGE_MERGE_H__
#define GOOGLE_UPB_UPB_MESSAGE_MERGE_H__

#include "upb/mem/arena.h"
#include "upb/message/message.h"
#include "upb/mini_table/extension_registry.h"
#include "upb/mini_table/message.h"

// Must be last.
#include "upb/port/def.inc"

#ifdef __cplusplus
extern "C" {
#endif

// Merges the contents of the source message `src` into the destination
// message `dst`.
//
// Both `dst` and `src` must conform to the schema and layout defined by `mt`.
// If the messages are defined by different (but compatible) minitables,
// encode and then decode, or use `upb_Message_Convert()` to first convert the
// source message to the destination minitable if applicable.
//
// Self-merge is not supported and will return false. This check guards
// against self-merging at any of the following levels:
// - The root messages (`dst` and `src` pointing to the same message)
// - Submessage fields recursively at any depth
// - Repeated fields (arrays)
// - Map fields
//
// Note: This function always performs a deep copy of submessages, strings,
// bytes, maps, and repeated fields (arrays). Newly copied values and structures
// are allocated on the destination message's arena.
UPB_NODISCARD UPB_API bool upb_Message_MergeFrom(
    upb_Message* dst, const upb_Message* src, const upb_MiniTable* mt,
    const upb_ExtensionRegistry* extreg, upb_Arena* arena);

#ifdef __cplusplus
} /* extern "C" */
#endif

#include "upb/port/undef.inc"
#endif  // GOOGLE_UPB_UPB_MESSAGE_MERGE_H__
