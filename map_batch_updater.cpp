// by ficool2

#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#include <iostream>
#include <vector>
#include <string>

#include "steam/steam_api.h"

#include <Windows.h>
#include <wincon.h>
#include <direct.h>
#include <io.h>

#include "minizip/mz.h"
#include "minizip/mz_strm.h"
#include "minizip/mz_strm_mem.h"
#include "minizip/mz_strm_buf.h"
#include "minizip/mz_zip.h"
#include "minizip/mz_zip_rw.h"

const AppId_t g_AppID = 440;
const char* g_ConfigName = "config.ini";

HANDLE g_Console;
char g_MapTempPath[_MAX_PATH] = { 0 };

CSteamID g_UserSteamID;
AccountID_t g_UserAccountID;

ISteamUser* g_SteamUser;
ISteamFriends* g_SteamFriends;
ISteamUGC* g_SteamUGC;

struct OperationBase;
std::vector<PublishedFileId_t> g_IniMaps;
std::vector<std::string> g_IniLocalMaps;
std::vector<OperationBase*> g_IniOperations;
bool g_IniDownloadMaps = true;
bool g_IniLogOperations = true;
bool g_IniOperateMaps = true;
bool g_IniPrintPakFile = false;
bool g_IniCompressPakFile = true;
int g_IniCompressionLevel = 5;
bool g_IniWritePakFile = true;
bool g_IniUploadMaps = false;
char g_IniChangeNote[1024] = { 0 };

void FixSlashes(char* str)
{
    const char find = '\\';
    const char replace = '/';
    char* current_pos = strchr(str, find);
    while (current_pos)
    {
        *current_pos = replace;
        current_pos = strchr(current_pos, find);
    }
}

struct ZipFile
{
    void Init(const char* _filename, size_t _size)
    {
        SetFilename(_filename);
        buffer = new char[_size];
        size = _size;
    }

    void InitFromFile(FILE* file, const char* _filename)
    {
        SetFilename(_filename);

        fseek(file, 0, SEEK_END);
        size = (size_t)ftell(file);
        fseek(file, 0, SEEK_SET);
        buffer = new char[size];
        fread(buffer, 1, size, file);
    }

    void SetFilename(const char* _filename)
    {
        filename = strdup(_filename);
        FixSlashes(filename);
    }

    void Destroy()
    {
        free(filename);
        filename = nullptr;
        delete[] buffer;
        buffer = nullptr;
    }

    char* filename;
    char* buffer;
    size_t size;
};

typedef std::vector<ZipFile> ZipFileList;

struct ZipContainer
{
    ZipContainer()
    {
        stream_read = mz_zip_reader_create();
        stream_write = mz_zip_writer_create();
        stream_read_mem = mz_stream_mem_create();
        stream_write_mem = mz_stream_mem_create();
    }

    ~ZipContainer()
    {
        mz_zip_reader_delete(&stream_read);
        mz_zip_writer_delete(&stream_write);

        mz_stream_mem_delete(&stream_read_mem);
        mz_stream_mem_delete(&stream_write_mem);
    }

    void* stream_read;
    void* stream_write;
    void* stream_read_mem;
    void* stream_write_mem;
};

struct BSPLump
{
    int    offset;
    int    length;
    int    version;
    char   fourCC[4];
};

const int IDBSPHEADER = (('P' << 24) + ('S' << 16) + ('B' << 8) + 'V');

struct BSPHeader
{
    int ident;
    int version;
    BSPLump lumps[64];
    int map_revision;
};

enum ConsoleColors
{
    DEFAULT = 7,
    GREEN = 10,
    AQUA = 11,
    RED = 12,
    PURPLE = 13,
    YELLOW = 14,
    WHITE = 15
};

void ConsolePrintf(ConsoleColors color, const char* format, ...)
{
    SetConsoleTextAttribute(g_Console, color);
    va_list args;
    va_start(args, format); 
    vprintf(format, args);
    va_end(args);
    SetConsoleTextAttribute(g_Console, DEFAULT);
}

void ConsolePrintProgress(ConsoleColors color, size_t processed, size_t total)
{
    ConsolePrintf(color, "Progress: %llu/%llu (%2.0f%%)           \r", processed, total, total > 0 ? ((processed / (float)total) * 100.0) : 0.f);
}

void ConsoleWaitForKey()
{
    printf("Press any key to continue...\n");
    getchar();
}

bool SteamInit()
{
    ConsolePrintf(WHITE, "Initializing Steam API...\n");

    if (!SteamAPI_Init())
    {
        ConsolePrintf(RED, "Failed to initialized Steam API\n");
        return false;
    }

    g_SteamUser = SteamUser();
    g_SteamFriends = SteamFriends();
    g_SteamUGC = SteamUGC();

    if (!g_SteamUser->BLoggedOn())
    {
        SteamAPI_Shutdown();
        ConsolePrintf(RED, "Failed to connect Steam. Are you logged in?\n");
        return false;
    }

    g_UserSteamID = g_SteamUser->GetSteamID();
    g_UserAccountID = g_UserSteamID.GetAccountID();
    return true;
}

template <typename T>
bool IsElementInVector(const std::vector<T>& vec, const T& v)
{
    return std::find(vec.begin(), vec.end(), v) != vec.end();
}

template <typename T>
void EraseElement(std::vector<T>& vec, size_t idx)
{
    vec.erase(vec.begin() + idx);
}

typedef bool(*SleepFunc)();
void SleepUntilCondition(SleepFunc Func, uint32 Delay)
{
    MSG msg = { 0 };
    while (true)
    {
        SteamAPI_RunCallbacks();

        if (Func())
            break;

        Sleep(Delay);
    }
}

struct OperationBase
{
    OperationBase(const char* name, char* value)
    {
        m_Name = name;
        strcpy(m_Value, value);
    }

    virtual bool OperateZip(ZipFileList& file_list) { return true; }

    const char* m_Name;
    char m_Value[512];
};

struct OperationAdd : OperationBase
{
    OperationAdd(char* value) : OperationBase("ADD", value) {}

    virtual bool OperateZip(ZipFileList& file_list) override
    {
        char base_dir[_MAX_PATH];
        char relative_path[_MAX_PATH];
        char full_path[_MAX_PATH];

        bool double_slash = false;
        char* p = m_Value;
        while (*p)
        {
            if (*p == '/' && *(p + 1) == '/')
            {
                size_t len = p - m_Value + 1;
                strncpy(base_dir, m_Value, len);
                base_dir[len] = '\0';
                strcpy(relative_path, p + 2);

                // trailing slash
                if (relative_path[0])
                {
                    p = &relative_path[strlen(relative_path) - 1];
                    if (*p == '/')
                        *p = '\0';
                }

                double_slash = true;
                break;
            }

            p++;
        }

        if (!double_slash)
        {
            ConsolePrintf(RED, "\tNo double slash found in %s\n", m_Value);
            return false;
        }

        snprintf(full_path, sizeof(full_path), "%s%s", base_dir, relative_path);

        p = strrchr(relative_path, '/');
        if (!p || strchr(p, '.')) // file
        {
            if (!AddFile(full_path, base_dir, file_list))
                return false;
        }
        else
        {
            if (!RecurseDirectory(full_path, base_dir, file_list))
                return false;
        }

        return true;
    }

    bool AddFile(const char* full_path, const char* base_dir, ZipFileList& file_list)
    {
        if (g_IniLogOperations)
            ConsolePrintf(WHITE, "\tAdding file %s\n", full_path);

        bool valid_file = false;

        FILE* file = fopen(full_path, "rb");
        if (!file)
        {
            ConsolePrintf(RED, "\tFailed to read file. Missing on disk?\n");
            return false;
        }

        const char* file_name = &full_path[strlen(base_dir)];

        int idx = -1;
        for (size_t i = 0; i < file_list.size(); i++)
        {
            if (!strcmp(file_list[i].filename, file_name))
            {
                idx = (int)i;
                break;
            }
        }

        if (idx >= 0)
        {
            ZipFile& zip_file = file_list[idx];
            zip_file.Destroy();
            zip_file.InitFromFile(file, file_name);
        }
        else
        {
            file_list.emplace_back();
            ZipFile& zip_file = file_list.back();
            zip_file.InitFromFile(file, file_name);
        }

        fclose(file);
        return true;
    }

    bool RecurseDirectory(const char* start_path, const char* base_dir, ZipFileList& file_list)
    {
        char path[_MAX_PATH];
        WIN32_FIND_DATA find_data;

        sprintf(path, "%s/*", start_path);

        HANDLE find = FindFirstFile(path, &find_data);
        if (find == INVALID_HANDLE_VALUE)
        {
            ConsolePrintf(RED, "\tFailed to recurse directory %s (error: %d)\n", start_path, GetLastError());
            return false;
        }

        do 
        {
            if (find_data.cFileName[0] != '.')
            {
                sprintf(path, "%s/%s", start_path, find_data.cFileName);

                if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    if (!RecurseDirectory(path, base_dir, file_list))
                        return false;
                }
                else 
                {
                    if (!AddFile(path, base_dir, file_list))
                        return false;
                }
            }
        } 
        while (FindNextFile(find, &find_data) != 0);

        FindClose(find);
        return true;
    }
};

struct OperationRemove : OperationBase
{
    OperationRemove(char* value) : OperationBase("REMOVE", value) {}
    virtual bool OperateZip(ZipFileList& file_list) override
    {
        size_t len = strlen(m_Value);

        int idx = -1;
        for (int i = (int)file_list.size() - 1; i >= 0; i--)
        {
            ZipFile& zip_file = file_list[i];
            if (!strncmp(zip_file.filename, m_Value, len))
            {
                if (g_IniLogOperations)
                    ConsolePrintf(WHITE, "\tRemoving file %s\n", zip_file.filename);

                zip_file.Destroy();
                EraseElement(file_list, i);
            }
        }

        return true;
    }
};

bool OperateZip(ZipFileList& file_list)
{
    for (OperationBase* operation : g_IniOperations)
    {
        if (g_IniLogOperations)
            ConsolePrintf(AQUA, "Running operation %s...\n", operation->m_Name);

        if (!operation->OperateZip(file_list))
        {
            ConsolePrintf(RED, "Failed to run all operations\n");
            return false;
        }
    }

    return true;
}

struct UGCWrapper
{
    UGCWrapper() : m_DownloadCallback(NULL, NULL) {}

    void CallbackQuery(SteamUGCQueryCompleted_t* result, bool error)
    {
        if (error || result->m_eResult != k_EResultOK)
        {
            ConsolePrintf(RED, "Failed to query Steam Workshop maps, result: %d\n", result->m_eResult);
            m_Done = m_Error = true;
            g_SteamUGC->ReleaseQueryUGCRequest(m_QueryHandle);
            return;
        }

        uint32 results = result->m_unNumResultsReturned;
        for (uint32_t i = 0; i < results; i++)
        {
            SteamUGCDetails_t details = {};
            g_SteamUGC->GetQueryUGCResult(result->m_handle, i, &details);
            m_Files.push_back(details);
        }
            
        uint32 total_results = result->m_unTotalMatchingResults;
        g_SteamUGC->ReleaseQueryUGCRequest(m_QueryHandle);

        if (results == 0 || m_Files.size() >= total_results)
            m_Done = true;
        else
            Enumerate(++m_Page);
    }

    void Enumerate(uint32_t page)
    {
        m_QueryHandle = g_SteamUGC->CreateQueryUserUGCRequest(g_UserAccountID,
            k_EUserUGCList_Published,
            k_EUGCMatchingUGCType_Items,
            k_EUserUGCListSortOrder_CreationOrderDesc,
            g_AppID, g_AppID, page);

        if (m_QueryHandle == k_UGCQueryHandleInvalid)
        {
            ConsolePrintf(RED, "Failed to fetch Steam Workshop maps\n");
            m_Done = m_Error = true;
            return;
        }

        m_Call = g_SteamUGC->SendQueryUGCRequest(m_QueryHandle);
        if (m_Call == k_uAPICallInvalid)
        {
            ConsolePrintf(RED, "Failed to send Steam Workshop query\n");
            m_Done = m_Error = true;
            return;
        }

        m_QueryCallback.Set(m_Call, this, &UGCWrapper::CallbackQuery);
    }

    void EnumerateAll()
    {
        m_Done = false;
        m_Page = 1;
        Enumerate(m_Page);
    }

    void DownloadQuery(DownloadItemResult_t* result)
    {
        if (result->m_unAppID != g_AppID || result->m_nPublishedFileId != m_DownloadID)
            return;

        if (result->m_eResult > k_EResultOK)
        {
            ConsolePrintf(RED, "Download failed, result: %d         \n", result->m_eResult);
            ConsolePrintf(RED, "Check internet connection and ensure BSP is not open in any tool\n");
            m_Done = m_Error = true;
            return;
        }

        ConsolePrintf(GREEN, "Download successful!                                    \n");
        if (++m_Downloaded >= m_Files.size())
            m_Done = true;
        else
            DownloadFile(m_Files[m_Downloaded].m_nPublishedFileId);
    }

    void DownloadFile(PublishedFileId_t id)
    {
        ConsolePrintf(WHITE, "Downloading map %llu...\n", id);
        m_DownloadID = id;
        g_SteamUGC->DownloadItem(id, true);
    }

    void DownloadAll()
    {
        m_Done = false;
        m_DownloadCallback.Register(this, &UGCWrapper::DownloadQuery);
        m_Downloaded = 0;
        DownloadFile(m_Files[0].m_nPublishedFileId);
    }

    bool Operate(const char* bspname)
    {
        FILE* bsp = fopen(bspname, "rb");
        if (!bsp)
        {
            ConsolePrintf(RED, "Failed to open %s\n", bspname);
            return false;
        }

        fseek(bsp, 0, SEEK_END);
        size_t bsp_size = ftell(bsp);
        fseek(bsp, 0, SEEK_SET);
        char* bsp_data = new char[bsp_size];
        fread(bsp_data, 1, bsp_size, bsp);
        fclose(bsp);

        BSPHeader* header = (BSPHeader*)bsp_data;
        if (header->ident != IDBSPHEADER)
        {
            ConsolePrintf(RED, "File %s is not a valid BSP!\n", bspname);
            return false;
        }

        BSPLump& pak_file = header->lumps[40];

        int zip_len = pak_file.length;
        char* zip_buf = bsp_data + pak_file.offset;

        ZipContainer zip;

        if (g_IniWritePakFile)
        {
            zip.stream_write_mem = mz_stream_mem_create();
            mz_stream_mem_set_grow_size(zip.stream_write_mem, (1024 * 1024 * 128)); // ~128mb
            mz_stream_open(zip.stream_write_mem, NULL, MZ_OPEN_MODE_CREATE);
            mz_zip_writer_open(zip.stream_write, zip.stream_write_mem, 0);

            if (g_IniCompressPakFile)
            {
                mz_zip_writer_set_compress_method(zip.stream_write, MZ_COMPRESS_METHOD_LZMA);
                mz_zip_writer_set_compress_level(zip.stream_write, g_IniCompressionLevel);
            }
            else
            {
                mz_zip_writer_set_compress_method(zip.stream_write, MZ_COMPRESS_METHOD_STORE);
                mz_zip_writer_set_compress_level(zip.stream_write, MZ_COMPRESS_LEVEL_DEFAULT);
            }
        }

        mz_stream_mem_set_buffer(zip.stream_read_mem, zip_buf, zip_len);
        mz_zip_reader_open(zip.stream_read, zip.stream_read_mem);
        mz_zip_reader_goto_first_entry(zip.stream_read);

        ConsolePrintf(WHITE, "Decompressing pak file...\n");
        ZipFileList file_list;

        do 
        {
            mz_zip_reader_entry_open(zip.stream_read);

            mz_zip_file* file_info = nullptr;
            mz_zip_reader_entry_get_info(zip.stream_read, &file_info);

            if (g_IniPrintPakFile)
                ConsolePrintf(WHITE, "\tsize: %u\t\t%s\n", file_info->uncompressed_size, file_info->filename);

            file_list.emplace_back();
            ZipFile& zip_file = file_list.back();
            zip_file.Init(file_info->filename, file_info->uncompressed_size);

            mz_zip_reader_entry_read(zip.stream_read, zip_file.buffer, (int32_t)zip_file.size);
            mz_zip_reader_entry_close(zip.stream_read);
        } 
        while (mz_zip_reader_goto_next_entry(zip.stream_read) == MZ_OK);

        mz_zip_reader_close(zip.stream_read);
        zip.stream_read = nullptr;

        bool success = OperateZip(file_list);

        if (g_IniWritePakFile)
        {   
            if (success)
            {
                if (g_IniCompressPakFile)
                    ConsolePrintf(WHITE, "Compressing files. This will take a while!\n");
                else
                    ConsolePrintf(WHITE, "Writing pak file...\n");

                time_t the_time = time(NULL);
                size_t zip_file_count = file_list.size();
                for (size_t i = 0; i < zip_file_count; i++)
                {
                    ZipFile& zip_file = file_list[i];

                    mz_zip_file write_file_info = { 0 };
                    write_file_info.filename = zip_file.filename;
                    write_file_info.modified_date = the_time;
                    write_file_info.version_madeby = MZ_VERSION_BUILD;
                    write_file_info.compression_method = g_IniCompressPakFile ? MZ_COMPRESS_METHOD_LZMA : MZ_COMPRESS_METHOD_STORE;
                    write_file_info.flag = MZ_ZIP_FLAG_UTF8;
                    write_file_info.zip64 = MZ_ZIP64_DISABLE;

                    mz_zip_writer_entry_open(zip.stream_write, &write_file_info);
                    mz_zip_writer_entry_write(zip.stream_write, zip_file.buffer, (int32_t)zip_file.size);
                    mz_zip_writer_entry_close(zip.stream_write);

                    ConsolePrintProgress(PURPLE, i, zip_file_count);
                }

                if (g_IniCompressPakFile)
                {
                    ConsolePrintf(GREEN, "Compression successful              \n");
                    ConsolePrintf(WHITE, "Writing pak file...\n");
                }

                mz_zip_writer_close(zip.stream_write);
                zip.stream_write = nullptr;

                mz_stream_mem_get_buffer(zip.stream_write_mem, (const void**)&zip_buf);
                mz_stream_mem_get_buffer_length(zip.stream_write_mem, &zip_len);

                bsp_size -= pak_file.length;
                pak_file.length = zip_len;
            }
            else
            {
                mz_zip_writer_close(zip.stream_write);
                zip.stream_write = nullptr;
            }
        }

        for (ZipFile& zip_file : file_list)
            zip_file.Destroy();

        if (!success)
        {
            delete[] bsp_data;
            return false;
        }

        const char* temp_filename = strrchr(bspname, '/');
        if (temp_filename)
            temp_filename += 1;
        else
            temp_filename = bspname;

        strcpy(m_LastTempMap, g_MapTempPath);
        strcat(m_LastTempMap, temp_filename);

        ConsolePrintf(WHITE, "Writing temporary BSP to %s\n", m_LastTempMap);

        FILE* bsp_temp = fopen(m_LastTempMap, "wb");
        if (bsp_temp)
        {
            fwrite(bsp_data, 1, bsp_size, bsp_temp);
            fwrite(zip_buf, 1, zip_len, bsp_temp);
            fclose(bsp_temp);
            delete[] bsp_data;

            ConsolePrintf(GREEN, "Done with BSP %s\n", bspname);
            return true;
        }
        else
        {
            delete[] bsp_data;

            ConsolePrintf(RED, "Failed to open temporary BSP for writing\n");
            return false;
        }
    }

    void OperateAll()
    {
        GetTempPath(sizeof(g_MapTempPath), g_MapTempPath);
        FixSlashes(g_MapTempPath);
        strcat(g_MapTempPath, "maps/");
        _mkdir(g_MapTempPath);
        strcat(g_MapTempPath, "workshop/");
        _mkdir(g_MapTempPath);

        char temp_path[_MAX_PATH];
        snprintf(temp_path, sizeof(temp_path), "%s*", g_MapTempPath);
        WIN32_FIND_DATA find_data;
        HANDLE find = FindFirstFile(temp_path, &find_data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (find_data.cFileName[0] != '.')
                {
                    char path[_MAX_PATH];
                    sprintf(path, "%s%s", g_MapTempPath, find_data.cFileName);
                    DeleteFile(path);
                }
            } 
            while (FindNextFile(find, &find_data) != 0);
        }

        for (SteamUGCDetails_t& details : m_Files)
        {
            char folder_path[_MAX_PATH];
            uint64_t file_size;
            uint32_t timestamp;
            if (!g_SteamUGC->GetItemInstallInfo(details.m_nPublishedFileId, &file_size, folder_path, sizeof(folder_path), &timestamp))
            {
                ConsolePrintf(RED, "Failed to get install information for map %llu!\n", details.m_nPublishedFileId);
                m_Error = true;
                break;
            }

            FixSlashes(folder_path);

            // this is definitely the wrong way to do it but oh well
            char find_path[_MAX_PATH];
            snprintf(find_path, sizeof(find_path), "%s/*.bsp", folder_path);

            WIN32_FIND_DATA find;
            HANDLE find_handle = FindFirstFile(find_path, &find);
            if (find_handle == INVALID_HANDLE_VALUE)
            {
                ConsolePrintf(RED, "Failed to locate bsp for map %llu in %s!\n", details.m_nPublishedFileId, folder_path);
                m_Error = true;
                break;
            }
            FindClose(find_handle);

            snprintf(find_path, sizeof(find_path), "%s/%s", folder_path, find.cFileName);
            ConsolePrintf(WHITE, "Operating on %llu (%s)...\n", details.m_nPublishedFileId, find_path);

            if (!Operate(find_path))
            {
                m_Error = true;
                break;
            }

            m_TempMaps.push_back(m_LastTempMap);
        }

        for (std::string& map_local : g_IniLocalMaps)
        {
            const char* map_filename = map_local.c_str();
            ConsolePrintf(WHITE, "Operating on local map %s...\n", map_filename);

            if (!Operate(map_filename))
            {
                m_Error = true;
                break;
            }
        }
    }

    void CallbackUpload(SubmitItemUpdateResult_t* result, bool error)
    {
        const char* file_name = m_TempMaps[m_Uploaded].c_str();

        if (result->m_bUserNeedsToAcceptWorkshopLegalAgreement)
        {
            ConsolePrintf(RED, "Failed to upload map. User needs to agree to the workshop legal agreement\n");
            m_Done = m_Error = true;
            if (!DeleteFile(file_name))
                ConsolePrintf(RED, "Failed to delete temporary map at %s\n", file_name);
            return;
        }

        if (error || result->m_eResult != k_EResultOK)
        {
            ConsolePrintf(RED, "Failed to upload map. Result: %d\n", result->m_eResult);
            m_Done = m_Error = true;
            if (!DeleteFile(file_name))
                ConsolePrintf(RED, "Failed to delete temporary map at %s\n", file_name);
            return;
        }

        ConsolePrintf(GREEN, "Upload successful!                     \n");

        if (!DeleteFile(file_name))
        {
            ConsolePrintf(RED, "Failed to delete temporary map at %s\n", file_name);
            m_Done = m_Error = true;
            return;
        }

        if (++m_Uploaded >= m_Files.size())
            m_Done = true;
        else
        {
            ConsolePrintf(PURPLE, "Waiting a moment to not trip spam filters...\n");
            Sleep(5000);

            Upload(m_Files[m_Uploaded].m_nPublishedFileId);
        }
    }

    void Upload(PublishedFileId_t id)
    {
        ConsolePrintf(WHITE, "Uploading map %llu...\n", id);

        m_UploadHandle = g_SteamUGC->StartItemUpdate(g_AppID, id);
        if (m_UploadHandle == k_UGCUpdateHandleInvalid)
        {
            ConsolePrintf(RED, "Failed to begin update for %llu!\n", id);
            m_Done = m_Error = true;
            return;
        }

        const char* map_path = m_TempMaps[m_Uploaded].c_str();
        const char* map_name = strrchr(map_path, '/');
        if (!map_name)
        {
            ConsolePrintf(RED, "Bad path for map %llu (%s)\n", id, map_path);
            m_Done = m_Error = true;
            return;
        }
        map_name++;

        if (!g_SteamUGC->SetItemContent(m_UploadHandle, map_path) ||
            !g_SteamUGC->SetItemMetadata(m_UploadHandle, map_name))
        {
            ConsolePrintf(RED, "Failed to set map data for %llu (%s)\n", id, map_path);
            m_Done = m_Error = true;
            return;
        }

        m_Call = g_SteamUGC->SubmitItemUpdate(m_UploadHandle, g_IniChangeNote[0] ? g_IniChangeNote : NULL);
        if (m_Call == k_uAPICallInvalid)
        {
            ConsolePrintf(RED, "Failed to send Steam Upload message\n");
            m_Done = m_Error = true;
            return;
        }

        m_UploadCallback.Set(m_Call, this, &UGCWrapper::CallbackUpload);
    }

    void UploadAll()
    {
        m_Done = false;
        m_UploadHandle = k_UGCUpdateHandleInvalid;
        m_Uploaded = 0;
        Upload(m_Files[0].m_nPublishedFileId);
    }

    void PurgeUnused()
    {
        for (int i = (int)m_Files.size() - 1; i >= 0; i--)
            if (!IsElementInVector(g_IniMaps, m_Files[i].m_nPublishedFileId))
                EraseElement(m_Files, i);
    }

    SteamAPICall_t m_Call;
    CCallResult<UGCWrapper, SteamUGCQueryCompleted_t> m_QueryCallback;
    CCallback<UGCWrapper, DownloadItemResult_t, false> m_DownloadCallback;
    CCallResult<UGCWrapper, SubmitItemUpdateResult_t> m_UploadCallback;
    std::vector<SteamUGCDetails_t> m_Files;
    std::vector<std::string> m_TempMaps;

    UGCQueryHandle_t m_QueryHandle;
    uint32_t m_Page;

    PublishedFileId_t m_DownloadID;
    size_t m_Downloaded;

    UGCUpdateHandle_t m_UploadHandle;
    size_t m_Uploaded;

    char m_LastTempMap[_MAX_PATH];
    bool m_Done;
    bool m_Error;
};

UGCWrapper g_UGCWrapper;

static bool IsUGCQueryFinished()
{
    return g_UGCWrapper.m_Done;
}

static bool IsUGCDownloadFinished()
{
    if (g_UGCWrapper.m_Done)
        return true;

    uint64 bytes_downloaded = 0;
    uint64 bytes_total = 0;
    if (g_SteamUGC->GetItemDownloadInfo(g_UGCWrapper.m_DownloadID, &bytes_downloaded, &bytes_total))
        ConsolePrintProgress(PURPLE, bytes_downloaded, bytes_total);

    return false;
}

static bool IsUGCUploadFinished()
{
    if (g_UGCWrapper.m_Done)
        return true;

    uint64 bytes_uploaded = 0;
    uint64 bytes_total = 0;
    if (g_SteamUGC->GetItemUpdateProgress(g_UGCWrapper.m_UploadHandle, &bytes_uploaded, &bytes_total))
        ConsolePrintProgress(PURPLE, bytes_uploaded, bytes_total);

    return false;
}

static bool FindUGCMaps()
{
    if (g_IniMaps.size() == 0)
    {
        if (g_IniLocalMaps.size() > 0)
        {
            g_IniDownloadMaps = false;
            g_IniUploadMaps = false;
            return true;
        }

        ConsolePrintf(RED, "No map IDs defined in %s\n", g_ConfigName);
        return false;
    }

    ConsolePrintf(PURPLE, "Finding owned workshop maps...\n");

    g_UGCWrapper.EnumerateAll();
    SleepUntilCondition(IsUGCQueryFinished, 100);

    ConsolePrintf(WHITE, "Found %u owned workshop maps\n", g_UGCWrapper.m_Files.size());
    for (SteamUGCDetails_t& details : g_UGCWrapper.m_Files)
    {
        if (details.m_eFileType != k_EWorkshopFileTypeCommunity)
            continue;

        ConsoleColors clr = WHITE;
        if (IsElementInVector(g_IniMaps, details.m_nPublishedFileId))
            clr = AQUA;
        ConsolePrintf(clr, "\t%s (%llu)\n", details.m_rgchTitle, details.m_nPublishedFileId);
    }

    bool ok = !g_UGCWrapper.m_Error;
    for (PublishedFileId_t id : g_IniMaps)
    {
        for (SteamUGCDetails_t& details : g_UGCWrapper.m_Files)
            if (details.m_nPublishedFileId == id)
                goto next;

        ConsolePrintf(RED, "Failed to find workshop map %llu listed in %s\n", id, g_ConfigName);
        ok = false;
        next: {}
    }

    g_UGCWrapper.PurgeUnused();

    if (g_UGCWrapper.m_Files.size() == 0)
    {
        ConsolePrintf(RED, "ERROR: No owned workshop maps found!\n");
        return false;
    }

    return ok;
}

static bool DownloadUGCMaps()
{
    ConsolePrintf(PURPLE, "Downloading workshop maps to modify...\n");

    g_UGCWrapper.DownloadAll();
    SleepUntilCondition(IsUGCDownloadFinished, 100);
    return !g_UGCWrapper.m_Error;
}

static bool OperateUGCMaps()
{
    ConsolePrintf(PURPLE, "Operating on selected maps...\n");

    g_UGCWrapper.OperateAll();

    if (g_UGCWrapper.m_Error)
        return false;

    ShellExecute(NULL, "open", g_MapTempPath, NULL, NULL, SW_SHOWDEFAULT);
    return true;
}

static bool UploadUGCMaps()
{
    ConsolePrintf(RED, "*********************************************************\n");
    ConsolePrintf(WHITE, "The next step uploads the modified maps to the workshop.\n");
    ConsolePrintf(WHITE, "Before proceding, ensure you have reviewed one of the modified BSPs.\n");
    ConsolePrintf(WHITE, "Copy paste any .bsp from the temp folder to your TF2 maps folder and check in-game to ensure it works as expected.\n");
    ConsolePrintf(WHITE, "To continue, type in the text \"iamsure\". If something is wrong, type the text \"abort\".\n");
    ConsolePrintf(YELLOW, "NOTE: TF2 won't launch if not currently open because Steam believes it's already running.\n");
    ConsolePrintf(YELLOW, "      As a workaround, you can launch it manually through hl2.exe, for example through a .bat file\n");
    ConsolePrintf(YELLOW, "      hl2.exe -game tf -steam -insecure\n");
    ConsolePrintf(RED, "*********************************************************\n");

    ShellExecute(NULL, "open", g_MapTempPath, NULL, NULL, SW_SHOWDEFAULT);

    do
    {
        char input[256] = { 0 };
        scanf("%s", input);

        if (!strcmp(input, "abort"))
            return false;
        else if (!strcmp(input, "iamsure"))
            break;
    }
    while (true);

    ConsolePrintf(PURPLE, "Uploading modified workshop maps...\n");

    g_UGCWrapper.UploadAll();
    SleepUntilCondition(IsUGCUploadFinished, 100);

    return !g_UGCWrapper.m_Error;
}

static bool ParseIni()
{
    ConsolePrintf(WHITE, "Reading %s ...\n", g_ConfigName);

    FILE* ini = fopen(g_ConfigName, "r");
    if (!ini)
    {
        ConsolePrintf(RED, "Failed to open %s. Does the file exist?\n", g_ConfigName);
        return false;
    }

    bool success = true;
    enum IniSections
    {
        SECTION_INVALID = 1,
        SECTION_MAPS,
        SECTION_OPERATIONS,
        SECTION_TOOL,
    };
    IniSections parse_section = SECTION_INVALID;

    char line[1024];
    for (int line_counter = 0; fgets(line, sizeof(line), ini); line_counter++)
    {
        if (line[0] == '\0' || line[0] == ';' || line[0] == '\n' || line[0] == '\r')
            continue;

        line[strcspn(line, "\n")] = 0;

        if (line[0] == '[')
        {
            if (!strnicmp(line, "[Maps]", 6))
                parse_section = SECTION_MAPS;
            else if (!strnicmp(line, "[Operations]", 12))
                parse_section = SECTION_OPERATIONS;
            else if (!strnicmp(line, "[Tool]", 6))
                parse_section = SECTION_TOOL;
            else
            {
                ConsolePrintf(RED, "%s: Unrecognized section '%s' on line %d\n", g_ConfigName, line, line_counter);
                success = false;
            }
            continue;
        }

        char* key = strtok(line, "=");
        if (!key)
        {
            ConsolePrintf(RED, "%s: Missing token = on line %d\n", g_ConfigName, line_counter);
            success = false;
            continue;
        }

        char* value = strtok(NULL, "=");
        if (!value)
        {
            ConsolePrintf(RED, "%s: Missing value on line %d\n", g_ConfigName, line_counter);
            success = false;
            continue;
        }
        FixSlashes(value);

        if (parse_section == SECTION_MAPS)
        {
            if (strspn(value, "0123456789") == strlen(value))
                g_IniMaps.emplace_back(strtoull(value, NULL, 10));
            else
                g_IniLocalMaps.emplace_back(value);
        }
        else if (parse_section == SECTION_OPERATIONS)
        {
            if (!strcmp(key, "ADD"))
                g_IniOperations.emplace_back(new OperationAdd(value));
            else if (!strcmp(key, "REMOVE"))
                g_IniOperations.emplace_back(new OperationRemove(value));
            else
            {
                ConsolePrintf(RED, "%s: Unrecognized operation '%s' on line %d\n", g_ConfigName, value);
                success = false;
            }
        }
        else if (parse_section == SECTION_TOOL)
        {
            if (!strcmp(key, "DownloadMaps"))
                g_IniDownloadMaps = !!atoi(value);
            else if (!strcmp(key, "LogOperations"))
                g_IniLogOperations = !!atoi(value);
            else if (!strcmp(key, "OperateMaps"))
                g_IniOperateMaps = !!atoi(value);
            else if (!strcmp(key, "PrintPakFile"))
                g_IniPrintPakFile = !!atoi(value);
            else if (!strcmp(key, "CompressPakFile"))
                g_IniCompressPakFile = !!atoi(value);
            else if (!strcmp(key, "CompressionLevel"))
                g_IniCompressionLevel = atoi(value);
            else if (!strcmp(key, "WritePakFile"))
                g_IniWritePakFile = !!atoi(value);
            else if (!strcmp(key, "UploadMaps"))
                g_IniUploadMaps = !!atoi(value);
            else if (!strcmp(key, "ChangeNote"))
            {
                if (!strcmp(value, "NULL"))
                    g_IniChangeNote[0] = '\0';
                else
                    strncpy(g_IniChangeNote, value, sizeof(g_IniChangeNote));
            }
            else
            {
                ConsolePrintf(RED, "%s: Unrecognized debug key '%s' on line % d\n", g_ConfigName, value);
                success = false;
            }
        }
    }

    fclose(ini);
    return success;
}

static int PerformUGCWork()
{
    if (!FindUGCMaps())
        return 1;
    if (g_IniDownloadMaps && !DownloadUGCMaps())
        return 1;
    if (g_IniOperateMaps && !OperateUGCMaps())
        return 1;
    if (g_IniUploadMaps && !UploadUGCMaps())
        return 1;
    return 0;
}

int main()
{
	g_Console = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_Console)
    {
        printf("Failed to get console handle\n");
        return 1;
    }

    ConsolePrintf(AQUA, "Map Batch Updater by ficool2 (%s)\n", __DATE__);

    if (!ParseIni())
    {
        ConsoleWaitForKey();
        return 1;
    }

    if (!SteamInit())
	{
		ConsoleWaitForKey();
        return 1;
	}
	
    ConsolePrintf(GREEN, "Initialized Steam as %s (%llu)\n", g_SteamFriends->GetPersonaName(), g_UserSteamID.ConvertToUint64());

    int ret = PerformUGCWork();

    SteamAPI_Shutdown();

    ConsolePrintf(AQUA, "Finished!\n");
    ConsoleWaitForKey();
    return ret;
}