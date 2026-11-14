// First-run setup for the public test build: the player supplies their own copy of the game
// (Army of Two: The Devil's Cartel, Xbox 360, Europe En/De/It ISO). Nothing of the game ships
// with the port. This module verifies the ISO, extracts the disc files the game needs and the
// HD texture package (an STFS container stored on the disc) into the local game folder.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ao4setup {

// Progress callback: bytes done / total, current file (relative path). Return false to cancel.
using Progress = std::function<bool(uint64_t done, uint64_t total, const std::wstring& file)>;

struct IsoFile {
  std::string path;     // relative, '/' separated
  uint64_t offset = 0;  // byte offset inside the ISO
  uint64_t size = 0;
};

struct IsoInfo {
  uint64_t partition_offset = 0;  // XDVDFS game partition (XGD3: 0x2080000)
  std::vector<IsoFile> files;
};

// Reads the XDVDFS directory of an Xbox 360 ISO. Returns false with an error text.
bool ReadIso(const std::wstring& iso, IsoInfo& info, std::wstring& error);

// The one supported game version: SHA-256 of Default.xex and AO4.Main.xex.
struct VersionCheck {
  bool ok = false;
  std::string default_xex_sha256, main_xex_sha256;
  std::wstring message;
};
VersionCheck CheckVersion(const std::wstring& iso, const IsoInfo& info);

// Extracts the game files (root .xex, Data/, Movies/, NxeArt; not $SystemUpdate or Content/)
// to disc_dir and the HD package to hd_dir. Existing files with the right size are kept, so an
// interrupted setup resumes. Returns false with an error text (or "cancelled").
bool Extract(const std::wstring& iso, const IsoInfo& info, const std::wstring& disc_dir,
             const std::wstring& hd_dir, const Progress& progress, std::wstring& error);

// Quick check of an installed game folder (key files present with the expected sizes).
bool IsInstalled(const std::wstring& disc_dir, const std::wstring& hd_dir);

std::string Sha256File(const std::wstring& path);

}  // namespace ao4setup
