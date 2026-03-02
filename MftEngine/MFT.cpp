#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <iomanip>
#include <algorithm> // 用于 std::min

#pragma pack(push, 1)

// MFT 记录头部
typedef struct {
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
} MFT_FILE_RECORD_HEADER;

// 属性头部
typedef struct {
    DWORD AttributeType;
    DWORD Length;
    BYTE NonResident;
    BYTE NameLength;
    WORD NameOffset;
    WORD Flags;
    WORD AttributeID;
} ATTRIBUTE_HEADER;

// $FILE_NAME 属性体
typedef struct {
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
} FILE_NAME_ATTRIBUTE;

#pragma pack(pop)

// 导出给 C# 的精简结构体
struct FileInfo {
    long long ID;
    long long ParentID;
    long long Size;
    DWORD Attributes;
    wchar_t Name[128];
};

using namespace std;

// 解析函数
bool ParseMFTRecordToStruct(BYTE* buffer, long long recordIndex, FileInfo& outInfo) {
    MFT_FILE_RECORD_HEADER* pHeader = (MFT_FILE_RECORD_HEADER*)buffer;

    if (pHeader->Signature != 0x454C4946) return false; // "FILE"
    if ((pHeader->Flags & 0x01) == 0) return false;     // Not in use

    outInfo.ID = recordIndex;
    outInfo.Size = 0;
    outInfo.ParentID = 0;
    outInfo.Attributes = pHeader->Flags;
    outInfo.Name[0] = 0;

    bool hasName = false;
    bool isDirectory = (pHeader->Flags & 0x02) != 0;

    // 指针计算使用 ptrdiff_t 或 size_t 避免警告，但这里偏移量很小，int 够用
    BYTE* pAttributeCursor = buffer + pHeader->AttributeOffset;

    while (true) {
        // 安全检查：确保读头不越界
        if ((size_t)(pAttributeCursor - buffer) + sizeof(ATTRIBUTE_HEADER) > 1024) break;

        ATTRIBUTE_HEADER* pAttrHeader = (ATTRIBUTE_HEADER*)pAttributeCursor;
        if (pAttrHeader->AttributeType == 0xFFFFFFFF) break;
        if (pAttrHeader->Length == 0) break;

        // --- $FILE_NAME (0x30) ---
        if (pAttrHeader->AttributeType == 0x30) {
            // 安全检查：确保读内容不越界
            if ((size_t)(pAttributeCursor - buffer) + 20 + sizeof(WORD) > 1024) break;

            WORD contentOffset = *((WORD*)(pAttributeCursor + 20));
            FILE_NAME_ATTRIBUTE* pFn = (FILE_NAME_ATTRIBUTE*)(pAttributeCursor + contentOffset);

            // 【修复】wcslen 返回 size_t，强转为 int 消除警告
            int currentNameLen = (int)wcslen(outInfo.Name);

            if (!hasName || pFn->NameLength > currentNameLen) {
                outInfo.ParentID = pFn->ParentDirectory & 0x0000FFFFFFFFFFFF;

                // 【修复】min 比较需要类型一致，且 pFn->NameLength 是 BYTE(unsigned char)
                int nameLen = (int)pFn->NameLength;
                int copyLen = (nameLen < 127) ? nameLen : 127;

                wmemcpy(outInfo.Name, pFn->Name, copyLen);
                outInfo.Name[copyLen] = 0;

                hasName = true;
            }
        }

        // --- $DATA (0x80) ---
        if (pAttrHeader->AttributeType == 0x80 && !isDirectory) {
            if (pAttrHeader->NonResident == 0) {
                outInfo.Size = *((DWORD*)(pAttributeCursor + 16));
            }
            else {
                outInfo.Size = *((unsigned long long*)(pAttributeCursor + 48));
            }
        }

        pAttributeCursor += pAttrHeader->Length;
        // 安全检查
        if (pAttributeCursor - buffer >= 1024) break;
    }

    return hasName;
}

int main() {
    // 【修复】使用 GetTickCount64 避免溢出
    ULONGLONG startTime = GetTickCount64();

    HANDLE hVol = CreateFile(L"\\\\.\\C:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE) {
        cout << "错误: 无法打开 C 盘 (请以管理员身份运行)." << endl;
        return 1;
    }

    NTFS_VOLUME_DATA_BUFFER volumeData;
    DWORD bytesReturned;
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &volumeData, sizeof(volumeData), &bytesReturned, NULL)) {
        CloseHandle(hVol);
        return 1;
    }

    long long recordSize = volumeData.BytesPerFileRecordSegment;
    long long mftOffset = volumeData.MftStartLcn.QuadPart * volumeData.BytesPerCluster;
    long long totalMftSize = volumeData.MftValidDataLength.QuadPart;
    // 【修复】__int64 除法结果赋给 long long，安全
    long long totalRecords = totalMftSize / recordSize;

    cout << "MFT 偏移: " << mftOffset << endl;
    cout << "MFT 大小: " << totalMftSize / 1024 / 1024 << " MB" << endl;
    cout << "预估记录: " << totalRecords << endl;

    LARGE_INTEGER liOffset;
    liOffset.QuadPart = mftOffset;
    SetFilePointerEx(hVol, liOffset, NULL, FILE_BEGIN);

    const DWORD CHUNK_SIZE = 64 * 1024 * 1024;
    std::vector<BYTE> bigBuffer(CHUNK_SIZE);
    std::vector<FileInfo> allFiles;
    allFiles.reserve((size_t)totalRecords); // 【修复】reserve 接受 size_t

    long long bytesProcessed = 0;

    cout << "开始高速扫描..." << endl;

    while (bytesProcessed < totalMftSize) {
        DWORD bytesRead = 0;
        // 【修复】min 类型匹配：(long long) vs (long long) -> 强转为 DWORD
        DWORD bytesToRead = (DWORD)min((long long)CHUNK_SIZE, totalMftSize - bytesProcessed);

        if (!ReadFile(hVol, bigBuffer.data(), bytesToRead, &bytesRead, NULL)) break;
        if (bytesRead == 0) break;

        // 【修复】除法结果强转为 int，消除 "conversion from 'long long' to 'int'" 警告
        int recordsInChunk = (int)(bytesRead / recordSize);

        for (int i = 0; i < recordsInChunk; i++) {
            BYTE* pRecord = bigBuffer.data() + (i * recordSize);
            long long globalRecordIndex = (bytesProcessed / recordSize) + i;

            FileInfo info;
            if (ParseMFTRecordToStruct(pRecord, globalRecordIndex, info)) {
                allFiles.push_back(info);
            }
        }

        bytesProcessed += bytesRead;
        if (bytesProcessed % (CHUNK_SIZE * 2) == 0) {
            cout << "\r进度: " << (bytesProcessed * 100 / totalMftSize) << "%" << flush;
        }
    }

    ULONGLONG endTime = GetTickCount64();
    cout << "\n\n解析完成!" << endl;
    cout << "耗时: " << (endTime - startTime) / 1000.0 << " 秒" << endl;
    cout << "有效文件数: " << allFiles.size() << endl;

    // --- 导出部分 ---
    const char* dumpFileName = "mft_dump.bin";
    FILE* fp = nullptr;

    // 【修复】使用 fopen_s 替代 fopen
    if (fopen_s(&fp, dumpFileName, "wb") == 0 && fp != nullptr) {
        size_t written = fwrite(allFiles.data(), sizeof(FileInfo), allFiles.size(), fp);
        fclose(fp);
        cout << "[Export] 成功导出 " << written << " 个文件记录到 " << dumpFileName << endl;
    }
    else {
        cout << "[Error] 无法写入导出文件!" << endl;
    }

    CloseHandle(hVol);
    cout << "按回车退出...";
    cin.get();
    return 0;
}