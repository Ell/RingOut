// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlay/RollbackSIInputProtocol.h"

#include <limits>

namespace NetPlay
{
namespace
{
constexpr u16 FLAG_HAS_CONTIGUOUS_ACK = 1;
constexpr std::size_t HEADER_SIZE = 25;
constexpr std::size_t BATCH_HEADER_SIZE = 9;
constexpr std::size_t PAD_SIZE = 12;

class Writer
{
public:
  explicit Writer(std::span<u8> output) : m_output(output) {}

  bool U8(const u8 value)
  {
    if (m_position >= m_output.size())
      return false;
    m_output[m_position++] = value;
    return true;
  }

  bool U16(const u16 value)
  {
    return U8(static_cast<u8>(value >> 8)) && U8(static_cast<u8>(value));
  }

  bool U32(const u32 value)
  {
    return U16(static_cast<u16>(value >> 16)) && U16(static_cast<u16>(value));
  }

  bool U64(const u64 value)
  {
    return U32(static_cast<u32>(value >> 32)) && U32(static_cast<u32>(value));
  }

  std::size_t Size() const { return m_position; }

private:
  std::span<u8> m_output;
  std::size_t m_position = 0;
};

class Reader
{
public:
  explicit Reader(std::span<const u8> input) : m_input(input) {}

  bool U8(u8* const value)
  {
    if (m_position >= m_input.size())
      return false;
    *value = m_input[m_position++];
    return true;
  }

  bool U16(u16* const value)
  {
    u8 high = 0;
    u8 low = 0;
    if (!U8(&high) || !U8(&low))
      return false;
    *value = static_cast<u16>((static_cast<u16>(high) << 8) | low);
    return true;
  }

  bool U32(u32* const value)
  {
    u16 high = 0;
    u16 low = 0;
    if (!U16(&high) || !U16(&low))
      return false;
    *value = (static_cast<u32>(high) << 16) | low;
    return true;
  }

  bool U64(u64* const value)
  {
    u32 high = 0;
    u32 low = 0;
    if (!U32(&high) || !U32(&low))
      return false;
    *value = (static_cast<u64>(high) << 32) | low;
    return true;
  }

  bool AtEnd() const { return m_position == m_input.size(); }

private:
  std::span<const u8> m_input;
  std::size_t m_position = 0;
};

bool WritePad(Writer* const writer, const GCPadStatus& pad)
{
  return writer->U16(pad.button) && writer->U8(pad.stickX) && writer->U8(pad.stickY) &&
         writer->U8(pad.substickX) && writer->U8(pad.substickY) && writer->U8(pad.triggerLeft) &&
         writer->U8(pad.triggerRight) && writer->U8(pad.analogA) && writer->U8(pad.analogB) &&
         writer->U8(pad.switches) && writer->U8(pad.isConnected ? 1 : 0);
}

RollbackSIInputCodecStatus ReadPad(Reader* const reader, GCPadStatus* const pad)
{
  u8 connected = 0;
  if (!reader->U16(&pad->button) || !reader->U8(&pad->stickX) || !reader->U8(&pad->stickY) ||
      !reader->U8(&pad->substickX) || !reader->U8(&pad->substickY) ||
      !reader->U8(&pad->triggerLeft) || !reader->U8(&pad->triggerRight) ||
      !reader->U8(&pad->analogA) || !reader->U8(&pad->analogB) || !reader->U8(&pad->switches) ||
      !reader->U8(&connected))
  {
    return RollbackSIInputCodecStatus::Truncated;
  }
  if (connected > 1)
    return RollbackSIInputCodecStatus::InvalidConnectedFlag;
  pad->isConnected = connected != 0;
  return RollbackSIInputCodecStatus::Ok;
}

RollbackSIInputCodecStatus ValidatePacket(const RollbackSIInputPacket& packet)
{
  if (packet.protocol_version != ROLLBACK_SI_INPUT_VERSION)
    return RollbackSIInputCodecStatus::UnsupportedVersion;
  if (packet.session_generation == 0)
    return RollbackSIInputCodecStatus::InvalidGeneration;
  if (packet.batch_count == 0 || packet.batch_count > ROLLBACK_SI_MAX_BATCHES_PER_PACKET)
    return RollbackSIInputCodecStatus::InvalidBatchCount;

  for (std::size_t i = 0; i < packet.batch_count; ++i)
  {
    const auto& batch = packet.batches[i];
    if (batch.pad_mask == 0 || (batch.pad_mask & 0xf0) != 0)
      return RollbackSIInputCodecStatus::InvalidPadMask;
    if (batch.batch_id == std::numeric_limits<u64>::max())
      return RollbackSIInputCodecStatus::NonIncreasingBatchIds;
    if (i != 0 && batch.batch_id <= packet.batches[i - 1].batch_id)
      return RollbackSIInputCodecStatus::NonIncreasingBatchIds;
  }
  return RollbackSIInputCodecStatus::Ok;
}

std::size_t EncodedSize(const RollbackSIInputPacket& packet)
{
  std::size_t size = HEADER_SIZE;
  for (std::size_t i = 0; i < packet.batch_count; ++i)
  {
    size += BATCH_HEADER_SIZE;
    for (std::size_t pad = 0; pad < 4; ++pad)
    {
      if ((packet.batches[i].pad_mask & (u8{1} << pad)) != 0)
        size += PAD_SIZE;
    }
  }
  return size;
}
}  // namespace

RollbackSIInputEncodeResult EncodeRollbackSIInputPacket(const RollbackSIInputPacket& packet,
                                                        const std::span<u8> output)
{
  const RollbackSIInputCodecStatus validation = ValidatePacket(packet);
  if (validation != RollbackSIInputCodecStatus::Ok)
    return {.status = validation};

  const std::size_t required = EncodedSize(packet);
  if (required > output.size() || required > ROLLBACK_SI_MAX_PACKET_SIZE)
    return {.status = RollbackSIInputCodecStatus::OutputTooSmall};

  Writer writer(output.first(required));
  const u16 flags = packet.has_contiguous_ack ? FLAG_HAS_CONTIGUOUS_ACK : 0;
  if (!writer.U32(ROLLBACK_SI_INPUT_MAGIC) || !writer.U16(packet.protocol_version) ||
      !writer.U16(flags) || !writer.U64(packet.session_generation) ||
      !writer.U64(packet.has_contiguous_ack ? packet.contiguous_ack : 0) ||
      !writer.U8(static_cast<u8>(packet.batch_count)))
  {
    return {.status = RollbackSIInputCodecStatus::OutputTooSmall};
  }

  for (std::size_t i = 0; i < packet.batch_count; ++i)
  {
    const auto& batch = packet.batches[i];
    if (!writer.U64(batch.batch_id) || !writer.U8(batch.pad_mask))
      return {.status = RollbackSIInputCodecStatus::OutputTooSmall};
    for (std::size_t pad = 0; pad < 4; ++pad)
    {
      if ((batch.pad_mask & (u8{1} << pad)) != 0 && !WritePad(&writer, batch.pads[pad]))
        return {.status = RollbackSIInputCodecStatus::OutputTooSmall};
    }
  }
  return {.status = RollbackSIInputCodecStatus::Ok, .size = writer.Size()};
}

RollbackSIInputDecodeResult DecodeRollbackSIInputPacket(const std::span<const u8> input,
                                                        const u64 expected_generation)
{
  RollbackSIInputDecodeResult result;
  Reader reader(input);
  u32 magic = 0;
  u16 flags = 0;
  u8 batch_count = 0;
  if (!reader.U32(&magic) || !reader.U16(&result.packet.protocol_version) || !reader.U16(&flags) ||
      !reader.U64(&result.packet.session_generation) ||
      !reader.U64(&result.packet.contiguous_ack) || !reader.U8(&batch_count))
  {
    result.status = RollbackSIInputCodecStatus::Truncated;
    return result;
  }
  if (magic != ROLLBACK_SI_INPUT_MAGIC)
  {
    result.status = RollbackSIInputCodecStatus::BadMagic;
    return result;
  }
  if (result.packet.protocol_version != ROLLBACK_SI_INPUT_VERSION)
  {
    result.status = RollbackSIInputCodecStatus::UnsupportedVersion;
    return result;
  }
  if (result.packet.session_generation == 0)
  {
    result.status = RollbackSIInputCodecStatus::InvalidGeneration;
    return result;
  }
  if (expected_generation != 0 && result.packet.session_generation != expected_generation)
  {
    result.status = RollbackSIInputCodecStatus::WrongGeneration;
    return result;
  }
  if ((flags & ~FLAG_HAS_CONTIGUOUS_ACK) != 0)
  {
    result.status = RollbackSIInputCodecStatus::InvalidFlags;
    return result;
  }
  result.packet.has_contiguous_ack = (flags & FLAG_HAS_CONTIGUOUS_ACK) != 0;
  if (!result.packet.has_contiguous_ack && result.packet.contiguous_ack != 0)
  {
    result.status = RollbackSIInputCodecStatus::InvalidFlags;
    return result;
  }
  if (batch_count == 0 || batch_count > ROLLBACK_SI_MAX_BATCHES_PER_PACKET)
  {
    result.status = RollbackSIInputCodecStatus::InvalidBatchCount;
    return result;
  }
  result.packet.batch_count = batch_count;

  for (std::size_t i = 0; i < result.packet.batch_count; ++i)
  {
    auto& batch = result.packet.batches[i];
    if (!reader.U64(&batch.batch_id) || !reader.U8(&batch.pad_mask))
    {
      result.status = RollbackSIInputCodecStatus::Truncated;
      return result;
    }
    if (batch.pad_mask == 0 || (batch.pad_mask & 0xf0) != 0)
    {
      result.status = RollbackSIInputCodecStatus::InvalidPadMask;
      return result;
    }
    if (batch.batch_id == std::numeric_limits<u64>::max() ||
        (i != 0 && batch.batch_id <= result.packet.batches[i - 1].batch_id))
    {
      result.status = RollbackSIInputCodecStatus::NonIncreasingBatchIds;
      return result;
    }
    for (std::size_t pad = 0; pad < 4; ++pad)
    {
      if ((batch.pad_mask & (u8{1} << pad)) == 0)
        continue;
      result.status = ReadPad(&reader, &batch.pads[pad]);
      if (result.status != RollbackSIInputCodecStatus::Ok)
        return result;
    }
  }
  if (!reader.AtEnd())
  {
    result.status = RollbackSIInputCodecStatus::TrailingData;
    return result;
  }
  result.status = RollbackSIInputCodecStatus::Ok;
  return result;
}

}  // namespace NetPlay
