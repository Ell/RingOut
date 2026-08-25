// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/RollbackMemorySnapshot.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "rollback_memory_snapshot_test:%d: %s\n", __LINE__, \
                   #condition);                                                \
      return false;                                                            \
    }                                                                          \
  } while (false)

bool TestAllZeroEncodingAndRestore() {
  std::array<u8, 64> memory{};
  std::array<u8, 128> buffer{};
  u8 *write = buffer.data();
  PointerWrap writer(&write, buffer.size(), PointerWrap::Mode::Write);
  CHECK(Memory::RollbackSnapshot::DoZeroAwareMemory(writer, memory.data(),
                                                    memory.size()));
  CHECK(write - buffer.data() == 1);
  CHECK(buffer[0] ==
        static_cast<u8>(Memory::RollbackSnapshot::ZeroMemoryEncoding::AllZero));

  memory.fill(0xab);
  u8 *read = buffer.data();
  PointerWrap reader(&read, 1, PointerWrap::Mode::Read);
  CHECK(Memory::RollbackSnapshot::DoZeroAwareMemory(reader, memory.data(),
                                                    memory.size()));
  CHECK(std::all_of(memory.begin(), memory.end(),
                    [](const u8 value) { return value == 0; }));
  return true;
}

bool TestNonzeroEncodingIsLossless() {
  std::array<u8, 64> memory{};
  for (std::size_t i = 0; i < memory.size(); ++i)
    memory[i] = static_cast<u8>((i * 17) & 0xff);
  const auto expected = memory;

  std::array<u8, 128> buffer{};
  u8 *write = buffer.data();
  PointerWrap writer(&write, buffer.size(), PointerWrap::Mode::Write);
  CHECK(Memory::RollbackSnapshot::DoZeroAwareMemory(writer, memory.data(),
                                                    memory.size()));
  CHECK(write - buffer.data() ==
        static_cast<std::ptrdiff_t>(memory.size() + 1));
  CHECK(buffer[0] ==
        static_cast<u8>(Memory::RollbackSnapshot::ZeroMemoryEncoding::Full));

  memory.fill(0);
  u8 *read = buffer.data();
  PointerWrap reader(&read, memory.size() + 1, PointerWrap::Mode::Read);
  CHECK(Memory::RollbackSnapshot::DoZeroAwareMemory(reader, memory.data(),
                                                    memory.size()));
  CHECK(memory == expected);
  return true;
}

bool TestInvalidEncodingFailsClosed() {
  std::array<u8, 8> memory{};
  std::array<u8, 1> buffer{0xff};
  u8 *read = buffer.data();
  PointerWrap reader(&read, buffer.size(), PointerWrap::Mode::Read);
  CHECK(!Memory::RollbackSnapshot::DoZeroAwareMemory(reader, memory.data(),
                                                     memory.size()));
  CHECK(reader.IsVerifyMode());
  return true;
}

bool TestRamPaddingIsReconstructed() {
  std::array<u8, 32> ram{};
  ram.fill(0x7d);
  CHECK(Memory::RollbackSnapshot::AddressableRamBytes(ram.size(), 24) == 24);
  Memory::RollbackSnapshot::ZeroRamPadding(ram.data(), ram.size(), 24);
  CHECK(std::all_of(ram.begin(), ram.begin() + 24,
                    [](const u8 value) { return value == 0x7d; }));
  CHECK(std::all_of(ram.begin() + 24, ram.end(),
                    [](const u8 value) { return value == 0; }));
  return true;
}

} // namespace

int main() {
  return TestAllZeroEncodingAndRestore() && TestNonzeroEncodingIsLossless() &&
                 TestInvalidEncodingFailsClosed() &&
                 TestRamPaddingIsReconstructed()
             ? 0
             : 1;
}
