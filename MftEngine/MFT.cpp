#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#pragma pack(push, 1)
struct MFT_FILE_RECORD_HEADER {
    DWORD Signature;
    WORD UsaOffset;
    WORD UsaCount;
    unsigned long long Lsn;
    WORD SequenceNumber;
    WORD LinkCount;
    WORD AttributeOffset;
    WORD Flags;
    DWORD BytesInUse;
    DWORD BytesAllocated;
    unsigned long long BaseFileRecord;
    WORD NextAttributeNumber;
    WORD Padded;
    DWORD MFTRecordNumber;
};

struct ATTRIBUTE_HEADER {
    DWORD AttributeType;
    DWORD Length;
    BYTE NonResident;
    BYTE NameLength;
    WORD NameOffset;
    WORD Flags;
    WORD AttributeID;
};

struct FILE_NAME_ATTRIBUTE {
    unsigned long long ParentDirectory;
    unsigned long long CreationTime;
    unsigned long long ChangeTime;
    unsigned long long LastWriteTime;
    unsigned long long LastAccessTime;
    unsigned long long AllocatedSize;
    unsigned long long RealSize;
    DWORD FileAttributes;
    DWORD ReparseTag;
    BYTE NameLength;
    BYTE NameType;
    WCHAR Name[1];
};
#pragma pack(pop)

struct FileInfo {
    long long ID = 0;
    long long ParentID = 0;
    long long Size = 0;
    DWORD Attributes = 0;
    std::wstring Name;
};

struct DataRunExtent {
    long long Lcn = 0;
    long long ClusterCount = 0;
};

constexpr std::int32_t kMagic = 0x55AA55AA;
constexpr DWORD kFileSignature = 0x454C4946; // "FILE"

static std::wstring NormalizeDrive(std::wstring input) {
    if (input.empty()) {
        return L"";
    }

    if (input.size() >= 2 && input[1] == L':') {
        return std::wstring{ input[0], L':' };
    }

    return std::wstring{ input[0], L':' };
}

static bool ApplyUsaFixup(BYTE* record, DWORD recordSize, DWORD bytesPerSector) {
    if (recordSize < sizeof(MFT_FILE_RECORD_HEADER) || bytesPerSector < 2) {
        return false;
    }

    auto* header = reinterpret_cast<MFT_FILE_RECORD_HEADER*>(record);
    if (header->UsaCount == 0) {
        return false;
    }

    size_t usaBytes = static_cast<size_t>(header->UsaCount) * sizeof(WORD);
    if (header->UsaOffset + usaBytes > recordSize) {
        return false;
    }

    auto* usa = reinterpret_cast<WORD*>(record + header->UsaOffset);
    WORD sequence = usa[0];
    DWORD sectors = header->UsaCount - 1;

    for (DWORD i = 0; i < sectors; ++i) {
        size_t tailOffset = static_cast<size_t>(i + 1) * bytesPerSector - sizeof(WORD);
        if (tailOffset + sizeof(WORD) > recordSize) {
            return false;
        }

        auto* sectorTail = reinterpret_cast<WORD*>(record + tailOffset);
        if (*sectorTail != sequence) {
            return false;
        }
        *sectorTail = usa[i + 1];
    }

    return true;
}

static unsigned long long ReadLeUnsigned(const BYTE* p, int bytes) {
    unsigned long long value = 0;
    for (int i = 0; i < bytes; ++i) {
        value |= static_cast<unsigned long long>(p[i]) << (i * 8);
    }
    return value;
}

static long long ReadLeSigned(const BYTE* p, int bytes) {
    unsigned long long value = ReadLeUnsigned(p, bytes);
    if (bytes > 0 && bytes < 8 && (p[bytes - 1] & 0x80) != 0) {
        value |= (~0ULL) << (bytes * 8);
    }
    return static_cast<long long>(value);
}

static bool ReadExactAt(HANDLE hFile, long long offset, BYTE* buffer, DWORD bytesToRead) {
    LARGE_INTEGER li{};
    li.QuadPart = offset;
    if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN)) {
        return false;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer, bytesToRead, &bytesRead, nullptr)) {
        return false;
    }

    return bytesRead == bytesToRead;
}

static bool ParseMftDataRunsFromRecord(
    const BYTE* rawRecord,
    DWORD recordSize,
    DWORD bytesPerSector,
    std::vector<DataRunExtent>& outRuns) {
    if (rawRecord == nullptr || recordSize < sizeof(MFT_FILE_RECORD_HEADER)) {
        return false;
    }

    std::vector<BYTE> fixedRecord(recordSize);
    std::memcpy(fixedRecord.data(), rawRecord, recordSize);
    if (!ApplyUsaFixup(fixedRecord.data(), recordSize, bytesPerSector)) {
        return false;
    }

    const BYTE* record = fixedRecord.data();
    const auto* header = reinterpret_cast<const MFT_FILE_RECORD_HEADER*>(record);
    if (header->Signature != kFileSignature || header->AttributeOffset >= recordSize) {
        return false;
    }

    const BYTE* attrCursor = record + header->AttributeOffset;
    const BYTE* recordEnd = record + recordSize;
    bool foundAnyRuns = false;

    while (attrCursor + sizeof(ATTRIBUTE_HEADER) <= recordEnd) {
        const auto* attrHeader = reinterpret_cast<const ATTRIBUTE_HEADER*>(attrCursor);
        if (attrHeader->AttributeType == 0xFFFFFFFF || attrHeader->Length == 0) {
            break;
        }

        const BYTE* attrEnd = attrCursor + attrHeader->Length;
        if (attrEnd > recordEnd || attrEnd <= attrCursor) {
            break;
        }

        // Parse unnamed non-resident $DATA on MFT(0), which carries the MFT stream runs.
        if (attrHeader->AttributeType == 0x80 && attrHeader->NonResident != 0 && attrHeader->NameLength == 0) {
            if (attrCursor + 34 > attrEnd) {
                return false;
            }

            WORD mappingPairsOffset = *reinterpret_cast<const WORD*>(attrCursor + 32);
            if (mappingPairsOffset >= attrHeader->Length) {
                return false;
            }

            const BYTE* run = attrCursor + mappingPairsOffset;
            long long currentLcn = 0;

            while (run < attrEnd) {
                BYTE runHeader = *run++;
                if (runHeader == 0) {
                    break;
                }

                int lengthBytes = runHeader & 0x0F;
                int offsetBytes = (runHeader >> 4) & 0x0F;
                if (lengthBytes <= 0 || lengthBytes > 8 || offsetBytes > 8) {
                    return false;
                }
                if (run + lengthBytes + offsetBytes > attrEnd) {
                    return false;
                }

                unsigned long long runClusters = ReadLeUnsigned(run, lengthBytes);
                run += lengthBytes;

                if (offsetBytes == 0) {
                    // Sparse run is unexpected for MFT. Fall back to ioctl mode for safety.
                    return false;
                }

                long long lcnDelta = ReadLeSigned(run, offsetBytes);
                run += offsetBytes;
                currentLcn += lcnDelta;

                if (runClusters > 0) {
                    DataRunExtent extent{};
                    extent.Lcn = currentLcn;
                    extent.ClusterCount = static_cast<long long>(runClusters);
                    outRuns.push_back(extent);
                    foundAnyRuns = true;
                }
            }
        }

        attrCursor = attrEnd;
    }

    return foundAnyRuns;
}

static bool ParseMFTRecordToStruct(
    BYTE* buffer,
    DWORD recordSize,
    DWORD bytesPerSector,
    long long recordIndex,
    FileInfo& outInfo) {
    if (recordSize < sizeof(MFT_FILE_RECORD_HEADER)) {
        return false;
    }

    BYTE* parseBuffer = buffer;
    std::vector<BYTE> fixedRecord(recordSize);
    std::memcpy(fixedRecord.data(), buffer, recordSize);
    if (ApplyUsaFixup(fixedRecord.data(), recordSize, bytesPerSector)) {
        parseBuffer = fixedRecord.data();
    }

    auto* pHeader = reinterpret_cast<MFT_FILE_RECORD_HEADER*>(parseBuffer);
    if (pHeader->Signature != kFileSignature) {
        return false;
    }
    if ((pHeader->Flags & 0x01) == 0) {
        return false;
    }
    if (pHeader->AttributeOffset >= recordSize) {
        return false;
    }

    outInfo.ID = recordIndex;
    outInfo.Size = 0;
    outInfo.ParentID = 0;
    outInfo.Attributes = pHeader->Flags;
    outInfo.Name.clear();

    bool hasName = false;
    size_t bestNameLen = 0;
    BYTE bestNameType = 2;
    bool hasPrimaryData = false;
    const bool isDirectory = (pHeader->Flags & 0x02) != 0;

    BYTE* attrCursor = parseBuffer + pHeader->AttributeOffset;
    BYTE* recordEnd = parseBuffer + recordSize;

    while (attrCursor + sizeof(ATTRIBUTE_HEADER) <= recordEnd) {
        auto* attrHeader = reinterpret_cast<ATTRIBUTE_HEADER*>(attrCursor);
        if (attrHeader->AttributeType == 0xFFFFFFFF) {
            break;
        }
        if (attrHeader->Length == 0) {
            break;
        }

        BYTE* attrEnd = attrCursor + attrHeader->Length;
        if (attrEnd > recordEnd || attrEnd <= attrCursor) {
            break;
        }

        if (attrHeader->AttributeType == 0x30 && attrHeader->NonResident == 0) {
            if (attrCursor + 22 <= attrEnd) {
                WORD contentOffset = *reinterpret_cast<WORD*>(attrCursor + 20);
                BYTE* content = attrCursor + contentOffset;
                constexpr size_t kBaseFileNameAttrSize = offsetof(FILE_NAME_ATTRIBUTE, Name);

                if (content + kBaseFileNameAttrSize <= attrEnd) {
                    auto* fileNameAttr = reinterpret_cast<FILE_NAME_ATTRIBUTE*>(content);
                    size_t nameChars = fileNameAttr->NameLength;
                    BYTE* nameEnd = reinterpret_cast<BYTE*>(fileNameAttr->Name) + (nameChars * sizeof(WCHAR));
                    if (nameEnd <= attrEnd) {
                        long long candidateParent = static_cast<long long>(fileNameAttr->ParentDirectory & 0x0000FFFFFFFFFFFFULL);
                        bool shouldReplace = false;
                        if (!hasName || nameChars > bestNameLen) {
                            shouldReplace = true;
                        }
                        else if (hasName && nameChars == bestNameLen) {
                            // Keep behavior close to the original tool: pick non-DOS
                            // name when lengths are equal.
                            if (bestNameType == 2 && fileNameAttr->NameType != 2) {
                                shouldReplace = true;
                            }
                        }

                        if (shouldReplace) {
                            outInfo.ParentID = candidateParent;
                            outInfo.Name.assign(fileNameAttr->Name, fileNameAttr->Name + nameChars);
                            hasName = true;
                            bestNameLen = nameChars;
                            bestNameType = fileNameAttr->NameType;
                        }
                    }
                }
            }
        }

        if (attrHeader->AttributeType == 0x80 && !isDirectory) {
            // Only count the unnamed default stream. Named streams (ADS), such as
            // $BadClus:$Bad, can have very large logical sizes and should not be
            // treated as the file's primary size.
            if (attrHeader->NameLength == 0) {
                long long dataSize = 0;
                if (attrHeader->NonResident == 0) {
                    if (attrCursor + 20 <= attrEnd) {
                        dataSize = *reinterpret_cast<DWORD*>(attrCursor + 16);
                    }
                }
                else {
                    if (attrCursor + 56 <= attrEnd) {
                        dataSize = static_cast<long long>(*reinterpret_cast<unsigned long long*>(attrCursor + 48));
                    }
                }

                if (!hasPrimaryData || dataSize > outInfo.Size) {
                    outInfo.Size = dataSize;
                }
                hasPrimaryData = true;
            }
        }

        attrCursor = attrEnd;
    }

    return hasName;
}

static bool WriteOutput(const std::wstring& outputPath, const std::vector<FileInfo>& allFiles) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, outputPath.c_str(), L"wb") != 0 || fp == nullptr) {
        return false;
    }

    std::int32_t magic = kMagic;
    std::int32_t count = static_cast<std::int32_t>(
        std::min<size_t>(allFiles.size(), static_cast<size_t>(INT32_MAX)));

    bool ok = true;
    ok = ok && (fwrite(&magic, sizeof(magic), 1, fp) == 1);
    ok = ok && (fwrite(&count, sizeof(count), 1, fp) == 1);

    for (std::int32_t i = 0; ok && i < count; ++i) {
        const auto& item = allFiles[static_cast<size_t>(i)];
        std::int64_t id = item.ID;
        std::int64_t parentId = item.ParentID;
        std::int64_t size = item.Size;
        std::uint32_t attributes = item.Attributes;
        std::int32_t nameLen = static_cast<std::int32_t>(
            std::min<size_t>(item.Name.size(), static_cast<size_t>(INT32_MAX / 2)));

        ok = ok && (fwrite(&id, sizeof(id), 1, fp) == 1);
        ok = ok && (fwrite(&parentId, sizeof(parentId), 1, fp) == 1);
        ok = ok && (fwrite(&size, sizeof(size), 1, fp) == 1);
        ok = ok && (fwrite(&attributes, sizeof(attributes), 1, fp) == 1);
        ok = ok && (fwrite(&nameLen, sizeof(nameLen), 1, fp) == 1);
        if (nameLen > 0) {
            ok = ok && (fwrite(item.Name.data(), sizeof(wchar_t), static_cast<size_t>(nameLen), fp) ==
                static_cast<size_t>(nameLen));
        }
    }

    fclose(fp);
    return ok;
}

static bool ScanMftByDataRuns(
    HANDLE hVol,
    const NTFS_VOLUME_DATA_BUFFER& volumeData,
    std::vector<FileInfo>& allFiles) {
    const DWORD recordSize = volumeData.BytesPerFileRecordSegment;
    const DWORD bytesPerSector = volumeData.BytesPerSector;
    const long long bytesPerCluster = volumeData.BytesPerCluster;
    const long long totalMftSize = volumeData.MftValidDataLength.QuadPart;
    const long long mftOffset = volumeData.MftStartLcn.QuadPart * bytesPerCluster;

    if (recordSize == 0 || bytesPerSector == 0 || bytesPerCluster <= 0 || totalMftSize <= 0) {
        return false;
    }

    std::vector<BYTE> firstRecord(recordSize);
    if (!ReadExactAt(hVol, mftOffset, firstRecord.data(), recordSize)) {
        return false;
    }

    std::vector<DataRunExtent> runs;
    if (!ParseMftDataRunsFromRecord(firstRecord.data(), recordSize, bytesPerSector, runs) || runs.empty()) {
        return false;
    }

    constexpr DWORD CHUNK_SIZE = 64 * 1024 * 1024;
    std::vector<BYTE> chunkBuffer(CHUNK_SIZE);
    std::vector<BYTE> carryBuffer;
    carryBuffer.reserve(recordSize * 2);

    long long remainingLogicalBytes = totalMftSize;
    long long recordIndex = 0;

    for (const auto& run : runs) {
        if (remainingLogicalBytes <= 0) {
            break;
        }

        if (run.ClusterCount <= 0 || run.Lcn < 0) {
            return false;
        }

        long long runOffset = run.Lcn * bytesPerCluster;
        long long runBytes = run.ClusterCount * bytesPerCluster;
        long long runProcessed = 0;

        while (runProcessed < runBytes && remainingLogicalBytes > 0) {
            long long maxStep = std::min<long long>(runBytes - runProcessed, remainingLogicalBytes);
            DWORD toRead = static_cast<DWORD>(std::min<long long>(CHUNK_SIZE, maxStep));
            if (toRead == 0) {
                break;
            }

            if (!ReadExactAt(hVol, runOffset + runProcessed, chunkBuffer.data(), toRead)) {
                return false;
            }

            size_t oldCarrySize = carryBuffer.size();
            carryBuffer.resize(oldCarrySize + toRead);
            std::memcpy(carryBuffer.data() + oldCarrySize, chunkBuffer.data(), toRead);

            size_t completeRecords = carryBuffer.size() / recordSize;
            for (size_t i = 0; i < completeRecords; ++i) {
                BYTE* rec = carryBuffer.data() + (i * recordSize);
                FileInfo info;
                if (ParseMFTRecordToStruct(rec, recordSize, bytesPerSector, recordIndex, info)) {
                    allFiles.push_back(std::move(info));
                }
                ++recordIndex;
            }

            size_t consumedBytes = completeRecords * recordSize;
            size_t leftoverBytes = carryBuffer.size() - consumedBytes;
            if (leftoverBytes > 0) {
                std::memmove(carryBuffer.data(), carryBuffer.data() + consumedBytes, leftoverBytes);
            }
            carryBuffer.resize(leftoverBytes);

            runProcessed += toRead;
            remainingLogicalBytes -= toRead;
        }
    }

    return remainingLogicalBytes == 0;
}

static bool ScanMftByRecordIoctl(
    HANDLE hVol,
    const NTFS_VOLUME_DATA_BUFFER& volumeData,
    std::vector<FileInfo>& allFiles) {
    const DWORD recordSize = volumeData.BytesPerFileRecordSegment;
    const DWORD bytesPerSector = volumeData.BytesPerSector;
    const long long totalMftSize = volumeData.MftValidDataLength.QuadPart;
    const long long totalRecords = totalMftSize / recordSize;

    if (recordSize == 0 || bytesPerSector == 0 || totalMftSize <= 0) {
        return false;
    }

    DWORD frOutputBufferSize = static_cast<DWORD>(offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + recordSize);
    std::vector<BYTE> frOutputBuffer(frOutputBufferSize);
    NTFS_FILE_RECORD_INPUT_BUFFER frInput{};

    bool foundAnyRecord = false;
    for (long long frn = 0; frn < totalRecords; ++frn) {
        frInput.FileReferenceNumber.QuadPart = static_cast<ULONGLONG>(frn);

        DWORD bytesReturnedRecord = 0;
        BOOL ioctlOk = DeviceIoControl(
            hVol,
            FSCTL_GET_NTFS_FILE_RECORD,
            &frInput,
            sizeof(frInput),
            frOutputBuffer.data(),
            frOutputBufferSize,
            &bytesReturnedRecord,
            nullptr);

        if (!ioctlOk) {
            continue;
        }

        auto* frOutput = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(frOutputBuffer.data());
        if (frOutput->FileRecordLength < sizeof(MFT_FILE_RECORD_HEADER)) {
            continue;
        }

        foundAnyRecord = true;

        FileInfo info;
        long long recordNumber = static_cast<long long>(frOutput->FileReferenceNumber.QuadPart & 0x0000FFFFFFFFFFFFULL);
        DWORD parseRecordSize = std::min<DWORD>(recordSize, frOutput->FileRecordLength);
        if (ParseMFTRecordToStruct(frOutput->FileRecordBuffer, parseRecordSize, bytesPerSector, recordNumber, info)) {
            allFiles.push_back(std::move(info));
        }
    }

    return foundAnyRecord;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 3) {
        std::wcerr << L"Usage: MftEngine.exe <drive> <output_file>\n";
        return 2;
    }

    const std::wstring drive = NormalizeDrive(argv[1]);
    if (drive.size() != 2 || drive[1] != L':') {
        std::wcerr << L"[Error] Invalid drive: " << argv[1] << L"\n";
        return 2;
    }

    const std::wstring outputPath = argv[2];
    if (outputPath.empty()) {
        std::wcerr << L"[Error] Output path is empty.\n";
        return 2;
    }

    const std::wstring volumePath = L"\\\\.\\" + drive;
    HANDLE hVol = CreateFileW(
        volumePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hVol == INVALID_HANDLE_VALUE) {
        std::wcerr << L"[Error] Unable to open volume: " << drive << L" (run as Administrator)\n";
        return 1;
    }

    NTFS_VOLUME_DATA_BUFFER volumeData{};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
        hVol,
        FSCTL_GET_NTFS_VOLUME_DATA,
        nullptr,
        0,
        &volumeData,
        sizeof(volumeData),
        &bytesReturned,
        nullptr)) {
        CloseHandle(hVol);
        std::wcerr << L"[Error] FSCTL_GET_NTFS_VOLUME_DATA failed.\n";
        return 1;
    }

    const DWORD recordSize = volumeData.BytesPerFileRecordSegment;
    const DWORD bytesPerSector = volumeData.BytesPerSector;
    const long long totalMftSize = volumeData.MftValidDataLength.QuadPart;
    if (recordSize == 0 || bytesPerSector == 0 || totalMftSize <= 0) {
        CloseHandle(hVol);
        std::wcerr << L"[Error] Invalid NTFS metadata.\n";
        return 1;
    }

    std::vector<FileInfo> allFiles;
    allFiles.reserve(static_cast<size_t>(std::max<long long>(1, totalMftSize / recordSize)));

    bool usedFastPath = false;
    bool scanned = ScanMftByDataRuns(hVol, volumeData, allFiles);
    if (scanned) {
        usedFastPath = true;
    }
    if (!scanned) {
        allFiles.clear();
        scanned = ScanMftByRecordIoctl(hVol, volumeData, allFiles);
    }

    CloseHandle(hVol);

    if (!scanned) {
        std::wcerr << L"[Error] Failed to read MFT.\n";
        return 1;
    }

    if (!WriteOutput(outputPath, allFiles)) {
        std::wcerr << L"[Error] Failed to write output file: " << outputPath << L"\n";
        return 1;
    }

    if (usedFastPath) {
        std::wcout << L"[Mode] DataRuns\n";
    }
    else {
        std::wcout << L"[Mode] RecordIoctlFallback\n";
    }
    std::wcout << L"[OK] Exported " << allFiles.size() << L" records to " << outputPath << L"\n";
    return 0;
}
