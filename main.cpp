#include <cassert>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <variant>
#include <vector>

extern "C" void signalHandler(int signal);

namespace
{
// RAII based signal handler
struct SignalHandler
{
  static const std::vector<int> signals;
  static volatile std::sig_atomic_t GOT_SIGNAL;

  SignalHandler()
  {
    for (const auto sig : signals)
    {
      std::signal(sig,  signalHandler);
    }
  }

  ~SignalHandler()
  {
    for (const auto sig : signals)
    {
      std::signal(sig,  SIG_DFL);
    }
  }

  static bool gotSignal()
  {
    return GOT_SIGNAL;
  }

  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;
};
// NOTE: don't handle SIGABRT for now, because it may mess with assert()
// TODO: think about replacing assert() with some other clean shutdown stuff
const std::vector<int> SignalHandler::signals{SIGINT, SIGTERM};
volatile std::sig_atomic_t SignalHandler::GOT_SIGNAL = 0;
}

// this is a C signal handler, so it unfortunately needs to live in global scope
//  and be very simple
void signalHandler([[maybe_unused]] int signal)
{
  SignalHandler::GOT_SIGNAL = 1;
}

namespace
{
using ByteType   = std::uint8_t;
using ByteBuffer = std::vector<ByteType>;
using ByteStream = std::fstream;

constexpr std::uint16_t toNum(const ByteType val)
{
  return val;
}

// overload to optimize for single byte case
void leToNum(ByteType& out, const ByteBuffer& data)
{
  assert(sizeof(ByteType) >= sizeof(ByteBuffer::value_type));
  assert(!data.empty());
  out = data.at(0);
}

// convert little-endian bytes to a numeric value
template<typename T> void leToNum(T& out, const ByteBuffer& data)
{
  out = {};
  assert(sizeof(T) >= data.size());
  for (std::size_t i{0}; i < data.size(); ++i)
  {
    out |= static_cast<T>(data[i] & ByteType{0xFF}) << (i * 8);
  }
}

// overload to optimize for single byte case
void readNum(ByteType& out, ByteStream& stream, const std::size_t len)
{
  assert(1 == len);
  char temp{};
  stream.read(&temp, 1);
  out = (ByteType&)temp;
}

template<typename T>
void readNum(T& out, ByteStream& stream, const std::size_t len)
{
  assert(len > 0);
  assert(sizeof(T) >= len);

  // this would execute only for 1-byte reads into non-ByteType variables
  if (1 == len)
  {
    char temp{};
    stream.read(&temp, 1);
    out = static_cast<T>(temp);
    return;
  }

  out = {};
  ByteBuffer buffer(len);
  assert(sizeof(char) == sizeof(ByteBuffer::value_type));
  stream.read((char*)buffer.data(), len);
  leToNum(out, buffer);
}

std::string readString(ByteStream& stream, std::size_t len)
{
  std::string retVal(len, ' ');
  stream.read(&(retVal.at(0)), len);
  return retVal;
}

struct DiskInfo
{
  // DOS 2.0 boot sector
  std::uint32_t jump{};
  std::string   oemName{};
  // DOS 2.0 BIOS parameter block
  std::uint16_t bytesPerSector{};
  ByteType      sectorsPerCluster{};
  std::uint16_t reservedSectors{};
  ByteType      fatCount{};
  std::uint16_t oldRootDirMax{};
  std::uint16_t oldTotalSectors{};
  ByteType      mediaDescriptor{};
  std::uint16_t oldSectorsPerFat{};
  // DOS 3.31 BIOS parameter block
  std::uint16_t sectorsPerTrack{};
  std::uint16_t headCount{};
  std::uint32_t hiddenSectorCount{};
  std::uint32_t totalSectors{};
  // FAT32 extended BIOS parameter block
  std::uint32_t sectorsPerFat{};
  std::uint16_t driveFlags{};
  ByteType      versionLo{};
  ByteType      versionHi{};
  std::uint32_t rootDirStartCluster{};
  std::uint16_t fsInfoSector{};
  std::uint16_t backupBootSector{};
  std::uint32_t reserved1{};
  std::uint32_t reserved2{};
  std::uint32_t reserved3{};
  ByteType      driveNumber{};
  ByteType      misc1{};
  ByteType      extBootSig{};
  std::uint32_t volumeId{};
  std::string   volumeLabel{};
  std::string   fsType{};
  // FS Information Sector
  std::uint32_t fsisSignature1{};
  //  480 reserved bytes not read
  std::uint32_t fsisSignature2{};
  std::uint32_t fsisFreeClusters{};
  std::uint32_t fsisRecentCluster{};
  //  12 reserved bytes not read
  std::uint32_t fsisSignature3{};
  // DERIVED VALUES
  std::uint32_t bytesPerCluster{};
  std::uint32_t totalClusters{};
  std::size_t   totalBytes{};
  std::size_t   cluster2StartByte{};
  std::size_t   dirEntriesPerCluster{};

  // copy the given source cluster's data to the given destination cluster
  // does nothing if cluster indices are out of range
  bool CopyCluster(
    ByteStream& stream,
    const std::uint32_t source,
    const std::uint32_t destination) const
  {
    if (!SeekToDataAreaCluster(stream, source))
    {
      std::cout << "ERROR: DiskInfo::CopyCluster(): Failed to seek to source data cluster " << std::dec << source << '\n';
      return false;
    }
    ByteBuffer buf(bytesPerCluster);
    stream.read((char*)buf.data(), bytesPerCluster);
    if (!stream.good())
    {
      std::cout << "ERROR: DiskInfo::CopyCluster(): Failed to read source data cluster " << std::dec << source << '\n';
      return false;
    }
    if (!SeekToDataAreaCluster(stream, destination))
    {
      std::cout << "ERROR: DiskInfo::CopyCluster(): Failed to seek to destination data cluster " << std::dec << destination << '\n';
      return false;
    }
    stream.write((char*)buf.data(), bytesPerCluster);
    if (!stream.good())
    {
      std::cout << "ERROR: DiskInfo::CopyCluster(): Failed to write destination data cluster " << std::dec << destination << '\n';
      return false;
    }
    return true;
  }

  // attempt to populate from an ifstream that provides binary read access to
  //  the partition
  void Get(ByteStream& stream)
  {
    // seek to start of stream
    stream.seekg(0);
    // DOS 2.0 boot sector
    readNum(jump, stream, 3);
    oemName = readString(stream, 8);
    // oemName[8] = '\0';
    // DOS 2.0 BIOS parameter block
    readNum(bytesPerSector, stream, 2);
    readNum(sectorsPerCluster, stream, 1);
    readNum(reservedSectors, stream, 2);
    readNum(fatCount, stream, 1);
    readNum(oldRootDirMax, stream, 2);
    readNum(oldTotalSectors, stream, 2);
    readNum(mediaDescriptor, stream, 1);
    readNum(oldSectorsPerFat, stream, 2);
    // DOS 3.31 BIOS parameter block
    readNum(sectorsPerTrack, stream, 2);
    readNum(headCount, stream, 2);
    readNum(hiddenSectorCount, stream, 4);
    readNum(totalSectors, stream, 4);
    // FAT32 extended BIOS parameter block
    readNum(sectorsPerFat, stream, 4);
    readNum(driveFlags, stream, 2);
    readNum(versionLo, stream, 1);
    readNum(versionHi, stream, 1);
    readNum(rootDirStartCluster, stream, 4);
    readNum(fsInfoSector, stream, 2);
    readNum(backupBootSector, stream, 2);
    readNum(reserved1, stream, 4);
    readNum(reserved2, stream, 4);
    readNum(reserved3, stream, 4);
    readNum(driveNumber, stream, 1);
    readNum(misc1, stream, 1);
    readNum(extBootSig, stream, 1);
    readNum(volumeId, stream, 4);
    volumeLabel = readString(stream, 11);
    fsType = readString(stream, 8);
    // FS Information Sector
    //  seek to start of sector
    SeekToAbsoluteSector(stream, fsInfoSector);
    readNum(fsisSignature1, stream, 4);
    //  seek past 480 byte reserved block
    stream.seekg(480, std::ios::cur);
    readNum(fsisSignature2, stream, 4);
    readNum(fsisFreeClusters, stream, 4);
    readNum(fsisRecentCluster, stream, 4);
    //  seek past 12 byte reserved block
    stream.seekg(12, std::ios::cur);
    readNum(fsisSignature3, stream, 4);

    // DERIVED VALUES
    bytesPerCluster = bytesPerSector * toNum(sectorsPerCluster);
    totalClusters = totalSectors / toNum(sectorsPerCluster);
    totalBytes = static_cast<std::size_t>(totalSectors) * bytesPerSector;
    cluster2StartByte =
      (
        static_cast<std::size_t>(reservedSectors) +
        (static_cast<std::size_t>(fatCount) * sectorsPerFat)
      ) * bytesPerSector;
    dirEntriesPerCluster = bytesPerCluster / 32;
  }

  void Print() const
  {
    std::cout
      << "DOS 2.0 boot sector:\n"
      << "\tJump instruction: 0x" << std::hex << jump << '\n'
      << "\tOEM name: \"" << oemName << "\"\n"
      << "DOS 2.0 BIOS parameter block:\n"
      << "\tBytes per logical sector: " << std::dec << bytesPerSector << '\n'
      << "\tLogical sectors per cluster: " << std::dec << toNum(sectorsPerCluster) << '\n'
      << "\tBytes per cluster (derived): " << std::dec << bytesPerCluster << '\n'
      << "\tReserved logical sectors: " << std::dec << reservedSectors << " (" << reservedSectors / toNum(sectorsPerCluster) << " clusters / " << reservedSectors * bytesPerSector << " bytes)\n"
      << "\tNumber of FATs (should be 1 or 2): " << std::dec << toNum(fatCount) << '\n'
      << "\tLegacy maximum number of root dir entries (should be zero for FAT32): " << std::dec << oldRootDirMax << '\n'
      << "\tLegacy total logical sectors (should be zero for FAT32): " << std::dec << oldTotalSectors << '\n'
      << "\tMedia descriptor (should be 0xF8 for a hard drive partition): 0x" << std::hex << toNum(mediaDescriptor) << '\n'
      << "\tLegacy logical sectors per FAT (should be zero for FAT32): " << std::dec << oldSectorsPerFat << '\n'
      << "DOS 3.31 BIOS parameter block:\n"
      << "\tSectors per track: " << std::dec << sectorsPerTrack << '\n'
      << "\tNumber of heads: " << std::dec << headCount << '\n'
      << "\tHidden sector count: " << std::dec << hiddenSectorCount << '\n'
      << "\tTotal logical sectors: " << std::dec << totalSectors << " (" << totalClusters << " clusters / " << totalBytes << " bytes)" << '\n'
      << "FAT32 extended BIOS parameter block:\n"
      << "\tLogical sectors per FAT: " << std::dec << sectorsPerFat << " (" << sectorsPerFat / toNum(sectorsPerCluster) << " clusters / " << sectorsPerFat * bytesPerSector << " bytes)\n"
      << "\tDrive flags: 0x" << std::hex << driveFlags << '\n'
      << "\tVersion: " << std::dec << toNum(versionHi) << "." << toNum(versionLo) << '\n'
      << "\tRoot directory start cluster: " << std::dec << rootDirStartCluster << '\n'
      << "\tFS Information Sector index: " << std::dec << fsInfoSector << '\n'
      << "\tBackup boot sector index: " << std::dec << backupBootSector << '\n'
      << "\tReserved data: 0x" << std::hex << reserved3 << " 0x" << reserved2 << " 0x" << reserved1 << '\n'
      << "\tPhysical drive number: " << std::dec << toNum(driveNumber) << '\n'
      << "\tMisc data: 0x" << std::hex << toNum(misc1) << '\n'
      << "\tExtended boot signature: 0x" << std::hex << toNum(extBootSig) << '\n'
      << "\tVolume ID: 0x" << std::hex << volumeId << '\n'
      << "\tVolume label: \"" << volumeLabel << "\"\n"
      << "\tFilesystem type: \"" << fsType << "\"\n"
      << "FS Information Sector:\n"
      << "\tFSIS signature 1: 0x" << std::hex << fsisSignature1 << '\n'
      << "\tFSIS signature 2: 0x" << std::hex << fsisSignature2 << '\n'
      << "\tFSIS free clusters: " << std::dec << fsisFreeClusters << '\n'
      << "\tFSIS last allocated cluster: " << std::dec << fsisRecentCluster << '\n'
      << "\tFSIS signature 3: 0x" << std::hex << fsisSignature3 << '\n'
    ;
  }

  // seek the given stream to the start of the given data area cluster index,
  //  based on the disk info stored in `this`
  // cluster index must be >= 2
  template<typename T>
  bool SeekToDataAreaCluster(T& stream, const std::size_t cluster) const
  {
    if (cluster < 2 || cluster >= totalClusters)
    {
      return false;
    }
    stream.seekg(cluster2StartByte + ((cluster - 2) * bytesPerCluster));
    return stream.good();
  }

  // seek the given stream to the start of the given sector index, based on the
  //  disk info stored in `this`
  void SeekToAbsoluteSector(ByteStream& stream, const std::size_t sector) const
  {
    stream.seekg(sector * bytesPerSector);
  }

  explicit DiskInfo(ByteStream& stream)
  {
    Get(stream);
  }
};

struct FileAllocationTable32
{
  // constants
  //  28-bit special markers
  //   free cluster
  enum FAT32
  {
    // free cluster
    FREE = std::uint32_t{0x00000000},
    // temporary allocation (treat as end of chain)
    EOCT = std::uint32_t{0x00000001},
    // reserved values
    //  seems these should be treated as data, but probably only if they're
    //  somehow valid cluster numbers?
    RSV0 = std::uint32_t{0x0ffffff0},
    RSV1 = std::uint32_t{0x0ffffff1},
    RSV2 = std::uint32_t{0x0ffffff2},
    RSV3 = std::uint32_t{0x0ffffff3},
    RSV4 = std::uint32_t{0x0ffffff4},
    RSV5 = std::uint32_t{0x0ffffff5},
    RSV6 = std::uint32_t{0x0ffffff6},
    // bad cluster?
    BADC = std::uint32_t{0x0ffffff7},
    // alternate end of chain markers
    //  NOTE: Linux uses EOC8 for marking the end of the root directory
    EOC8 = std::uint32_t{0x0ffffff8},
    EOC9 = std::uint32_t{0x0ffffff9},
    EOCA = std::uint32_t{0x0ffffffa},
    EOCB = std::uint32_t{0x0ffffffb},
    EOCC = std::uint32_t{0x0ffffffc},
    EOCD = std::uint32_t{0x0ffffffd},
    EOCE = std::uint32_t{0x0ffffffe},
    // normal end of chain marker
    EOCN = std::uint32_t{0x0fffffff}
  };

  // metadata
  //  byte index of table start on disk
  std::uint64_t startByte{};

  // actual table contents
  std::vector<std::uint32_t> entries{};

  std::uint32_t GetFATID() const
  {
    assert(entries.size() > 0);
    return entries.at(0);
  }

  std::uint32_t GetFlags() const
  {
    assert(entries.size() > 1);
    return entries.at(1);
  }

  // attempt to populate from an ifstream that provides binary read access to
  //  the FAT at the given byte location with the given length in bytes
  void Get(
    ByteStream& stream,
    const std::uint64_t byteLocation,
    const std::uint32_t byteLength)
  {
    // cache table start location
    startByte = byteLocation;
    // seek stream to table start
    stream.seekg(byteLocation);
    // drop existing table (if any)
    entries.clear();
    // each entry is 32 bits (4 bytes) in size
    const std::uint32_t numEntries{byteLength / 4};
    // allocate (but don't initialize) enough space for the entire table
    entries.reserve(numEntries);
    std::uint32_t temp{};
    for (std::uint32_t i{0}; i < numEntries; ++i)
    {
      // read into temp var, then append to table
      // this should be fairly efficient, as the table RAM is already allocated
      readNum(temp, stream, 4);
      entries.push_back(temp);
    }
  }

  FileAllocationTable32(
    ByteStream& stream,
    const std::uint64_t byteLocation,
    const std::uint32_t byteLength)
  {
    Get(stream, byteLocation, byteLength);
  }

  static bool IsBad(const std::uint32_t data)
  {
    return data == FAT32::BADC;
  }

  bool IsBadAt(const std::uint32_t index) const
  {
    return IsBad(entries.at(index));
  }

  static bool IsCluster(const std::uint32_t data)
  {
    return data > FAT32::EOCT && data < FAT32::RSV0;
  }

  bool IsClusterAt(const std::uint32_t index) const
  {
    return IsCluster(entries.at(index));
  }

  static bool IsEnd(const std::uint32_t data)
  {
    return data == FAT32::EOCT
        || (data >= FAT32::EOC8 && data <= FAT32::EOCN);
  }

  bool IsEndAt(const std::uint32_t index) const
  {
    return IsEnd(entries.at(index));
  }

  static bool IsFree(const std::uint32_t data)
  {
    return data == FAT32::FREE;
  }

  bool IsFreeAt(const std::uint32_t index) const
  {
    return IsFree(entries.at(index));
  }

  // get all clusters in FAT chain following the given start cluster
  // start cluster IS included
  // no entry is provided for end of cluster marker, as vectors have a known
  //  length
  std::vector<std::uint32_t> GetClusterChain(
    const std::uint32_t startCluster) const
  {
    if (startCluster < 2 || startCluster >= entries.size()) return {};
    std::vector<std::uint32_t> retVal{};
    for (
      auto curCluster{startCluster};
      !IsEnd(curCluster);
      curCluster = entries.at(curCluster)
    )
    {
      if (!IsCluster(curCluster))
      {
        std::cout << "WARNING: Aborting read of FAT chain for start cluster " << std::dec << startCluster << " because special value 0x" << std::hex << curCluster << " was unexpectedly encountered\n";
        std::cout << "         Chain of size " << retVal.size() << " looks like:";
        for (const auto c : retVal)
        {
          std::cout << " " << std::dec << c << " (0x" << std::hex << c << ')';
        }
        std::cout << '\n';
        break;
      }
      retVal.emplace_back(curCluster);
    }
    return retVal;
  }

  // get the "ideal" cluster chain starting at the given index with the given
  //  length
  // this is basically a monotonically increasing index chain, except that it
  //  skips any bad clusters
  std::vector<std::uint32_t> GetIdealClusterChain(
    const std::uint32_t startCluster, const std::size_t length) const
  {
    if (!length) return {};
    std::vector<std::uint32_t> retVal{};
    retVal.reserve(length);
    retVal.push_back(startCluster);
    while (retVal.size() < length)
    {
      const auto nextCluster{GetNextGoodCluster(retVal.back())};
      if (!nextCluster) return {};
      retVal.push_back(nextCluster);
    }
    return retVal;
  }

  // return the cluster index of the first free cluster from the given cluster
  //  index onwards
  std::uint32_t GetFirstFreeCluster(std::uint32_t cluster) const
  {
    // abort if start cluster is illegal, or table is empty
    if (cluster < 2 || entries.empty() || cluster >= entries.size() - 1)
    {
      return 0;
    }
    while (!IsFree(entries.at(cluster)))
    {
      // abort if we're out of clusters
      if (cluster >= entries.size() - 1) return 0;
      // advance
      ++cluster;
    }
    return cluster;
  }

  // return the cluster index of the next non-bad cluster following the given
  //  cluster index
  std::uint32_t GetNextGoodCluster(std::uint32_t cluster) const
  {
    // abort if start cluster is illegal, or table is empty
    if (cluster < 2 || entries.empty() || cluster >= entries.size() - 1)
    {
      return 0;
    }
    // advance to next cluster
    ++cluster;
    // keep advancing until a good cluster is reached
    while (IsBad(entries.at(cluster)))
    {
      // abort if we're out of clusters
      if (cluster >= entries.size() - 1) return 0;
      // advance
      ++cluster;
    }
    return cluster;
  };

  void Print(const std::string& indentString = {}) const
  {
    std::cout
      << indentString << "Entries: " << std::dec << entries.size() << '\n'
      << indentString << "FAT ID: 0x" << std::hex << GetFATID() << '\n'
      << indentString << "Flags: 0x" << std::hex << GetFlags() << '\n'
    ;
    std::cout << indentString << "First (up to) 16 entries:\n";
    Print(0, 16, indentString + indentString);
    const auto lastCount{entries.size() < 16 ? entries.size() : 16};
    std::cout << indentString << "Last " << std::dec << lastCount << " entries:\n";
    Print(
      static_cast<std::uint32_t>(entries.size() - lastCount),
      lastCount, indentString + indentString);
  }

  void Print(
    const std::uint32_t startCluster,
    const std::size_t   numClusters,
    const std::string&  indentString = {}) const
  {
    assert(startCluster < entries.size());
    std::cout
      << indentString << "CLUSTER #     +0         +1         +2         +3\n";
    for (std::size_t j{startCluster};
         j < startCluster + numClusters && j < entries.size();
         ++j)
    {
      const auto countThisLine{j - startCluster};
      if (countThisLine & 0x3)
      {
        // non-initial value on line
        // prepend with a space
        std::cout << ' ';
      }
      else
      {
        // initial value on line
        // prepend with tabs and cluster index
        std::cout
          << indentString
          << std::dec << std::setw(9) << std::setfill(' ') << j
          << ' '
        ;
      }
      std::cout
        << "0x" << std::hex << std::setw(8) << std::setfill('0')
        << entries.at(j)
      ;
      // final value on line
      // append newline
      if (0x3 == (countThisLine & 0x3)) std::cout << '\n';
    }
  }

  bool Write(
    ByteStream& stream, const std::uint32_t index, const std::uint32_t data)
  {
    if (index < 2 || index >= entries.size())
    {
      std::cout << "ERROR: FileAllocationTable32::Write(): Bad index " << std::dec << index << '\n';
      return false;
    }
    stream.seekp(startByte + (index * 4));
    if (!stream.good())
    {
      std::cout << "ERROR: FileAllocationTable32::Write(): Failed to seek stream to index " << std::dec << index << '\n';
      return false;
    }
    // pack data into a 4-byte buffer in little-endian order
    const ByteBuffer buf
    {
      static_cast<ByteType>( data        & 0xff),
      static_cast<ByteType>((data >>  8) & 0xff),
      static_cast<ByteType>((data >> 16) & 0xff),
      static_cast<ByteType>((data >> 24) & 0xff)
    };
    stream.write((const char*)(buf.data()), 4);
    if (!stream.good())
    {
      std::cout << "ERROR: FileAllocationTable32::Write(): Strem write failed for index " << std::dec << index << '\n';
      return false;
    }
    // update memory cache last, to ensure if reflects the disk state
    entries.at(index) = data;
    return true;
  }

  // compare two FATs for data equivalency
  bool Compare(const FileAllocationTable32& rhs) const
  {
    // NOTE: don't care about startByte, just compare entries

    if (rhs.entries.size() != entries.size())
    {
      std::cout << "\tFAT tables have different sizes\n";
      return false;
    }

    std::size_t diffs{0};
    for (std::size_t i{0}; i < entries.size(); ++i)
    {
      if (rhs.entries.at(i) == entries.at(i)) continue;
      if (++diffs > 100) continue;
      std::cout
        << " @0x" << std::hex << std::setw(8) << std::setfill('0') << i
        << "  0x" << std::hex << std::setw(8) << std::setfill('0') << entries.at(i)
        << "  0x" << std::hex << std::setw(8) << std::setfill('0') << rhs.entries.at(i)
        << '\n';
      if (100 == diffs)
      {
        std::cout << "\t(maximum of 100 diffs found; suppressing any additional findings)\n";
      }
    }

    std::cout << "\t" << std::dec << diffs << " difference(s) found\n";
    return !diffs;
  }

  // get a vector of all FAT tables from a stream with the given disk info
  static std::vector<FileAllocationTable32> GetAll(const DiskInfo& diskInfo, ByteStream& stream)
  {
    const std::uint64_t fatAreaStartByte
    {
      static_cast<std::uint64_t>(diskInfo.reservedSectors) * diskInfo.bytesPerSector
    };
    const std::uint64_t fatSizeBytes
    {
      static_cast<std::uint64_t>(diskInfo.sectorsPerFat) * diskInfo.bytesPerSector
    };
    std::vector<FileAllocationTable32> fat{};
    for (std::size_t i{0}; i < toNum(diskInfo.fatCount); ++i)
    {
      const std::uint64_t fatStartByte{fatAreaStartByte + (fatSizeBytes * i)};
      fat.emplace_back(stream, fatStartByte, fatSizeBytes);
      const auto& fatI{fat.at(i)};
      std::cout
        << "FAT table #" << std::dec << i << ", starting at byte index " << std::dec << fatStartByte << ":\n";
      fatI.Print("\t");
    }
    return fat;
  }
};

struct DirectoryEntry
{
  // special initial bytes for short file name field
  // these are absolute values, not bit masks
  enum DIR
  {
    // end of directory list (available for use)
    AEND = ByteType{0x00},
    // interpret first character as literal 0xE5
    ISE5 = ByteType{0x05},
    // entry is "." or ".."
    DOTS = ByteType{0x2E},
    // erased entry (available for use)
    ADEL = ByteType{0xE5}
  };
  // file attribute bit flags
  enum ATTR
  {
    // VFAT long filename pattern (overrides 0x01 through 0x08)
    VFATLN = ByteType{0x0F},
    // standard bit flags
    //  read only
    RDONLY = ByteType{0x01},
    //  hidden
    HIDDEN = ByteType{0x02},
    //  system
    SYSTEM = ByteType{0x04},
    //  volume label
    VLABEL = ByteType{0x08},
    //  subdirectory
    SUBDIR = ByteType{0x10},
    //  archive
    ARCHIV = ByteType{0x20},
    //  device
    DEVICE = ByteType{0x40},
    //  reserved (do not chage)
    RESERV = ByteType{0x80}
  };
  // VFAT long filename sequence data values/masks
  enum LFN
  {
    // mask: LFN chain sequence number
    NUM = ByteType{0x1F},
    // mask: distant end of LFN chain
    END = ByteType{0x40},
    // absolute value: deleted entry
    DEL = ByteType{0xE5}
  };

  // fields
  //  standard FAT32 dir/file entry
  std::string   shortName{};
  std::string   shortExt{};
  ByteType      attributes{};
  ByteType      flags{};
  ByteType      lifecycleInfo{};
  std::uint16_t createTime{};
  std::uint16_t createDate{};
  std::uint16_t accessDate{};
  std::uint16_t startClusterHi{};
  std::uint16_t modifyTime{};
  std::uint16_t modifyDate{};
  std::uint16_t startClusterLo{};
  std::uint32_t sizeBytes{}; // NOTE: should be 0 for labels/subdirs
  //  long filename overlay data
  ByteType      sequenceData{};
  ByteBuffer    lfnBytes{};

  // populate from 32 bytes read from current stream position
  void Get(ByteStream& stream)
  {
    // cache current stream position in case we need to rewind to get LFN data
    const auto startByte{stream.tellg()};
    // NOTE: this will be garbage if it's an LFN entry; it's up to the user to
    //  deal with that
    shortName = readString(stream, 8);
    shortExt = readString(stream, 3);
    readNum(attributes, stream, 1);
    readNum(flags, stream, 1);
    readNum(lifecycleInfo, stream, 1);
    readNum(createTime, stream, 2);
    readNum(createDate, stream, 2);
    readNum(accessDate, stream, 2);
    readNum(startClusterHi, stream, 2);
    readNum(modifyTime, stream, 2);
    readNum(modifyDate, stream, 2);
    readNum(startClusterLo, stream, 2);
    readNum(sizeBytes, stream, 4);
    // if this is a long filename entry, grab relevant data
    if (IsLongFilenameData())
    {
      // rewind to record start
      stream.seekg(startByte);
      readNum(sequenceData, stream, 1);
      lfnBytes.resize(26);
      // read 10 bytes
      stream.read((char*)lfnBytes.data(), 10);
      // skip over 3 bytes
      stream.seekg(3, std::ios_base::cur);
      // read + append 12 bytes
      stream.read((char*)(lfnBytes.data() + 10), 12);
      // skip 2 bytes
      stream.seekg(2, std::ios_base::cur);
      // read + append 4 bytes
      stream.read((char*)(lfnBytes.data() + 22), 4);
      // stream pointer is now back where it was
    }
    else
    {
      sequenceData = 0;
      lfnBytes.clear();
    }
  }

  std::string GetFilename() const
  {
    if (IsLongFilenameData() || IsEnd()) return {};
    std::string base{shortName};
    if (DIR::ISE5 == shortName[0])
    {
      base.at(0) = char(0xE5);
    }
    // discard any trailing spaces that were added for padding purposes
    while (' ' == base.back())
    {
      base.pop_back();
    }
    std::string ext{shortExt};
    // discard any trailing spaces that were added for padding purposes
    while (' ' == ext.back())
    {
      ext.pop_back();
    }
    if (ext.empty()) return base;
    if (IsVolumeLabel()) return base + ext;
    return base + '.' + ext;
  }

  std::uint8_t GetLFNSequenceNumber() const
  {
    return IsLongFilenameData() ? sequenceData & LFN::NUM : LFN::END;
  }

  std::uint32_t GetStartCluster() const
  {
    return (static_cast<std::uint32_t>(startClusterHi) << 16) | startClusterLo;
  }

  // IsXYZ() NOTES:
  // - precedence is: LFN > shortName[0] > attributes
  // - some LFN-related stuff marked "SEQ" also checks sequence data

  // (ATTR) return whether entry is a device
  bool IsDevice() const
  {
    return !IsLongFilenameData()        // LFN
        && !IsEnd()                     // SN0
        && !IsErased()                  // SN0
        && (attributes & ATTR::DEVICE); // ATTR
  }

  // (ATTR) return whether entry is a directory (subdirectory or dot entry)
  bool IsDirectory() const
  {
    return !IsLongFilenameData()        // LFN
        && !IsEnd()                     // SN0
        && !IsErased()                  // SN0
        && (attributes & ATTR::SUBDIR); // ATTR
  }

  // (ATTR) return whether entry is "." or ".." directory
  bool IsDotEntry() const
  {
    return !IsLongFilenameData()                              // LFN
        && (DIR::DOTS == static_cast<ByteType>(shortName[0])) // SN0
        && (attributes & ATTR::SUBDIR);                       // ATTR
  }

  // (SN0) return whether entry is an end-of-directory-listing marker
  bool IsEnd() const
  {
    return !IsLongFilenameData()                                // LFN
        && (DIR::AEND == static_cast<ByteType>(shortName[0]));  // SN0
  }

  // (SEQ) return whether entry is an end-of-long-filename marker
  bool IsEndLFN() const
  {
    return IsLongFilenameData()       // LFN
        && !IsErasedLFN()             // LFN
        && (sequenceData & LFN::END); // SEQ
  }

  // (SN0) return whether entry is an empty/erased slot
  bool IsErased() const
  {
    return !IsLongFilenameData()                                // LFN
        && (DIR::ADEL == static_cast<ByteType>(shortName[0]));  // SN0
  }

  // (SEQ) return whether entry is an erased long filename slot
  bool IsErasedLFN() const
  {
    return IsLongFilenameData()         // LFN
        && (LFN::DEL == sequenceData);  // SEQ
  }

  // (ATTR) return whether entry is a regular file
  bool IsFile() const
  {
    return !IsLongFilenameData()  // LFN
        && !IsEnd()               // SN0
        && !IsErased()            // SN0
        && !IsDevice()            // ATTR
        && !IsDirectory()         // ATTR
        && !IsDotEntry()          // ATTR
        && !IsVolumeLabel()       // ATTR
    ;
  }

  // (LFN) return whether entry is LFN metadata
  // this is based on a special combination of attributes, and should be checked
  //  before shortName[0] based special bytes or other attributes when
  //  attempting to determine an entry's nature
  bool IsLongFilenameData() const
  {
    return ATTR::VFATLN == attributes;  // LFN
  }

  // (ATTR) return whether entry is a subdirectory (non-dot directory)
  bool IsSubdirectory() const
  {
    return IsDirectory()  // ATTR
        && !IsDotEntry(); // ATTR
  }

  // (ATTR) return whether entry is a volume label
  bool IsVolumeLabel() const
  {
    return !IsLongFilenameData()        // LFN
        && !IsEnd()                     // SN0
        && !IsErased()                  // SN0
        && (attributes & ATTR::VLABEL); // ATTR
  }

  void Print(const FileAllocationTable32& fat) const
  {
    if (IsLongFilenameData())
    {
      // this is a long filename entry; print basic metadata and continue
      std::cout << "<LFN> a: 0x" << std::hex << toNum(sequenceData);
      if (IsErasedLFN())
      {
        std::cout << " <DEL>\n";
        return;
      }

      std::cout << " c: " << std::dec << toNum(GetLFNSequenceNumber());
      if (IsEndLFN())
      {
        std::cout << " <END>";
      }
      std::cout << '\n';
      return;
    }

    if (IsEnd())
    {
      std::cout << "<END>\n";
      return;
    }

    if (IsErased())
    {
      std::cout << "<DEL>\n";
      return;
    }

    if (IsVolumeLabel())
    {
      std::cout << "<VOL> ";
    }
    else if (IsDotEntry())
    {
      std::cout << "<DOT> ";
    }
    else if (IsDirectory())
    {
      std::cout << "<DIR> ";
    }
    else
    {
      std::cout << "<FIL> ";
    }

    // this is a standard file, directory, or volume label entry; print detailed
    //  data
    const bool printClusterCount{IsFile() || IsSubdirectory()};
    std::cout
      << '\"' << GetFilename() << '\"'
      << " a: 0x" << std::hex << std::setw(2) << std::setfill('0') << toNum(attributes)
      << " f: 0x" << std::hex << std::setw(2) << std::setfill('0') << toNum(flags)
      << " l: 0x" << std::hex << std::setw(2) << std::setfill('0') << toNum(lifecycleInfo)
      << " ct: " << DirectoryEntry::ToDate(createDate) << ' ' << DirectoryEntry::ToTime(createTime)
      << " at: " << DirectoryEntry::ToDate(accessDate)
      << " mt: " << DirectoryEntry::ToDate(modifyDate) << ' ' << DirectoryEntry::ToTime(modifyTime)
      << " sc: 0x" << std::hex << std::setw(8) << std::setfill('0') << GetStartCluster()
      << " cc: " << std::dec << (printClusterCount ? fat.GetClusterChain(GetStartCluster()).size() : 0)
      << " sb: " << std::dec << sizeBytes
      << '\n'
    ;
  }

  static std::string ToDate(const std::uint16_t word)
  {
    std::stringstream s;
    s << std::dec
      << 1980 + ((word >> 9) & 0x7F)
      << '/'
      << std::setw(2) << std::setfill('0') << ((word >> 5) & 0x0F)
      << '/'
      << std::setw(2) << std::setfill('0') << (word & 0x1F)
    ;
    return s.str();
  }

  static std::string ToTime(const std::uint16_t word)
  {
    std::stringstream s;
    s << std::dec
      << std::setw(2) << std::setfill('0') << ((word >> 11) & 0x1F)
      << ':'
      << std::setw(2) << std::setfill('0') << ((word >>  5) & 0x3F)
      << ':'
      << std::setw(2) << std::setfill('0') << ((word <<  1) & 0x3F)
    ;
    return s.str();
  }

  DirectoryEntry() = delete;
  DirectoryEntry(const DirectoryEntry&) = delete;
  DirectoryEntry& operator=(const DirectoryEntry&) = delete;

  explicit DirectoryEntry(ByteStream& stream)
  {
    Get(stream);
  }

  // this is required in order to make std::vector::emplace_back() work
  DirectoryEntry(DirectoryEntry&& rhs) = default;
};

struct Directory : public std::enable_shared_from_this<Directory>
{
  // parent directory, or nullptr for root directory
  std::shared_ptr<Directory> parent{};
  // contained LFN/file/subdirectory metadata entries, plus optional
  //  corresponding Directory object
  std::vector<DirectoryEntry> entryData{};
  std::vector<std::shared_ptr<Directory>> entryDir{};

  // read all data for the directory stored in the given FAT cluster chain
  // optionally recurse to follow the entire directory tree from the given
  //  starting directory
  void Get(
    ByteStream& stream,
    const DiskInfo& diskInfo,
    const FileAllocationTable32& fat,
    const std::uint32_t startCluster,
    const bool recurse = false)
  {
    const auto maxEntriesPerCluster{diskInfo.bytesPerCluster / 32};
    entryData.clear();
    entryDir.clear();

    // walk cluster chain, in case directory somehow spans multiple clusters
    bool abort{false};
    for (const auto cluster : fat.GetClusterChain(startCluster))
    {
      // stop the whole show if we've already read an end-of-directory record
      if (abort) break;
      // seek stream to start of cluster
      if (!diskInfo.SeekToDataAreaCluster(stream, cluster))
      {
        std::cout << "ERROR: Directory::Get(): Seek to cluster " << std::dec << cluster << " failed\n";
        abort = true;
        continue;
      }
      // read up to maxEntriesPerCluster directory entries from this cluster
      for (std::size_t i{0}; i < maxEntriesPerCluster; ++i)
      {
        // decode current stream position into a new directory entry object
        const auto& myEntryData{entryData.emplace_back(stream)};
        entryDir.emplace_back(nullptr);
        // stop the show if this is an end-of-directory record
        if (myEntryData.IsEnd())
        {
          abort = true;
          break;
        }
      }
    }

    // stop here if recursion is NOT requested
    if (!recurse) return;

    // do a breadth-first walk of all subdirectories for disk read efficiency
    for (std::size_t i{0}; i < entryData.size(); ++i)
    {
      const auto& curEntry{entryData.at(i)};
      auto& curDir{entryDir.at(i)};
      // abort if end record found
      if (curEntry.IsEnd()) break;
      // skip anything that's not a proper subdirectory entry
      if (!curEntry.IsSubdirectory()) continue;
      // recursively call this method for this subdirectory
      curDir = std::make_shared<Directory>(shared_from_this());
      curDir->Get(
        stream,
        diskInfo,
        fat,
        curEntry.GetStartCluster(),
        recurse);
    }
  }

  // constructor only allows population of parent pointer, which can be nullptr
  //  for the root directory
  // use Get() to populate data from stream
  explicit Directory(std::shared_ptr<Directory> parentSptr)
    : parent(parentSptr)
  {
  }

  Directory() = delete;
  Directory(const Directory&) = delete;
  Directory& operator= (const Directory&) = delete;
  Directory(Directory&&) = delete;

  // change the start cluster of the entry at the given index, both in memory
  //  and on disk
  // only works for files and subdirectories
  // returns whether operation succeeded
  bool ChangeStartClusterAt(
    ByteStream& stream,
    const DiskInfo& diskInfo,
    const FileAllocationTable32& fat,
    const std::size_t entryIndex,
    const std::uint32_t newStartCluster)
  {
    // std::cout << "ChangeStartClusterAt(): BEFORE: ";
    // entryData.at(entryIndex).Print(fat);

    // sanity checks
    if (entryIndex >= entryData.size())
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): Index out of range\n";
      return false;
    }
    if (newStartCluster < 2 || newStartCluster >= fat.entries.size())
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): Start cluster out of range\n";
      return false;
    }
    // calculate how many clusters into our own FAT chain the given entry lives
    const auto fatChainIndex{entryIndex / diskInfo.dirEntriesPerCluster};
    // get our own start cluster & FAT chain and sanity check
    auto ownStartCluster{GetStartCluster()};
    // root directory doesn't have a "." entry, so assume that's who we are if
    //  we also have no parent pointer
    if (!ownStartCluster && !parent)
    {
      ownStartCluster = diskInfo.rootDirStartCluster;
    }
    if (!ownStartCluster)
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): Failed to get own start cluster\n";
      return false;
    }
    const auto& fatChain{fat.GetClusterChain(ownStartCluster)};
    if (fatChainIndex >= fatChain.size())
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): FAT chain smaller than expected\n";
      return false;
    }
    const auto entryCluster{fatChain.at(fatChainIndex)};
    if (!FileAllocationTable32::IsCluster(entryCluster))
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): FAT chain entry for containing cluster is invalid\n";
      return false;
    }

    // seek to start of entry's containing cluster, then to start of entry data
    if (!diskInfo.SeekToDataAreaCluster(stream, entryCluster))
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): Failed to seek stream to start of entry's containing cluster\n";
      return false;
    }

    // calculate the byte offset of the given entry from the start of its
    //  containing cluster
    const auto entryOffsetBytes{
      (entryIndex % diskInfo.dirEntriesPerCluster) * 32};
    // now do a relative seek to the byte where the first bit of target data
    //  needs to go
    // NOTE: seekg() and seekp() are interchangeable for fstream
    stream.seekp(entryOffsetBytes + 20, std::ios_base::cur);
    if (!stream.good())
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): Failed to seek stream to start of entry's startClusterHi field\n";
      return false;
    }
    // write the high word of the new start cluster
    const ByteBuffer bufHi
    {
      static_cast<ByteType>((newStartCluster >> 16) & 0xff),
      static_cast<ByteType>((newStartCluster >> 24) & 0xff)
    };
    stream.write((const char*)(bufHi.data()), 2);

    // do a relative seek to where the low word needs to go
    // NOTE: the stream advanced 2 bytes as a result of the previous write
    stream.seekp(4, std::ios_base::cur);
    if (!stream.good())
    {
      std::cout << "ERROR: Directory::ChangeStartCluster(): Failed to seek stream to start of entry's startClusterLo field ***** SEVERE WARNING: DATA IS LIKELY NOW IN A BAD STATE *****\n";
      return false;
    }
    // write the low word of the new start cluster
    const ByteBuffer bufLo
    {
      static_cast<ByteType>( newStartCluster       & 0xff),
      static_cast<ByteType>((newStartCluster >> 8) & 0xff)
    };
    stream.write((const char*)(bufLo.data()), 2);

    // update in-memory entry
    auto& data{entryData.at(entryIndex)};
    data.startClusterHi = newStartCluster >> 16;
    data.startClusterLo = newStartCluster & 0xFFFF;

    // std::cout << "ChangeStartClusterAt(): AFTER: ";
    // entryData.at(entryIndex).Print(fat);

    return true;
  }

  static constexpr auto npos{std::string::npos};
  // find entry with the given start cluster, and return its index
  // returns Directory::npos if no match exists
  std::size_t Find(const std::uint32_t atCluster) const
  {
    for (std::size_t i{0}; i < entryData.size(); ++i)
    {
      const auto& data{entryData.at(i)};
      // skip LFN data entries
      if (data.IsLongFilenameData()) continue;
      // abort if end record reached
      if (data.IsEnd()) break;
      // skip erased data entries
      if (data.IsErased()) continue;
      // return match
      if (atCluster == data.GetStartCluster()) return i;
    }
    return npos;
  }

  std::string GetFileNameAt(const std::size_t index) const
  {
    if (index >= entryData.size()) return {};
    const auto& entry{entryData.at(index)};
    if (entry.IsLongFilenameData() || entry.IsEnd()) return {};

    const auto& lfn{GetLongFileNameAt(index)};

    return lfn.empty() ? entry.GetFilename() : lfn;
  }

  std::string GetLongFileNameAt(const std::size_t index) const
  {
    if (!index) return {};
    // find first entry with LFN data for given index
    std::size_t lfnStart{index - 1};
    while (true)
    {
      const auto& lfnEntry{entryData.at(lfnStart)};
      // abort if we ran out of LFN data without hitting the end record
      if (!lfnEntry.IsLongFilenameData()) return {};
      // stop if we found the end
      if (lfnEntry.IsEndLFN()) break;
      // abort if this is the very first directory entry
      if (!lfnStart) return {};
      // else decrement
      --lfnStart;
    }
    // lfnStart is now the first LFN entry, and index - 1 is the last one
    // accumulate data into a buffer
    ByteBuffer buf{};
    for (std::size_t i{lfnStart}; i < index; ++i)
    {
      const auto& entryLfnBytes{entryData.at(i).lfnBytes};
      buf.insert(buf.end(), entryLfnBytes.begin(), entryLfnBytes.end());
    }
    // buf now holds all the character data
    // strip off any 0xffff padding words
    while (buf.size() >= 2 && 0xff == buf.back() && 0xff == buf.at(buf.size() - 2))
    {
      buf.pop_back();
      buf.pop_back();
    }
    // also strip off any 0x0000 terminator words
    while (buf.size() >= 2 && 0x00 == buf.back() && 0x00 == buf.at(buf.size() - 2))
    {
      buf.pop_back();
      buf.pop_back();
    }
    std::filesystem::path lfn(
      (char16_t*)(buf.data()),
      (char16_t*)(buf.data() + buf.size())
    );
    return lfn.string();
  }

  // get own start cluster as reported by "." entry, or 0 if not found
  std::uint32_t GetStartCluster() const
  {
    for (const auto& data : entryData)
    {
      if (data.IsEnd()) break;
      if (data.IsDotEntry() && "." == data.GetFilename())
      {
        return data.GetStartCluster();
      }
    }
    return 0;
  }

  void Print(const FileAllocationTable32& fat) const
  {
    for (const auto& data : entryData)
    {
      std::cout << '\t';
      data.Print(fat);
    }
  }

  // get entire directory tree, as a pointer to the root directory
  static std::shared_ptr<Directory> GetAll(
    ByteStream& stream,
    const DiskInfo& diskInfo,
    const FileAllocationTable32& fat)
  {
    auto rootDirectory{std::make_shared<Directory>(nullptr)};
    rootDirectory->Get(
      stream, diskInfo, fat, diskInfo.rootDirStartCluster, true);
    return rootDirectory;
  }
};

// "reverse FAT" object that maps each cluster index to the previous cluster in
//  its FAT chain, or to containing Directory if initial cluster
struct Rfat
{
  using Entry = std::variant<std::shared_ptr<Directory>, std::uint32_t>;
  std::vector<Entry> entries{};

  void RecordChain(
    const FileAllocationTable32& fat,
    std::shared_ptr<Directory>   parentDir,
    const std::uint32_t          startCluster)
  {
    // get the rest of the clusters (if any) and record previous cluster in
    //  chain at each index
    bool start{true};
    // start with prevCluster=startCluster, but we won't use it on the initial
    //  entry, because that needs to point at the directory
    auto prevCluster{startCluster};
    const auto& forwardChain{fat.GetClusterChain(startCluster)};
    for (const auto curCluster : forwardChain)
    {
      if (start)
      {
        // first entry points at the parent directory (or nullptr for root dir)
        entries.at(curCluster) = parentDir;
        // std::cout << "DIR <- 0x" << std::hex << startCluster;
        start = false;
      }
      else
      {
        // subsequent entries point at previous cluster in chain
        entries.at(curCluster) = prevCluster;
        // std::cout << ", 0x" << std::hex << prevCluster << " <- 0x" << std::hex << curCluster;
      }
      prevCluster = curCluster;
    }
    // std::cout << '\n';
  }

  // process the given directory with the given start cluster, populating the
  //  reverse-FAT table for all of its contained files/subdirectories
  // this is called recursively, and is meant to be initiated from the root dir
  void Process(
    const FileAllocationTable32& fat,
    std::shared_ptr<Directory>   dir,
    const std::uint32_t          dirStartCluster)
  {
    assert(dir);
    // record the RFAT chain for the directory itself
    RecordChain(fat, dir->parent, dirStartCluster);
    // process all directory entries, recursing into directories
    for (std::size_t i{0}; i < dir->entryData.size(); ++i)
    {
      const auto& data{dir->entryData.at(i)};
      const auto subDir{dir->entryDir.at(i)};
      // stop if end record reached
      if (data.IsEnd()) break;
      // ignore if not a regular file or subdirectory
      const bool isSubdir{data.IsSubdirectory()};
      if (!isSubdir && !data.IsFile()) continue;
      // get start cluster
      const auto startCluster{data.GetStartCluster()};
      if (isSubdir)
      {
        // process subdirectory via recursion
        assert(subDir);
        // std::cout << "Recording RFAT chain for subdirectory '" << dirData.GetFilename() << "': ";
        Process(fat, subDir, startCluster);
      }
      else
      {
        // process file
        // std::cout << "Recording RFAT chain for regular file '" << dirData.GetFilename() << "': ";
        RecordChain(fat, dir, startCluster);
      }
    }
  }

  // (re)initialize reverse-FAT from given FAT, root directory, and root
  //  directory start cluster
  void Get(
    const FileAllocationTable32& fat,
    std::shared_ptr<Directory>   rootDir,
    std::uint32_t                rootDirStartCluster)
  {
    // (re)allocate entry vector with size matching standard FAT, and all
    //  elements marked as free clusters
    entries = {fat.entries.size(), FileAllocationTable32::FAT32::FREE};

    // walk the directory tree to initialize the table
    // std::cout << "Recording RFAT chain for root directory: ";
    Process(fat, rootDir, rootDirStartCluster);
  }

  // instantiate a reverse-FAT table from a given FAT and root directory
  explicit Rfat(
    const FileAllocationTable32& fat,
    std::shared_ptr<Directory>   rootDir,
    std::uint32_t                rootDirStartCluster)
  {
    Get(fat, rootDir, rootDirStartCluster);
  }

  // walk the given cluster index back to the file's containing directory and
  //  start cluster
  std::pair<std::shared_ptr<Directory>, std::uint32_t> GetFileInfo(
    const std::uint32_t cluster) const
  {
    if (cluster < 2 || cluster >= entries.size()) return {};
    // grab by copy
    auto entry{entries.at(cluster)};
    // track potential file start cluster, starting with initial cluster in case
    //  it ends up being the one
    auto startCluster{cluster};
    while (true)
    {
      if (std::holds_alternative<std::shared_ptr<Directory>>(entry))
      {
        // entry contains a directory pointer - return it
        return {std::get<std::shared_ptr<Directory>>(entry), startCluster};
      }
      // entry contains next cluster up the chain, or is free
      const auto upCluster{std::get<std::uint32_t>(entry)};
      if (!FileAllocationTable32::IsCluster(upCluster))
      {
        // not a valid cluster number - abort
        return {};
      }
      // grab by copy
      entry = entries.at(upCluster);
      // record potential start cluster
      startCluster = upCluster;
    }
  }

  static bool IsCluster(const Entry& entry)
  {
    return std::holds_alternative<std::uint32_t>(entry)
        && FileAllocationTable32::IsCluster(std::get<std::uint32_t>(entry));
  }

  static bool IsCluster(const std::uint32_t data)
  {
    return FileAllocationTable32::IsCluster(data);
  }

  static std::string ToString(const Entry& entry)
  {
    if (std::holds_alternative<std::shared_ptr<Directory>>(entry))
    {
      return "DIR_PTR";
    }

    if (std::holds_alternative<std::uint32_t>(entry))
    {
      const auto data{std::get<std::uint32_t>(entry)};
      std::stringstream s;
      s << "0x" << std::hex << data << " (" << std::dec << data << ')';
      return s.str();
    }

    return "UNKNOWN";
  }
};

// update the start cluster of a directory entry with the given index:
// - file/subdir: changes start cluster in parent directory's data
// - subdir only: also changes own "." entry, plus all sub-subdirs' ".." entries
bool updateDirEntry(
  std::shared_ptr<Directory> parentDir,
  ByteStream& stream,
  const DiskInfo& diskInfo,
  const FileAllocationTable32& fat,
  const std::size_t entryIndex,
  const std::uint32_t cluster,
  std::shared_ptr<Directory> meDir
)
{
  // std::cout << "***** Changing start cluster in parent data from " << std::dec << (parentDir ? parentDir->entryData.at(entryIndex).GetStartCluster() : static_cast<std::uint32_t>(-1)) << " to " << cluster << '\n';
  // update entry in parent directory to point at new start cluster
  if (!parentDir || !parentDir->ChangeStartClusterAt(
    stream, diskInfo, fat, entryIndex, cluster))
  {
    return false;
  }

  // if not moving initial cluster of a subdirectory, we're done
  if (!meDir)
  {
    return true;
  }

  // update directory entries affected by subdirectory start cluster move
  for (std::size_t i{0}; i < meDir->entryData.size(); ++i)
  {
    const auto& subdirEntry{meDir->entryData.at(i)};
    // stop if end record reached
    if (subdirEntry.IsEnd()) break;
    // ignore non-directory records
    if (!subdirEntry.IsDirectory()) continue;
    // update own "." entry to point at destCluster
    if ("." == subdirEntry.GetFilename())
    {
      // std::cout << "***** Changing '.' start cluster from " << std::dec << subdirEntry.GetStartCluster() << " to " << cluster << '\n';
      if (!meDir->ChangeStartClusterAt(stream, diskInfo, fat, i, cluster))
      {
        return false;
      }
      continue;
    }

    // only care about updating proper subdirectories from here onwards
    if (!subdirEntry.IsSubdirectory())
    {
      continue;
    }

    // also need corresponding Directory object
    auto subdirPtr{meDir->entryDir.at(i)};
    if (!subdirPtr)
    {
      std::cout << "WARNING: Subdirectory '" << subdirEntry.GetFilename() << "' has null Directory pointer\n";
      continue;
    }

    // update subdirectory's ".." entry to point at destCluster
    for (std::size_t j{0}; j < subdirPtr->entryData.size(); ++j)
    {
      const auto& ssdirEntry{subdirPtr->entryData.at(j)};
      // stop if end record found
      if (ssdirEntry.IsEnd())
      {
        std::cout << "WARNING: Subdirectory '" << ssdirEntry.GetFilename() << "' of subdirectory '" << subdirEntry.GetFilename() << "' has no '..' entry\n";
      }
      // ignore non-directory entries
      if (!ssdirEntry.IsDirectory()) continue;
      // ignore non-".." entries
      if (".." != ssdirEntry.GetFilename()) continue;
      // std::cout << "***** Changing '..' start cluster from " << std::dec << ssdirEntry.GetStartCluster() << " to " << cluster << '\n';
      if (!subdirPtr->ChangeStartClusterAt(
        stream, diskInfo, fat, j, cluster))
      {
        return false;
      }
      // optimization: there should only be one ".." entry, so break here
      break;
    }
  }

  return true;
}

// move whatever is occupying the given source cluster to the given destination
//  cluster, and update FAT, RFAT, directory tree, etc. as needed
// TODO: support moving non-initial clusters in a root directory chain
bool moveCluster(
  ByteStream& stream,
  const std::uint32_t srcCluster,
  const std::uint32_t destCluster,
  const DiskInfo& diskInfo,
  std::vector<FileAllocationTable32>& fat,
  Rfat& rfat)
{
  // perform a bunch of sanity checks, and collect some basic data
  if (fat.empty())
  {
    std::cout << "ERROR: Empty FAT list\n";
    return false;
  }
  auto& fat0{fat.at(0)};
  if (fat0.entries.empty())
  {
    std::cout << "ERROR: FAT0 has no entries\n";
    return false;
  }
  const auto fatSize{fat0.entries.size()};
  for (const auto& fatI : fat)
  {
    if (fatI.entries.size() != fatSize)
    {
      std::cout << "ERROR: Found FAT with " << std::dec << fatI.entries.size() << " entries but expected " << fatSize << '\n';
      return false;
    }
  }
  if (rfat.entries.size() != fatSize)
  {
    std::cout << "ERROR: RFAT has " << std::dec << rfat.entries.size() << " entries but expected " << fatSize << '\n';
    return false;
  }
  if (srcCluster == destCluster)
  {
    std::cout << "ERROR: Not moving cluster " << std::dec << srcCluster << " to itself\n";
    return false;
  }
  if (srcCluster <= 2 && srcCluster >= fat0.entries.size())
  {
    std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is outside of legal range\n";
    return false;
  }
  if (destCluster <= 2 && destCluster >= fat0.entries.size())
  {
    std::cout << "ERROR: Destination cluster " << std::dec << destCluster << " is outside of legal range\n";
    return false;
  }
  if (!fat0.IsClusterAt(srcCluster) && !fat0.IsEndAt(srcCluster))
  {
    std::cout << "ERROR: Not moving non-data/non-end source cluster " << std::dec << srcCluster << '\n';
    return false;
  }
  if (!fat0.IsFreeAt(destCluster))
  {
    std::cout << "ERROR: Not moving to non-free destination cluster " << std::dec << destCluster << '\n';
    return false;
  }

  // query RFAT for containing directory and start cluster of srcCluster's FAT chain
  const auto [srcOwnerDir, srcStartCluster]{rfat.GetFileInfo(srcCluster)};
  if (!srcOwnerDir)
  {
    // TODO: root directory won't have a containing directory, but could span
    //  multiple clusters
    std::cout << "ERROR: No containing directory for file spanning source cluster " << std::dec << srcCluster << " with possible start cluster " << srcStartCluster << '\n';
    return false;
  }
  // std::cout << "\t\tSource cluster " << std::dec << srcCluster << " belongs to chain starting with cluster " << srcStartCluster << '\n';

  // search the containing dir for using file's entry
  const auto srcEntryIndex{srcOwnerDir->Find(srcStartCluster)};
  if (Directory::npos == srcEntryIndex)
  {
    std::cout << "ERROR: Containing directory can't locate file spanning source cluster " << std::dec << srcCluster << " with possible start cluster " << srcStartCluster << '\n';
    return false;
  }
  const auto& srcEntry{srcOwnerDir->entryData.at(srcEntryIndex)};
  // get type of file using srcCluster
  const bool isFile{srcEntry.IsFile()};
  const bool isSubdir{srcEntry.IsSubdirectory()};
  if (!isFile && !isSubdir)
  {
    std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is in use by something other than a file or directory\n";
    std::cout << "       Listing: ";
    srcEntry.Print(fat0);
    std::cout << "       filename='" << srcEntry.GetFilename()
              << "', IsDevice=" << std::dec << srcEntry.IsDevice()
              << ", IsDirectory=" << srcEntry.IsDirectory()
              << ", IsDotEntry=" << srcEntry.IsDotEntry()
              << ", IsEnd=" << srcEntry.IsEnd()
              << ", IsEndLFN=" << srcEntry.IsEndLFN()
              << ", IsErased=" << srcEntry.IsErased()
              << ", IsErasedLFN=" << srcEntry.IsErasedLFN()
              << ", IsFile=" << srcEntry.IsFile()
              << ", IsLongFilenameData=" << srcEntry.IsLongFilenameData()
              << ", IsSubdirectory=" << srcEntry.IsSubdirectory()
              << ", IsVolumeLabel=" << srcEntry.IsVolumeLabel()
              << '\n';
    return false;
  }
  const auto& srcDir{srcOwnerDir->entryDir.at(srcEntryIndex)};
  if (isSubdir && !srcDir)
  {
    std::cout << "ERROR: Directory spanning source cluster " << srcCluster << " has null Directory pointer\n";
    return false;
  }

  // get (R)FAT chain info for srcCluster
  // ...and do a ton of sanity checking to ensure everything is in a good state
  //  actual FAT data for srcCluster
  const auto srcFatData{fat0.entries.at(srcCluster)};
  //  actual RFAT data for srcCluster
  const auto srcRfatData{rfat.entries.at(srcCluster)};
  //  whether srcCluster is the start of a FAT chain
  const bool isChainStart{srcCluster == srcStartCluster};
  //  sanity check that RFAT data is appropriate
  if (isChainStart && !std::holds_alternative<std::shared_ptr<Directory>>(srcRfatData))
  {
    std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is the start of its FAT chain, but RFAT does not hold a directory pointer for it\n";
    return false;
  }
  if (!isChainStart && !std::holds_alternative<std::uint32_t>(srcRfatData))
  {
    std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is not the start of its FAT chain, but RFAT does not hold cluster data for it\n";
    return false;
  }
  //  whether srcCluster is the end of a FAT chain
  const bool isChainEnd{FileAllocationTable32::IsEnd(srcFatData)};
  //  previous cluster in FAT chain, or 0 if none
  const auto prevCluster{
    isChainStart ? 0 : std::get<std::uint32_t>(srcRfatData)};
  //  sanity check that non-start cluster has a valid previous cluster
  if (!isChainStart && !FileAllocationTable32::IsCluster(prevCluster))
  {
    std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is not the start of its FAT chain, but previous cluster in chain has illegal value: 0x" << std::hex << prevCluster << " (" << std::dec << prevCluster << ")\n";
    return false;
  }
  //  sanity check that FAT[prevCluster]=srcCluster if appropriate
  if (!isChainStart && srcCluster != fat0.entries.at(prevCluster))
  {
    std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is not the start of its FAT chain, but RFAT-reported previous cluster in chain " << prevCluster << " does not point at it\n";
    return false;
  }
  //  next cluster in FAT chain, or 0 if none
  const auto nextCluster{isChainEnd ? 0 : srcFatData};
  if (!isChainEnd)
  {
    // sanity check that non-end cluster has a valid next cluster
    if (!FileAllocationTable32::IsCluster(nextCluster))
    {
      std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is not the end of its FAT chain, but next cluster in chain has illegal FAT value: 0x" << std::hex << nextCluster << " (" << std::dec << nextCluster << ")\n";
      return false;
    }
    // sanity check that RFAT[nextCluster]=srcCluster if appropriate
    const auto& ncRfatData{rfat.entries.at(nextCluster)};
    if (!Rfat::IsCluster(ncRfatData) || srcCluster != std::get<std::uint32_t>(ncRfatData))
    {
      std::cout << "ERROR: Source cluster " << std::dec << srcCluster << " is not the end of its FAT chain, but next cluster in chain has unexpected RFAT data: " << Rfat::ToString(ncRfatData) << '\n';
      return false;
    }
  }
  // std::cout << "\t\tsrcFatData=0x" << std::hex << srcFatData << ", srcRfatData=" << Rfat::ToString(srcRfatData) << ", isChainStart=" << std::dec << isChainStart << ", isChainEnd=" << isChainEnd << ", prevCluster=" << prevCluster << ", nextCluster=" << nextCluster << '\n';

  // if first cluster of a file/directory, update related directory data
  if (isChainStart)
  {
    // std::cout << "\t\tUpdating directory data for moved file " << srcOwnerDir->entries.at(srcEntryIndex).first.GetFilename() << '\n';
    if (!updateDirEntry(
      srcOwnerDir, stream, diskInfo, fat0, srcEntryIndex, destCluster, srcDir))
    {
      std::cout << "WARNING: Failed to update directory data for moved file " << srcEntry.GetFilename() << '\n';
      return false;
    }
  }

  // copy data contents of srcCluster to destCluster on disk
  // this is done after copy in the very likely case that it modifies the
  //  contents of srcCluster itself
  // std::cout << "\t\tCopying contents of source cluster " << std::dec << srcCluster << " to destination cluster " << destCluster << '\n';
  if (!diskInfo.CopyCluster(stream, srcCluster, destCluster))
  {
    return false;
  }

  // update FATs
  for (auto& fatI : fat)
  {
    // copy srcCluster FAT data to destCluster's index
    // std::cout << "\t\tWriting FAT[" << std::dec << destCluster << "]=0x" << std::hex << srcFatData << " (" << std::dec << srcFatData << ")\n";
    if (!fatI.Write(stream, destCluster, srcFatData))
    {
      return false;
    }
    // mark srcCluster FAT entry as free
    // std::cout << "\t\tFreeing FAT[" << std::dec << srcCluster << "]=0x" << std::hex << FileAllocationTable32::FREE << " (" << std::dec << FileAllocationTable32::FREE << ")\n";
    if (!fatI.Write(stream, srcCluster, FileAllocationTable32::FREE))
    {
      return false;
    }
    // if previous FAT chain entry exists, point at new cluster
    if (!isChainStart)
    {
      // std::cout << "\t\tWriting previous FAT[" << std::dec << prevCluster << "]=0x" << std::hex << destCluster << " (" << std::dec << destCluster << ")\n";
      if (!fatI.Write(stream, prevCluster, destCluster))
      {
        return false;
      }
    }
  }
  // FATs update end

  stream.sync();

  // update RFAT
  //  copy srcCluster RFAT data to destCluster's index
  rfat.entries.at(destCluster) = srcRfatData;
  //  mark srcCluster RFAT entry as free
  rfat.entries.at(srcCluster) = FileAllocationTable32::FREE;
  //  if next RFAT chain entry exists, point at new cluster
  if (!isChainEnd)
  {
    rfat.entries.at(nextCluster) = destCluster;
  }
  // RFAT update end

  return true;
}

using DirList = std::vector<std::shared_ptr<Directory>>;

// build a breadth-first list of all directories in the given tree
std::pair<DirList, std::uint32_t> getDirList(
  std::shared_ptr<Directory> rootDir,
  const DiskInfo& diskInfo,
  const FileAllocationTable32& fat)
{
  DirList dirList{rootDir};
  std::cout << "Building directory list...\n";
  // ...and also count total clusters used by directories
  auto dirClusters{static_cast<std::uint32_t>(
    fat.GetClusterChain(diskInfo.rootDirStartCluster).size())};
  std::cout << "\tAdded root directory with start cluster " << std::dec << diskInfo.rootDirStartCluster << " and length of " << dirClusters << " cluster(s)\n";
  // NOTE: Neither range loop nor iteration works here, because they don't
  //  support traversing a container that grows on the fly
  for (std::size_t dirListIndex{0}; dirListIndex < dirList.size(); ++dirListIndex)
  {
    // std::cout << "#" << std::dec << dirListIndex+1 << '/' << dirList.size() << '\n';
    // grab by copy, because a reference will likely get invalidated as dirList
    //  is modified
    const auto dirPtr{dirList.at(dirListIndex)};
    if (!dirPtr)
    {
      std::cout << "ERROR: dirList[" << std::dec << dirListIndex << "] is null\n";
      return {};
    }
    // const auto dirStartCluster{dlIter->second};
    // put all of this directory's subdirs on the queue
    // ...and also count and add its cluster length
    const auto& entryData{dirPtr->entryData};
    const auto& entryDir{dirPtr->entryDir};
    for (std::size_t entryIndex{0}; entryIndex < entryData.size(); ++entryIndex)
    {
      const auto& subdirData{entryData.at(entryIndex)};
      // std::cout << "\t#" << std::dec << entryIndex+1 << '/' << entryData.size() << ": ";
      // subdirData.Print(fat0);
      if (subdirData.IsEnd())
      {
        // std::cout << "\t\tBreaking on end record\n";
        break;
      }
      // only process Directory entries
      if (const auto& subdirPtr{entryDir.at(entryIndex)}; subdirPtr)
      {
        const auto subdirStartCluster{subdirData.GetStartCluster()};
        const auto subdirNumClusters{static_cast<std::uint32_t>(
          fat.GetClusterChain(subdirStartCluster).size())};
        std::cout << "\tAdding directory '" << dirPtr->GetFileNameAt(entryIndex) << "' with start cluster " << std::dec << subdirStartCluster << " and length of " << subdirNumClusters << " cluster(s)\n";
        dirList.emplace_back(subdirPtr);
        dirClusters += subdirNumClusters;
      }
      // std::cout << "\tEND Entry\n";
    }
    // std::cout << "END\n";
  }
  std::cout << "Counted " << std::dec << dirList.size() << " directories total, spanning " << dirClusters << " total cluster(s)\n";
  return {std::move(dirList), dirClusters};
}

// compare two DirList's for equality of entry count and entry start clusters
bool compare(const DirList& lhs, const DirList& rhs)
{
  if (lhs.size() != rhs.size())
  {
    std::cout << "compare(DirList,DirList): lists have different sizes\n";
    return false;
  }

  for (std::size_t dlIndex{0}; dlIndex < lhs.size(); ++dlIndex)
  {
    const auto& dirL{*(lhs.at(dlIndex))};
    const auto& dirR{*(rhs.at(dlIndex))};
    if (dirL.entryData.size() != dirR.entryData.size())
    {
      std::cout << "compare(DirList,DirList): directories at index " << std::dec << dlIndex << " have different entry list sizes\n";
      return false;
    }
    for (std::size_t entryIndex{0}; entryIndex < dirL.entryData.size(); ++entryIndex)
    {
      const auto& entryL{dirL.entryData.at(entryIndex)};
      const auto& entryR{dirR.entryData.at(entryIndex)};
      if (entryL.IsDevice() != entryR.IsDevice()
        || entryL.IsDirectory() != entryR.IsDirectory()
        || entryL.IsDotEntry() != entryR.IsDotEntry()
        || entryL.IsEnd() != entryR.IsEnd()
        || entryL.IsEndLFN() != entryR.IsEndLFN()
        || entryL.IsErased() != entryR.IsErased()
        || entryL.IsErasedLFN() != entryR.IsErasedLFN()
        || entryL.IsFile() != entryR.IsFile()
        || entryL.IsLongFilenameData() != entryR.IsLongFilenameData()
        || entryL.IsSubdirectory() != entryR.IsSubdirectory()
        || entryL.IsVolumeLabel() != entryR.IsVolumeLabel()
      )
      {
        std::cout << "compare(DirList,DirList): Entries at index " << std::dec << entryIndex << " of directories at index " << dlIndex << " have different characteristics\n";
        return false;
      }
      // break at end, because data after doesn't matter
      if (entryL.IsEnd()) break;
      // ignore LFN / erased entries for now
      if (entryL.IsLongFilenameData() && entryL.IsErased()) continue;
      if (entryL.GetFilename() != entryR.GetFilename())
      {
        std::cout << "compare(DirList,DirList): Entries at index " << std::dec << entryIndex << " of directories at index " << dlIndex << " have different filenames\n";
        return false;
      }
      if (entryL.GetStartCluster() != entryR.GetStartCluster())
      {
        std::cout << "compare(DirList,DirList): Entries at index " << std::dec << entryIndex << " of directories at index " << dlIndex << " have different start clusters\n";
        return false;
      }
    }
  }

  return true;
}

bool optimize(
  ByteStream& stream,
  const DiskInfo& diskInfo,
  const DirList& dirList,
  const std::uint32_t dirClusters,
  std::vector<FileAllocationTable32>& fat,
  Rfat& rfat
)
{
  // std::cout << "SANITY CHECK 2:";
  // for (const auto& dir : dirList)
  // {
  //   std::cout << " " << std::dec << dir->GetStartCluster();
  // }
  // std::cout << '\n';

  SignalHandler sh{};
  auto& fat0{fat.at(0)};
  // find the first free cluster after the optimal directory area
  // - first, find the first cluster after the optimal directory area, by
  //    counting good clusters for optimized directory use
  std::uint32_t freeCluster{diskInfo.rootDirStartCluster};
  for (std::uint32_t i{0}; i < dirClusters; ++i)
  {
    freeCluster = fat0.GetNextGoodCluster(freeCluster);
    if (!freeCluster)
    {
      std::cout << "ERROR: Failed to find sufficient good clusters for holding all directories\n";
      return false;
    }
  }
  // - now that freeCluster holds the first cluster index after the directory
  //    area, find the first free cluster from there
  freeCluster = fat0.GetFirstFreeCluster(freeCluster);
  if (!freeCluster)
  {
    std::cout << "ERROR: Failed to find a free cluster\n";
    return false;
  }
  std::cout << "First free cluster index after optimal directory area: " << freeCluster << " (value=0x" << std::hex << fat0.entries.at(freeCluster) << ")\n";
  // now process the list for optimization
  // root directory is assumed to start at its ideal cluster, since we probably
  //  can't/shouldn't try to move its start cluster
  // we do need to check its intermediate clusters though
  std::uint32_t idealCluster{diskInfo.rootDirStartCluster};
  for (const auto& dirPtr : dirList)
  {
    if (SignalHandler::gotSignal())
    {
      std::cout << "ERROR: Caught signal\n";
      return false;
    }
    if (!dirPtr)
    {
      std::cout << "ERROR: Optimization list entry is null\n";
      return false;
    }

    const bool isRoot{dirList.front() == dirPtr};
    // const std::string& dirName
    // {
    //   isRoot ?
    //     "<ROOT_DIR>" :
    //     dirPtr->parent->entryData.at(
    //       dirPtr->parent->Find(dirPtr->GetStartCluster())).GetFilename()
    // };
    // std::cout << "Optimizing directory '" << dirName << "' with start cluster " << std::dec << dirPtr->GetStartCluster() << '\n';

    // get directory's current start cluster, assuming
    //  diskInfo.rootDirStartCluster if it's the first entry, since it doesn't
    //  have a "." entry
    const auto startCluster{isRoot ?
      diskInfo.rootDirStartCluster : dirPtr->GetStartCluster()};
    if (!startCluster)
    {
      std::cout << "ERROR: Failed to get directory start cluster\n";
      return false;
    }

    // check whether any clusters in current dir's chain need to be moved
    const auto& curChain{fat0.GetClusterChain(startCluster)};
    const auto& idealChain{
      fat0.GetIdealClusterChain(idealCluster, curChain.size())};
    if (curChain.size() != idealChain.size())
    {
      std::cout << "ERROR: Current cluster chain length=" << std::dec << curChain.size() << " does not match ideal cluster chain length=" << idealChain.size() << "; aborting\n";
      for (const auto cc : curChain) std::cout << " " << cc;
      std::cout << '\n';
      for (const auto ic : idealChain) std::cout << " " << ic;
      std::cout << '\n';
      return false;
    }
    for (std::size_t i{0}; i < curChain.size(); ++i)
    {
      const auto srcCluster{curChain.at(i)};
      const auto destCluster{idealChain.at(i)};
      if (srcCluster == destCluster)
      {
        std::cout << "Cluster " << std::dec << srcCluster << " is already optimized\n";
        continue;
      }
      std::cout << "Cluster " << std::dec << srcCluster << " should be moved to " << destCluster << '\n';
      if (FileAllocationTable32::IsFree(fat0.entries.at(destCluster)))
      {
        std::cout << "\tDestination cluster " << std::dec << destCluster << " is already free\n";
      }
      else if (freeCluster)
      {
        std::cout << "\tMoving data from destination cluster " << std::dec << destCluster << " to free cluster " << freeCluster << '\n';
        if (!moveCluster(
          stream, destCluster, freeCluster, diskInfo, fat, rfat))
        {
          std::cout << "ERROR: Cluster move failed\n";
          return false;
        }

        // advance freeCluster
        freeCluster = fat0.GetFirstFreeCluster(freeCluster);
      }
      else
      {
        std::cout << "ERROR: No free cluster\n";
        return false;
      }

      // move dir cluster into ideal cluster
      std::cout << "\tMoving data from source cluster " << std::dec << srcCluster << " to destination cluster " << destCluster << '\n';
      if (!moveCluster(stream, srcCluster, destCluster, diskInfo, fat, rfat))
      {
        std::cout << "ERROR: Cluster move failed\n";
        return false;
      }
    }
    // set next entry's ideal start cluster to the first good cluster following
    //  the end of the current entry's ideal chain
    idealCluster = fat0.GetNextGoodCluster(idealChain.back());
    if (!idealCluster)
    {
      std::cout << "ERROR: Failed to find next ideal cluster\n";
      return false;
    }
  }

  std::cout << "Optimization successful!\n";
  return true;
}
}

int main(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::cout << "ERROR: Must specify a file or block device to operate on\n";
    return -1;
  }
  ByteStream stream(argv[1], std::ios::in|std::ios::out|std::ios::binary);
  if (!stream.is_open() || !stream.good())
  {
    std::cout << "ERROR: Failed to access " << argv[1] << " as target file/device\n";
    return -2;
  }
  DiskInfo diskInfo(stream);
  diskInfo.Print();
  auto fat{FileAllocationTable32::GetAll(diskInfo, stream)};
  if (fat.empty())
  {
    std::cout << "ERROR: No FAT table(s) found\n";
    return -3;
  }
  const auto& fat0{fat.at(0)};
  std::cout << "Cross-comparing secondary FAT tables...\n";
  for (std::size_t i{1}; i < fat.size(); ++i)
  {
    const auto& fatI{fat.at(i)};
    if (!fat0.Compare(fatI))
    {
      std::cout << "WARNING: FAT table mismatch; post-optimize verify will also likely fail\n";
    }
  }
  // process the directory tree, starting with the root directory
  // std::cout << "Root directory spans " << std::dec << fat.at(0).GetClusterChain(diskInfo.rootDirStartCluster).size() << " cluster(s):";
  // for (const auto cluster : fat.at(0).GetClusterChain(diskInfo.rootDirStartCluster))
  // {
  //   std::cout << std::dec << " " << cluster;
  // }
  // std::cout << '\n';
  auto rootDirectory{Directory::GetAll(stream, diskInfo, fat0)};
  // std::cout << "Root directory entries (" << rootDirectory->entries.size() << "):\n";
  // rootDirectory->Print(fat.at(0));
  // build reverse-FAT table
  Rfat rfat{fat0, rootDirectory, diskInfo.rootDirStartCluster};
  // now build a breadth-first list of all directories
  auto [dirList, dirClusters]{getDirList(rootDirectory, diskInfo, fat0)};
  if (dirList.empty())
  {
    std::cout << "Directory list is empty - either an error occurred, or there is nothing to optimize\n";
    return 0;
  }
  // std::cout << "SANITY CHECK 1:";
  // for (const auto& dir : dirList)
  // {
  //   std::cout << " " << std::dec << dir->GetStartCluster();
  // }
  // std::cout << '\n';

  std::cout << "\nType OPTIMIZE and press Enter if you want to proceed with optimization based on the above info: ";
  std::cout.flush();
  std::string userResponse{};
  std::getline(std::cin, userResponse);
  if ("OPTIMIZE" != userResponse)
  {
    std::cout << "\nUser response '" << userResponse << "' was not OPTIMIZE; aborting\n";
    return 0;
  }

  std::cout << "\n********************\n";
  optimize(stream, diskInfo, dirList, dirClusters, fat, rfat);
  std::cout << "********************\n\n";

  stream.sync();
  std::cout << "Verifying FATs...\n";
  const auto& postFats{FileAllocationTable32::GetAll(diskInfo, stream)};
  if (fat.size() == postFats.size())
  {
    for (std::size_t i{0}; i < fat.size(); ++i)
    {
      std::cout << " #" << std::dec << i << ":\n";
      fat.at(i).Compare(postFats.at(i));
    }
  }
  else
  {
    std::cout << "ERROR: Different number of FAT tables on disk versus in memory!\n";
  }
  std::cout << "Cross-comparing secondary FAT tables...\n";
  for (std::size_t i{1}; i < postFats.size(); ++i)
  {
    if (!postFats.at(0).Compare(postFats.at(i)))
    {
      std::cout << "WARNING: FAT table mismatch;\n";
    }
  }
  std::cout << "Verifying directory trees...\n";
  if (compare(dirList, getDirList(
    Directory::GetAll(stream, diskInfo, postFats.at(0)),
    diskInfo, postFats.at(0)).first))
  {
    std::cout << "Trees match!\n";
  }

  return 0;
}
