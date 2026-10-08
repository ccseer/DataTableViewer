#include "csv_record_index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#endif

namespace dtv::parsers {

namespace {
std::atomic<uint64_t> s_fileCounter{0};
// Distinguishes this process run from an earlier one that recycled the same
// PID. Without it, leftovers from a crashed process name-clash with every one
// of this process's CREATE_NEW retries.
uint64_t processRunSalt()
{
    static const uint64_t salt = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return salt;
}
constexpr size_t kWriteBufferSize = 4096;
constexpr size_t kBlockReadSize = 2048;
#ifdef _WIN32
// Converts a UTF-16 Windows path to UTF-8 so callers and tests can compare
// or display it without depending on the ANSI code page.
std::string wideToUtf8(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                   static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        utf8.data(), size, nullptr, nullptr);
    return utf8;
}

bool createPrivateIndexFile(std::wstring &outDir, std::wstring &outPath, HANDLE &outHandle, std::string &error)
{
    wchar_t tempPath[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, tempPath);
    if (len == 0 || len >= MAX_PATH) {
        error = "Failed to get temp path";
        return false;
    }

    // Per-process private directory under the OS temp directory. A file in
    // the shared temp root is pre-creatable by another process; the private
    // directory plus CREATE_NEW removes both the collision and the hijack.
    const std::wstring suffix = L"dtv_idx_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                std::to_wstring(processRunSalt());
    outDir = std::wstring(tempPath) + suffix;
    if (!CreateDirectoryW(outDir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "Failed to create index directory: " + std::to_string(GetLastError());
        return false;
    }

    outHandle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5 && outHandle == INVALID_HANDLE_VALUE; ++attempt) {
        uint64_t counter = s_fileCounter.fetch_add(1);
        outPath = outDir + L"\\dtv_idx_" + std::to_wstring(counter) + L".bin";
        outHandle = CreateFileW(
            outPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, // Exclusive access
            nullptr,
            CREATE_NEW, // Fail if the name somehow exists
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
        if (outHandle == INVALID_HANDLE_VALUE &&
            GetLastError() != ERROR_FILE_EXISTS) {
            break; // Unrelated failure; report it below
        }
    }

    if (outHandle == INVALID_HANDLE_VALUE) {
        error = "Failed to create temporary index file: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}
#else
bool createPrivateIndexFile(std::string &outDir, std::string &outPath, FILE *&outHandle, std::string &error)
{
    const char *tempBase = getenv("TMPDIR");
    std::string root = tempBase && *tempBase ? tempBase : "/tmp";
    outDir = root + "/dtv_idx_" + std::to_string(getpid()) + "_" +
             std::to_string(processRunSalt());
    if (mkdir(outDir.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "Failed to create index directory";
        return false;
    }

    outHandle = nullptr;
    for (int attempt = 0; attempt < 5 && !outHandle; ++attempt) {
        uint64_t counter = s_fileCounter.fetch_add(1);
        outPath = outDir + "/dtv_idx_" + std::to_string(counter) + ".bin";
        int fd = open(outPath.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) {
            outHandle = fdopen(fd, "w+b");
            if (!outHandle) {
                close(fd);
                std::remove(outPath.c_str());
            }
        } else if (errno != EEXIST) {
            break;
        }
    }

    if (!outHandle) {
        error = "Failed to create temporary index file";
        return false;
    }
    return true;
}
#endif
} // namespace

struct CsvRecordIndex::Impl {
#ifdef _WIN32
    HANDLE fileHandle{INVALID_HANDLE_VALUE};
    std::wstring filePath;
    std::wstring dirPath;
#else
    FILE *fileHandle{nullptr};
    std::string filePath;
    std::string dirPath;
#endif

    std::vector<CsvRecordSpan> ramCache; // Caches first 65,536 spans
    std::vector<CsvRecordSpan> writeBuffer;

    // Sliding window cache for ordinals beyond 65,536
    std::vector<CsvRecordSpan> blockCache;
    int64_t blockStartOrdinal{-1};
    bool failed{false};

    ~Impl()
    {
        close();
    }

    void close()
    {
#ifdef _WIN32
        if (fileHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(fileHandle);
            fileHandle = INVALID_HANDLE_VALUE;
        }
        // Best-effort removal of the now-empty private directory. Fails
        // harmlessly while other index instances in this process still own
        // files inside it.
        if (!dirPath.empty()) {
            RemoveDirectoryW(dirPath.c_str());
            dirPath.clear();
        }
#else
        if (fileHandle) {
            fclose(fileHandle);
            fileHandle = nullptr;
            if (!filePath.empty()) {
                std::remove(filePath.c_str());
            }
        }
        if (!dirPath.empty()) {
            rmdir(dirPath.c_str());
            dirPath.clear();
        }
#endif
    }

    void invalidateCache()
    {
        blockStartOrdinal = -1;
        blockCache.clear();
    }

    bool flushBuffer(CsvIndexHeader &header, std::string &error)
    {
        if (writeBuffer.empty())
            return true;

#ifdef _WIN32
        if (fileHandle == INVALID_HANDLE_VALUE) {
            error = "Invalid index file handle";
            return false;
        }

        // Seek to end of data
        LARGE_INTEGER li;
        li.QuadPart = sizeof(CsvIndexHeader) + (header.dataRecordCount - writeBuffer.size()) * sizeof(CsvRecordSpan);
        if (!SetFilePointerEx(fileHandle, li, nullptr, FILE_BEGIN)) {
            error = "Failed to seek in index file";
            return false;
        }

        DWORD bytesToWrite = static_cast<DWORD>(writeBuffer.size() * sizeof(CsvRecordSpan));
        DWORD bytesWritten = 0;
        if (!WriteFile(fileHandle, writeBuffer.data(), bytesToWrite, &bytesWritten, nullptr) ||
            bytesWritten != bytesToWrite) {
            error = "Failed to write spans to index file";
            return false;
        }
#else
        if (!fileHandle) {
            error = "Invalid index file pointer";
            return false;
        }
        uint64_t offset = sizeof(CsvIndexHeader) + (header.dataRecordCount - writeBuffer.size()) * sizeof(CsvRecordSpan);
        if (fseeko(fileHandle, static_cast<off_t>(offset), SEEK_SET) != 0) {
            error = "Failed to seek in index file";
            return false;
        }
        size_t written = fwrite(writeBuffer.data(), sizeof(CsvRecordSpan), writeBuffer.size(), fileHandle);
        if (written != writeBuffer.size()) {
            error = "Failed to write spans to index file";
            return false;
        }
#endif
        writeBuffer.clear();
        return true;
    }

    bool writeHeader(const CsvIndexHeader &header, std::string &error)
    {
#ifdef _WIN32
        if (fileHandle == INVALID_HANDLE_VALUE) {
            error = "Invalid index file handle";
            return false;
        }
        LARGE_INTEGER li;
        li.QuadPart = 0;
        if (!SetFilePointerEx(fileHandle, li, nullptr, FILE_BEGIN)) {
            error = "Failed to seek to header";
            return false;
        }
        DWORD written = 0;
        if (!WriteFile(fileHandle, &header, sizeof(CsvIndexHeader), &written, nullptr) ||
            written != sizeof(CsvIndexHeader)) {
            error = "Failed to write index header";
            return false;
        }
#else
        if (!fileHandle) {
            error = "Invalid index file pointer";
            return false;
        }
        if (fseek(fileHandle, 0, SEEK_SET) != 0) {
            error = "Failed to seek to header";
            return false;
        }
        if (fwrite(&header, sizeof(CsvIndexHeader), 1, fileHandle) != 1) {
            error = "Failed to write index header";
            return false;
        }
#endif
        return true;
    }
};

CsvRecordIndex::CsvRecordIndex() : m_impl(std::make_unique<Impl>())
{
    std::memcpy(m_header.magic, "DTVCSV01", 8);
    m_header.headerSize = sizeof(CsvIndexHeader);
    m_header.version = 1;
}

CsvRecordIndex::~CsvRecordIndex() = default;

CsvRecordIndex::CsvRecordIndex(CsvRecordIndex &&other) noexcept
    : m_impl(std::move(other.m_impl)), m_header(other.m_header), m_error(std::move(other.m_error))
{
    other.m_impl = std::make_unique<Impl>();
    other.m_impl->failed = true;
    other.m_header = CsvIndexHeader{};
}

CsvRecordIndex &CsvRecordIndex::operator=(CsvRecordIndex &&other) noexcept
{
    if (this != &other) {
        m_impl = std::move(other.m_impl);
        m_header = other.m_header;
        m_error = std::move(other.m_error);
        other.m_impl = std::make_unique<Impl>();
        other.m_impl->failed = true;
        other.m_header = CsvIndexHeader{};
    }
    return *this;
}

bool CsvRecordIndex::init()
{
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    m_impl->close();
    m_impl->ramCache.clear();
    m_impl->writeBuffer.clear();
    m_impl->blockCache.clear();
    m_impl->blockStartOrdinal = -1;
    m_impl->failed = false;
    // Reset the whole header so metadata from a previously opened file can
    // never leak into the new index.
    m_header = CsvIndexHeader{};

    if (!createPrivateIndexFile(m_impl->dirPath, m_impl->filePath, m_impl->fileHandle, m_error)) {
        return false;
    }

    return m_impl->writeHeader(m_header, m_error);
}

bool CsvRecordIndex::appendSpan(uint64_t start, uint64_t end)
{
    if (!m_impl || m_impl->failed || start > end) {
        return false;
    }

    CsvRecordSpan span{start, end};

    // Cache the first 65,536 spans in RAM
    if (m_impl->ramCache.size() < kMaxCachedSpans) {
        m_impl->ramCache.push_back(span);
    }

    m_impl->writeBuffer.push_back(span);
    m_header.dataRecordCount++;

    if (m_impl->writeBuffer.size() >= kWriteBufferSize) {
        if (!m_impl->flushBuffer(m_header, m_error)) {
            m_impl->failed = true;
            return false;
        }
    }

    return true;
}

std::optional<CsvRecordSpan> CsvRecordIndex::readSpan(int64_t ordinal)
{
    if (!m_impl || m_impl->failed || ordinal < 0 || static_cast<uint64_t>(ordinal) >= m_header.dataRecordCount) {
        return std::nullopt;
    }

    // 1. Check RAM cache for first 65,536 spans
    if (static_cast<size_t>(ordinal) < m_impl->ramCache.size()) {
        return m_impl->ramCache[static_cast<size_t>(ordinal)];
    }

    // 2. Check sliding block cache
    if (m_impl->blockStartOrdinal >= 0 && ordinal >= m_impl->blockStartOrdinal &&
        static_cast<size_t>(ordinal - m_impl->blockStartOrdinal) < m_impl->blockCache.size()) {
        return m_impl->blockCache[static_cast<size_t>(ordinal - m_impl->blockStartOrdinal)];
    }

    // 3. Flush write buffer before reading from disk to ensure data is visible
    if (!m_impl->flushBuffer(m_header, m_error)) {
        m_impl->failed = true;
        return std::nullopt;
    }

    // 4. Read block from disk
    uint64_t fileOffset = sizeof(CsvIndexHeader) + static_cast<uint64_t>(ordinal) * sizeof(CsvRecordSpan);
    size_t countToRead = std::min<size_t>(kBlockReadSize, static_cast<size_t>(m_header.dataRecordCount - ordinal));
    // Drop the previous window first: a partially written block must never be
    // reachable through blockStartOrdinal, or later reads would serve zeros
    // instead of failing.
    m_impl->invalidateCache();
    m_impl->blockCache.resize(countToRead);

#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = fileOffset;
    if (!SetFilePointerEx(m_impl->fileHandle, li, nullptr, FILE_BEGIN)) {
        m_error = "Seek failed during span read";
        m_impl->failed = true;
        m_impl->invalidateCache();
        return std::nullopt;
    }
    DWORD bytesRead = 0;
    DWORD bytesToRead = static_cast<DWORD>(countToRead * sizeof(CsvRecordSpan));
    if (!ReadFile(m_impl->fileHandle, m_impl->blockCache.data(), bytesToRead, &bytesRead, nullptr) ||
        bytesRead != bytesToRead) {
        m_error = "ReadFile failed during span read";
        m_impl->failed = true;
        m_impl->invalidateCache();
        return std::nullopt;
    }
#else
    if (fseeko(m_impl->fileHandle, static_cast<off_t>(fileOffset), SEEK_SET) != 0) {
        m_error = "Seek failed during span read";
        m_impl->failed = true;
        m_impl->invalidateCache();
        return std::nullopt;
    }
    size_t read = fread(m_impl->blockCache.data(), sizeof(CsvRecordSpan), countToRead, m_impl->fileHandle);
    if (read != countToRead) {
        m_error = "fread failed during span read";
        m_impl->failed = true;
        m_impl->invalidateCache();
        return std::nullopt;
    }
#endif

    m_impl->blockStartOrdinal = ordinal;
    return m_impl->blockCache[0];
}

std::vector<CsvRecordSpan> CsvRecordIndex::readSpans(int64_t firstOrdinal, size_t count)
{
    std::vector<CsvRecordSpan> result;
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto span = readSpan(firstOrdinal + static_cast<int64_t>(i));
        if (!span)
            break;
        result.push_back(*span);
    }

    return result;
}

void CsvRecordIndex::setHeaderByteSpan(uint64_t start, uint64_t end)
{
    m_header.headerStart = start;
    m_header.headerEnd = end;
}

std::pair<uint64_t, uint64_t> CsvRecordIndex::headerByteSpan() const
{
    return {m_header.headerStart, m_header.headerEnd};
}

void CsvRecordIndex::setMetadata(uint32_t colCount, char delimiter, bool hasBom)
{
    m_header.columnCount = colCount;
    m_header.delimiter = delimiter;
    m_header.hasBom = hasBom ? 1 : 0;
}

bool CsvRecordIndex::markComplete()
{
    if (!m_impl || m_impl->failed) {
        return false;
    }
    m_header.isComplete = 1;
    if (!flush()) {
        // Roll the flag back together with the write failure. Leaving it set
        // would make every later query report EOF at N rows while every span
        // read fails, i.e. a dead index advertised as a finished one.
        m_header.isComplete = 0;
        return false;
    }
    return true;
}

std::string CsvRecordIndex::filePath() const
{
    if (!m_impl) {
        return {};
    }
#ifdef _WIN32
    return wideToUtf8(m_impl->filePath);
#else
    return m_impl->filePath;
#endif
}

bool CsvRecordIndex::isComplete() const
{
    return m_impl && m_header.isComplete != 0 && !m_impl->failed;
}

bool CsvRecordIndex::hasFailed() const
{
    return !m_impl || m_impl->failed;
}

bool CsvRecordIndex::flush()
{
    if (!m_impl || m_impl->failed) {
        return false;
    }
    if (!m_impl->flushBuffer(m_header, m_error)) {
        m_impl->failed = true;
        return false;
    }
    if (!m_impl->writeHeader(m_header, m_error)) {
        m_impl->failed = true;
        return false;
    }
    return true;
}

} // namespace dtv::parsers
