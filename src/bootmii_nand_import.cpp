// bootmii_nand_import.cpp
//
// Standalone extraction of the Dolphin Emulator "Import BootMii NAND Backup"
// feature (DiscIO::NANDImporter), without any Qt/GUI or Dolphin-project
// dependencies. Behavior mirrors:
//   Source/Core/DiscIO/NANDImporter.{h,cpp}   (core logic)
//   Source/Core/DolphinQt/MainWindow.cpp      (OnImportNANDBackup, for reference)
//
// Original code: Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// This file is a from-scratch re-implementation of the same algorithm using
// only the C++ standard library plus a small embedded AES-128-CBC decryptor
// (needed because Dolphin's version relies on mbedtls / Dolphin's own
// Common::AES wrapper, which we don't want to drag in for a single-purpose
// tool).
//
// Build:
//   g++ -std=c++20 -O2 -o bootmii_nand_import bootmii_nand_import.cpp
//
// Usage:
//   bootmii_nand_import <nand_backup.bin> <output_nand_root> [otp_keys.bin]
//
//   <nand_backup.bin>    BootMii NAND backup (raw dump straight off the SD
//                        card, with or without the appended 0x400-byte
//                        keys.bin/OTP+SEEPROM dump).
//   <output_nand_root>   Directory to extract the Wii NAND filesystem into
//                        (created if it doesn't exist). This is Dolphin's
//                        "Wii NAND root" (normally Wii/ inside the user
//                        folder).
//   [otp_keys.bin]       Only required if the backup does NOT already
//                        contain the appended keys dump. This is the
//                        "otp.bin"/"keys.bin" file (0x400 bytes) some BootMii
//                        dumps ship separately.

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// ---------------------------------------------------------------------------
// Big-endian helpers (the Wii NAND filesystem is big-endian throughout)
// ---------------------------------------------------------------------------
static u16 Swap16(u16 v) { return static_cast<u16>((v >> 8) | (v << 8)); }
static u32 Swap32(u32 v)
{
  return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x000000FFu) << 24);
}
static u64 Swap64(u64 v)
{
  return (static_cast<u64>(Swap32(static_cast<u32>(v & 0xFFFFFFFFull))) << 32) |
         Swap32(static_cast<u32>(v >> 32));
}
static u16 ReadBE16(const u8* p) { u16 v; std::memcpy(&v, p, 2); return Swap16(v); }
static u32 ReadBE32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return Swap32(v); }
static u64 ReadBE64(const u8* p) { u64 v; std::memcpy(&v, p, 8); return Swap64(v); }

// A tiny wrapper that behaves like Dolphin's Common::BigEndianValue<T> for
// the fixed-width unsigned integer types we need, so the on-disk struct
// layouts below read exactly like Dolphin's originals.
template <typename T>
class BigEndianValue
{
public:
  BigEndianValue() = default;
  operator T() const
  {
    T value{};
    std::memcpy(&value, raw.data(), sizeof(T));
    if constexpr (sizeof(T) == 2)
      value = Swap16(value);
    else if constexpr (sizeof(T) == 4)
      value = Swap32(value);
    else if constexpr (sizeof(T) == 8)
      value = Swap64(value);
    return value;
  }

private:
  std::array<u8, sizeof(T)> raw{};
};

// ---------------------------------------------------------------------------
// Minimal, self-contained AES-128 (ECB core used to build CBC decryption with
// a zero IV, which is all NANDImporter needs: m_aes_ctx->CryptIvZero(...)).
// Public-domain-style textbook implementation (Rijndael, Nk=4, Nr=10).
// ---------------------------------------------------------------------------
namespace AES128
{
static const u8 sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static u8 inv_sbox[256];
static u8 rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

static void InitInvSBox()
{
  static bool done = false;
  if (done)
    return;
  for (int i = 0; i < 256; i++)
    inv_sbox[sbox[i]] = static_cast<u8>(i);
  done = true;
}

static u8 XTime(u8 x) { return static_cast<u8>((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00)); }
static u8 Mul(u8 a, u8 b)
{
  u8 p = 0;
  for (int i = 0; i < 8; i++)
  {
    if (b & 1)
      p ^= a;
    bool hi = a & 0x80;
    a = static_cast<u8>(a << 1);
    if (hi)
      a ^= 0x1B;
    b >>= 1;
  }
  return p;
}

class Context
{
public:
  explicit Context(const u8 key[16])
  {
    InitInvSBox();
    KeyExpansion(key);
  }

  // Decrypts 'len' bytes (must be a multiple of 16) from buf_in to buf_out,
  // CBC mode, using an all-zero IV (this is the only mode NANDImporter uses,
  // via CryptIvZero).
  void DecryptCbcZeroIv(const u8* buf_in, u8* buf_out, size_t len) const
  {
    u8 prev[16] = {0};
    u8 block[16];
    for (size_t off = 0; off < len; off += 16)
    {
      std::memcpy(block, buf_in + off, 16);
      u8 decrypted[16];
      DecryptBlock(block, decrypted);
      for (int i = 0; i < 16; i++)
        buf_out[off + i] = decrypted[i] ^ prev[i];
      std::memcpy(prev, block, 16);
    }
  }

private:
  static constexpr int Nk = 4, Nb = 4, Nr = 10;
  u8 round_keys[(Nr + 1) * 16];

  void KeyExpansion(const u8 key[16])
  {
    std::memcpy(round_keys, key, 16);
    u8 temp[4];
    for (int i = Nk; i < Nb * (Nr + 1); i++)
    {
      std::memcpy(temp, round_keys + (i - 1) * 4, 4);
      if (i % Nk == 0)
      {
        u8 t = temp[0];
        temp[0] = static_cast<u8>(sbox[temp[1]] ^ rcon[i / Nk]);
        temp[1] = sbox[temp[2]];
        temp[2] = sbox[temp[3]];
        temp[3] = sbox[t];
      }
      for (int j = 0; j < 4; j++)
        round_keys[i * 4 + j] = static_cast<u8>(round_keys[(i - Nk) * 4 + j] ^ temp[j]);
    }
  }

  void AddRoundKey(u8 state[16], int round) const
  {
    for (int i = 0; i < 16; i++)
      state[i] ^= round_keys[round * 16 + i];
  }

  static void InvSubBytes(u8 state[16])
  {
    for (int i = 0; i < 16; i++)
      state[i] = inv_sbox[state[i]];
  }

  static void InvShiftRows(u8 state[16])
  {
    // State is column-major: state[col*4 + row]
    u8 tmp[16];
    std::memcpy(tmp, state, 16);
    for (int row = 0; row < 4; row++)
    {
      for (int col = 0; col < 4; col++)
        state[col * 4 + row] = tmp[((col - row + 4) % 4) * 4 + row];
    }
  }

  static void InvMixColumns(u8 state[16])
  {
    for (int c = 0; c < 4; c++)
    {
      u8 a0 = state[c * 4 + 0], a1 = state[c * 4 + 1], a2 = state[c * 4 + 2], a3 = state[c * 4 + 3];
      state[c * 4 + 0] = static_cast<u8>(Mul(a0, 0x0e) ^ Mul(a1, 0x0b) ^ Mul(a2, 0x0d) ^ Mul(a3, 0x09));
      state[c * 4 + 1] = static_cast<u8>(Mul(a0, 0x09) ^ Mul(a1, 0x0e) ^ Mul(a2, 0x0b) ^ Mul(a3, 0x0d));
      state[c * 4 + 2] = static_cast<u8>(Mul(a0, 0x0d) ^ Mul(a1, 0x09) ^ Mul(a2, 0x0e) ^ Mul(a3, 0x0b));
      state[c * 4 + 3] = static_cast<u8>(Mul(a0, 0x0b) ^ Mul(a1, 0x0d) ^ Mul(a2, 0x09) ^ Mul(a3, 0x0e));
    }
  }

  void DecryptBlock(const u8 in[16], u8 out[16]) const
  {
    u8 state[16];
    std::memcpy(state, in, 16);

    AddRoundKey(state, Nr);
    for (int round = Nr - 1; round >= 1; round--)
    {
      InvShiftRows(state);
      InvSubBytes(state);
      AddRoundKey(state, round);
      InvMixColumns(state);
    }
    InvShiftRows(state);
    InvSubBytes(state);
    AddRoundKey(state, 0);

    std::memcpy(out, state, 16);
  }
};
}  // namespace AES128

// ---------------------------------------------------------------------------
// File name escaping, equivalent to Dolphin's Common::EscapeFileName
// (Source/Core/Common/NandPaths.cpp)
// ---------------------------------------------------------------------------
static bool IsIllegalCharacter(char c)
{
  static constexpr char illegal_chars[] = {'\"', '*', '/', ':', '<', '>', '?', '\\', '|', '\x7f'};
  return static_cast<unsigned char>(c) <= 0x1F ||
         std::find(std::begin(illegal_chars), std::end(illegal_chars), c) !=
             std::end(illegal_chars);
}

static std::string ReplaceAll(std::string str, std::string_view from, std::string_view to)
{
  if (from.empty())
    return str;
  size_t pos = 0;
  while ((pos = str.find(from, pos)) != std::string::npos)
  {
    str.replace(pos, from.size(), to);
    pos += to.size();
  }
  return str;
}

static std::string EscapeFileName(const std::string& filename)
{
  if (std::all_of(filename.begin(), filename.end(), [](char c) { return c == '.'; }))
    return ReplaceAll(filename, ".", "__2e__");

  std::string escaped_double_underscores = ReplaceAll(filename, "__", "__5f____5f__");

  std::string result;
  result.reserve(escaped_double_underscores.size());
  char buf[8];
  for (char c : escaped_double_underscores)
  {
    if (IsIllegalCharacter(c))
    {
      std::snprintf(buf, sizeof(buf), "__%02x__", static_cast<unsigned char>(c));
      result += buf;
    }
    else
    {
      result.push_back(c);
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// NAND filesystem on-disk structures (identical layout to Dolphin's
// DiscIO::NANDImporter::NANDFSTEntry / NANDSuperblock)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct NANDFSTEntry
{
  char name[12];
  u8 mode;
  u8 attr;
  BigEndianValue<u16> sub;
  BigEndianValue<u16> sib;
  BigEndianValue<u32> size;
  BigEndianValue<u32> uid;
  BigEndianValue<u16> gid;
  BigEndianValue<u32> x3;
};
static_assert(sizeof(NANDFSTEntry) == 0x20, "Wrong size");

constexpr u16 FSTEntryCount = 0x17FF;
struct NANDSuperblock
{
  std::array<char, 4> magic;  // "SFFS"
  BigEndianValue<u32> version;
  BigEndianValue<u32> unknown;
  std::array<BigEndianValue<u16>, 0x8000> fat;
  std::array<NANDFSTEntry, FSTEntryCount> fst;
  std::array<u8, 0x14> pad;
};
static_assert(sizeof(NANDSuperblock) == 0x40000, "Wrong size");
#pragma pack(pop)

enum class EntryType
{
  File = 1,
  Directory = 2,
};

// ---------------------------------------------------------------------------
// NANDImporter: behavior-equivalent port of DiscIO::NANDImporter
// ---------------------------------------------------------------------------
class NANDImporter
{
public:
  explicit NANDImporter(std::string nand_root) : m_nand_root(std::move(nand_root)) {}

  bool ImportNANDBin(const std::string& path_to_bin, const std::string& otp_dump_path)
  {
    if (!ReadNANDBin(path_to_bin, otp_dump_path))
      return false;
    if (!FindSuperblock())
      return false;

    ExportKeys();

    if (!ExtractFiles())
      return false;

    if (!ExtractCertificates())
      std::cerr << "Warning: could not extract certificates (non-fatal; NAND was imported).\n";

    return true;
  }

private:
  static constexpr size_t NAND_SIZE = 0x20000000;
  static constexpr size_t NAND_KEYS_SIZE = 0x400;

  bool ReadNANDBin(const std::string& path_to_bin, const std::string& otp_dump_path)
  {
    constexpr size_t NAND_TOTAL_BLOCKS = 0x40000;
    constexpr size_t NAND_BLOCK_SIZE = 0x800;
    constexpr size_t NAND_ECC_BLOCK_SIZE = 0x40;
    constexpr size_t NAND_BIN_SIZE = (NAND_BLOCK_SIZE + NAND_ECC_BLOCK_SIZE) * NAND_TOTAL_BLOCKS;

    std::ifstream file(path_to_bin, std::ios::binary);
    if (!file)
    {
      std::cerr << "Error: could not open '" << path_to_bin << "'.\n";
      return false;
    }

    file.seekg(0, std::ios::end);
    const u64 image_size = static_cast<u64>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (image_size != NAND_BIN_SIZE + NAND_KEYS_SIZE && image_size != NAND_BIN_SIZE)
    {
      std::cerr << "Error: this file does not look like a BootMii NAND backup "
                   "(unexpected size "
                << image_size << " bytes).\n";
      return false;
    }

    m_nand.resize(NAND_SIZE);

    std::cout << "Loading NAND image...\n";
    for (size_t i = 0; i < NAND_TOTAL_BLOCKS; i++)
    {
      file.read(reinterpret_cast<char*>(&m_nand[i * NAND_BLOCK_SIZE]), NAND_BLOCK_SIZE);
      if (!file)
      {
        std::cerr << "Error: unexpected end of file while reading NAND blocks.\n";
        return false;
      }
      file.seekg(NAND_ECC_BLOCK_SIZE, std::ios::cur);  // Skip ECC, unused.

      if (i % (NAND_TOTAL_BLOCKS / 20 + 1) == 0)
        PrintProgress(i, NAND_TOTAL_BLOCKS);
    }
    PrintProgress(NAND_TOTAL_BLOCKS, NAND_TOTAL_BLOCKS);
    std::cout << '\n';

    m_nand_keys.resize(NAND_KEYS_SIZE);

    if (image_size == NAND_BIN_SIZE)
    {
      // Keys not included in the backup: read them from a separate dump.
      if (otp_dump_path.empty())
      {
        std::cerr << "Error: this NAND backup does not include the OTP/SEEPROM "
                     "dump; pass the keys file as the 3rd argument.\n";
        return false;
      }
      std::ifstream keys_file(otp_dump_path, std::ios::binary);
      if (!keys_file)
      {
        std::cerr << "Error: could not open keys file '" << otp_dump_path << "'.\n";
        return false;
      }
      keys_file.read(reinterpret_cast<char*>(m_nand_keys.data()), NAND_KEYS_SIZE);
      return static_cast<bool>(keys_file);
    }

    // Keys are appended to the image itself.
    file.read(reinterpret_cast<char*>(m_nand_keys.data()), NAND_KEYS_SIZE);
    return static_cast<bool>(file);
  }

  bool FindSuperblock()
  {
    constexpr size_t NAND_SUPERBLOCK_START = 0x1fc00000;

    for (int i = 0; i < 16; i++)
    {
      auto superblock = std::make_unique<NANDSuperblock>();
      std::memcpy(superblock.get(), &m_nand[NAND_SUPERBLOCK_START + i * sizeof(NANDSuperblock)],
                  sizeof(NANDSuperblock));

      if (std::memcmp(superblock->magic.data(), "SFFS", 4) != 0)
        continue;

      if (!m_superblock || static_cast<u32>(superblock->version) > static_cast<u32>(m_superblock->version))
        m_superblock = std::move(superblock);
    }

    if (!m_superblock)
    {
      std::cerr << "Error: this file does not contain a valid Wii filesystem.\n";
      return false;
    }

    std::cout << "Using superblock version 0x" << std::hex << static_cast<u32>(m_superblock->version)
               << std::dec << '\n';
    return true;
  }

  bool ExtractFiles()
  {
    constexpr u16 CLUSTER_CHAIN_END = 0xFFFB;
    m_progress_cur = 0;
    m_progress_max = 0;
    for (const auto& entry : m_superblock->fat)
    {
      if (static_cast<u16>(entry) == CLUSTER_CHAIN_END)
        m_progress_max++;
    }

    std::bitset<FSTEntryCount> visited;
    std::cout << "Extracting NAND filesystem to '" << m_nand_root << "'...\n";
    bool ok = ProcessEntry(0, "", &visited);
    std::cout << '\n';
    return ok;
  }

  static std::string GetPath(const NANDFSTEntry& entry, const std::string& parent_path)
  {
    std::string name(entry.name, strnlen(entry.name, sizeof(NANDFSTEntry::name)));
    return parent_path + '/' + EscapeFileName(name);
  }

  bool ProcessEntry(u16 entry_number, const std::string& parent_path, std::bitset<FSTEntryCount>* visited)
  {
    while (entry_number != 0xffff)
    {
      if (entry_number >= m_superblock->fst.size())
      {
        std::cerr << "Error: FST entry number " << entry_number << " out of range.\n";
        return false;
      }

      if ((*visited)[entry_number])
      {
        std::cerr << "Error: FST entry number " << entry_number << " visited multiple times.\n";
        return false;
      }
      (*visited)[entry_number] = true;

      const NANDFSTEntry entry = m_superblock->fst[entry_number];
      const std::string path = entry_number == 0 ? parent_path : GetPath(entry, parent_path);

      const EntryType type = static_cast<EntryType>(entry.mode & 3);
      if (type == EntryType::File)
      {
        m_progress_cur++;
        if (m_progress_max)
          PrintProgress(m_progress_cur, m_progress_max);

        std::vector<u8> data = GetEntryData(entry);
        const fs::path out_path = m_nand_root + path;
        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
        std::ofstream out(out_path, std::ios::binary);
        if (!out)
        {
          std::cerr << "Error: could not write file '" << out_path.string() << "'.\n";
          return false;
        }
        if (!data.empty())
          out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
      }
      else if (type == EntryType::Directory)
      {
        std::error_code ec;
        fs::create_directories(m_nand_root + path, ec);
        if (!ProcessEntry(entry.sub, path, visited))
          return false;
      }
      else
      {
        std::cerr << "Warning: ignoring unknown entry type for '" << path << "'.\n";
      }

      entry_number = entry.sib;
    }
    return true;
  }

  std::vector<u8> GetEntryData(const NANDFSTEntry& entry) const
  {
    constexpr size_t NAND_FAT_BLOCK_SIZE = 0x4000;

    u16 sub = entry.sub;
    size_t remaining_bytes = entry.size;
    std::vector<u8> data;
    data.reserve(remaining_bytes);

    std::vector<u8> block(NAND_FAT_BLOCK_SIZE);
    while (remaining_bytes > 0)
    {
      if (sub >= m_superblock->fat.size())
      {
        std::cerr << "Error: FAT block index " << sub << " out of range.\n";
        return {};
      }

      m_aes_ctx->DecryptCbcZeroIv(&m_nand[NAND_FAT_BLOCK_SIZE * sub], block.data(), NAND_FAT_BLOCK_SIZE);

      size_t size = std::min(remaining_bytes, NAND_FAT_BLOCK_SIZE);
      data.insert(data.end(), block.begin(), block.begin() + static_cast<long>(size));
      remaining_bytes -= size;

      sub = m_superblock->fat[sub];
    }

    return data;
  }

  void ExportKeys()
  {
    constexpr size_t NAND_AES_KEY_OFFSET = 0x158;
    m_aes_ctx = std::make_unique<AES128::Context>(&m_nand_keys[NAND_AES_KEY_OFFSET]);

    const std::string file_path = m_nand_root + "/keys.bin";
    std::error_code ec;
    fs::create_directories(m_nand_root, ec);
    std::ofstream file(file_path, std::ios::binary);
    if (!file || !file.write(reinterpret_cast<const char*>(m_nand_keys.data()),
                              static_cast<std::streamsize>(NAND_KEYS_SIZE)))
    {
      std::cerr << "Warning: unable to write to file " << file_path << '\n';
    }
  }

  // Minimal TMD parsing (Wii ES title metadata format) just to locate the
  // boot content of title 00000001-0000000d (IOS13, aka the certs holder),
  // matching DiscIO::NANDImporter::ExtractCertificates. On-disk layout taken
  // from Source/Core/Core/IOS/ES/Formats.h (TMDHeader / Content, both under
  // #pragma pack(push, 4)):
  //   sizeof(SignatureRSA2048) == 0x180
  //   offsetof(TMDHeader, num_contents) == 0x1de
  //   offsetof(TMDHeader, boot_index)   == 0x1e0
  //   sizeof(TMDHeader)                 == 0x1e4
  //   sizeof(Content)                   == 36 (id:4, index:2, type:2, size:8, sha1:20)
  bool ExtractCertificates()
  {
    static constexpr size_t TMD_HEADER_SIZE = 0x1e4;
    static constexpr size_t TMD_NUM_CONTENTS_OFFSET = 0x1de;
    static constexpr size_t TMD_BOOT_INDEX_OFFSET = 0x1e0;
    static constexpr size_t CONTENT_ENTRY_SIZE = 36;

    const std::string content_dir = m_nand_root + "/title/00000001/0000000d/content/";

    std::vector<u8> tmd_bytes = ReadWholeFile(content_dir + "title.tmd");
    if (tmd_bytes.size() < TMD_HEADER_SIZE)
    {
      std::cerr << "Note: could not read IOS13 TMD; skipping certificate extraction.\n";
      return false;
    }

    const u16 num_contents = ReadBE16(&tmd_bytes[TMD_NUM_CONTENTS_OFFSET]);
    const u16 boot_index = ReadBE16(&tmd_bytes[TMD_BOOT_INDEX_OFFSET]);
    if (boot_index >= num_contents)
    {
      std::cerr << "Note: could not get content ID from TMD; skipping certificate extraction.\n";
      return false;
    }

    const size_t content_offset = TMD_HEADER_SIZE + static_cast<size_t>(boot_index) * CONTENT_ENTRY_SIZE;
    if (tmd_bytes.size() < content_offset + CONTENT_ENTRY_SIZE)
    {
      std::cerr << "Note: TMD too small for content table; skipping certificate extraction.\n";
      return false;
    }
    const u32 content_id = ReadBE32(&tmd_bytes[content_offset]);

    char content_name[16];
    std::snprintf(content_name, sizeof(content_name), "%08x.app", content_id);
    std::vector<u8> content_bytes = ReadWholeFile(content_dir + content_name);
    if (content_bytes.empty())
    {
      std::cerr << "Note: could not read IOS13 contents; skipping certificate extraction.\n";
      return false;
    }

    struct PEMCertificate
    {
      const char* filename;
      std::array<u8, 4> search_bytes;
    };
    static constexpr std::array<PEMCertificate, 3> certificates{{
        {"/clientca.pem", {0x30, 0x82, 0x03, 0xE9}},
        {"/clientcakey.pem", {0x30, 0x82, 0x02, 0x5D}},
        {"/rootca.pem", {0x30, 0x82, 0x03, 0x7D}},
    }};

    bool all_ok = true;
    for (const auto& certificate : certificates)
    {
      auto it = std::search(content_bytes.begin(), content_bytes.end(),
                             certificate.search_bytes.begin(), certificate.search_bytes.end());
      if (it == content_bytes.end())
      {
        std::cerr << "Note: could not find offset for certificate '" << certificate.filename << "'.\n";
        all_ok = false;
        continue;
      }

      const ptrdiff_t certificate_offset = std::distance(content_bytes.begin(), it);
      constexpr int min_offset = 2;
      if (certificate_offset < min_offset)
      {
        all_ok = false;
        continue;
      }
      const u16 certificate_size = ReadBE16(&content_bytes[certificate_offset - min_offset]);
      const size_t available_size = content_bytes.size() - static_cast<size_t>(certificate_offset);
      if (certificate_size > available_size)
      {
        all_ok = false;
        continue;
      }

      const std::string pem_file_path = m_nand_root + certificate.filename;
      std::ofstream pem_file(pem_file_path, std::ios::binary);
      if (!pem_file || !pem_file.write(reinterpret_cast<const char*>(&content_bytes[certificate_offset]),
                                        certificate_size))
      {
        std::cerr << "Note: unable to write to file " << pem_file_path << '\n';
        all_ok = false;
      }
    }
    return all_ok;
  }

  static std::vector<u8> ReadWholeFile(const std::string& path)
  {
    std::ifstream f(path, std::ios::binary);
    if (!f)
      return {};
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size <= 0)
      return {};
    std::vector<u8> data(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (!f)
      return {};
    return data;
  }

  static void PrintProgress(size_t cur, size_t max)
  {
    if (max == 0)
      return;
    const int percent = static_cast<int>((cur * 100) / max);
    std::cout << "\rProgress: " << percent << "%   " << std::flush;
  }

  std::string m_nand_root;
  std::vector<u8> m_nand;
  std::vector<u8> m_nand_keys;
  std::unique_ptr<AES128::Context> m_aes_ctx;
  std::unique_ptr<NANDSuperblock> m_superblock;
  u32 m_progress_cur = 0;
  u32 m_progress_max = 0;
};

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
  if (argc < 3 || argc > 4)
  {
    std::cerr << "Dolphin BootMii NAND Backup Importer (standalone)\n\n"
              << "Usage: " << argv[0] << " <nand_backup.bin> <output_nand_root> [otp_keys.bin]\n\n"
              << "  <nand_backup.bin>    Raw BootMii NAND dump (with or without appended keys).\n"
              << "  <output_nand_root>   Directory to extract the Wii NAND filesystem into.\n"
              << "  [otp_keys.bin]       OTP/SEEPROM dump, only needed if not already appended\n"
              << "                       to the backup file.\n";
    return 1;
  }

  const std::string bin_path = argv[1];
  const std::string nand_root = argv[2];
  const std::string otp_path = argc == 4 ? argv[3] : "";

  NANDImporter importer(nand_root);
  if (!importer.ImportNANDBin(bin_path, otp_path))
  {
    std::cerr << "Import failed.\n";
    return 1;
  }

  std::cout << "NAND backup imported successfully into '" << nand_root << "'.\n";
  return 0;
}
