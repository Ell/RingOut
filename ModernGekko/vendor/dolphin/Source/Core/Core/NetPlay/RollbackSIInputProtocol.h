// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace NetPlay
{

// The wire protocol identifies the input slot to which a sample is scheduled.
// Its emulated frame and poll ordinal are deliberately not accepted from the
// sender: fixed input delay can schedule a sample before that future SI poll has
// occurred. Each receiver records the authoritative application position when
// the emulated SI device actually consumes the slot.
constexpr u32 ROLLBACK_SI_INPUT_MAGIC = 0x52534942;  // "RSIB"
constexpr u16 ROLLBACK_SI_INPUT_VERSION = 1;
constexpr std::size_t ROLLBACK_SI_MAX_BATCHES_PER_PACKET = 8;
constexpr std::size_t ROLLBACK_SI_MAX_PACKET_SIZE = 481;

struct RollbackSIInputBatch
{
  u64 batch_id = 0;
  u8 pad_mask = 0;
  std::array<GCPadStatus, 4> pads{};
};

struct RollbackSIInputPacket
{
  u16 protocol_version = ROLLBACK_SI_INPUT_VERSION;
  u64 session_generation = 0;
  bool has_contiguous_ack = false;
  u64 contiguous_ack = 0;
  std::array<RollbackSIInputBatch, ROLLBACK_SI_MAX_BATCHES_PER_PACKET> batches{};
  std::size_t batch_count = 0;
};

enum class RollbackSIInputCodecStatus : u8
{
  Ok,
  OutputTooSmall,
  Truncated,
  TrailingData,
  BadMagic,
  UnsupportedVersion,
  WrongGeneration,
  InvalidGeneration,
  InvalidFlags,
  InvalidBatchCount,
  InvalidPadMask,
  NonIncreasingBatchIds,
  InvalidConnectedFlag,
};

struct RollbackSIInputEncodeResult
{
  RollbackSIInputCodecStatus status = RollbackSIInputCodecStatus::Ok;
  std::size_t size = 0;

  explicit operator bool() const { return status == RollbackSIInputCodecStatus::Ok; }
};

struct RollbackSIInputDecodeResult
{
  RollbackSIInputCodecStatus status = RollbackSIInputCodecStatus::Ok;
  RollbackSIInputPacket packet{};

  explicit operator bool() const { return status == RollbackSIInputCodecStatus::Ok; }
};

RollbackSIInputEncodeResult EncodeRollbackSIInputPacket(const RollbackSIInputPacket& packet,
                                                        std::span<u8> output);
RollbackSIInputDecodeResult DecodeRollbackSIInputPacket(std::span<const u8> input,
                                                        u64 expected_generation);

}  // namespace NetPlay
