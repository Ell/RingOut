// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"

namespace Memory::RollbackSnapshot
{

enum class ZeroMemoryEncoding : u8
{
  Full = 0,
  AllZero = 1,
};

inline bool IsAllZero(const u8* const data, const u32 size)
{
  return std::all_of(data, data + size, [](const u8 value) { return value == 0; });
}

// Rollback snapshots are internal, short-lived buffers. Fake VMEM is commonly
// entirely zero for GameCube titles, but omitting it unconditionally is unsafe:
// an MMU configuration or guest can use it. Encode the observed state instead.
// A single non-zero byte selects a lossless full copy for that checkpoint.
inline bool DoZeroAwareMemory(PointerWrap& p, u8* const data, const u32 size)
{
  ZeroMemoryEncoding encoding = ZeroMemoryEncoding::Full;
  if (!p.IsReadMode() && IsAllZero(data, size))
    encoding = ZeroMemoryEncoding::AllZero;

  p.Do(encoding);
  if (p.IsReadMode() && encoding != ZeroMemoryEncoding::Full &&
      encoding != ZeroMemoryEncoding::AllZero)
  {
    p.SetVerifyMode();
    return false;
  }

  if (encoding == ZeroMemoryEncoding::AllZero)
  {
    if (p.IsReadMode())
      std::memset(data, 0, size);
  }
  else
  {
    p.DoArray(data, size);
  }
  return !p.IsVerifyMode();
}

inline u32 AddressableRamBytes(const u32 allocated_size, const u32 guest_addressable_size)
{
  return std::min(allocated_size, guest_addressable_size);
}

inline void ZeroRamPadding(u8* const ram, const u32 allocated_size,
                           const u32 guest_addressable_size)
{
  const u32 addressable_size = AddressableRamBytes(allocated_size, guest_addressable_size);
  if (addressable_size < allocated_size)
    std::memset(ram + addressable_size, 0, allocated_size - addressable_size);
}

}  // namespace Memory::RollbackSnapshot
