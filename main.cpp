/**********************************************************************
 *  Win32 Disk Imager - native Win32 rewrite (no Qt, no custom controls)
 *  GPL-2.0-or-later, same as the original project.
 **********************************************************************/

#define _WIN32_WINNT 0x0601
#define WINVER       0x0601

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <dbt.h>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "resource.h"

#ifndef BCRYPT_MD5_ALGORITHM
#define BCRYPT_MD5_ALGORITHM    L"MD5"
#define BCRYPT_SHA1_ALGORITHM   L"SHA1"
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#endif

static const ULONGLONG CHUNK_SECTORS = 1024;

enum { STATUS_IDLE, STATUS_READING, STATUS_WRITING, STATUS_VERIFYING, STATUS_EXIT, STATUS_CANCELED };

static HINSTANCE g_inst;
static HWND      g_hwnd;
static HWND      g_status;
static LANGID    g_lang;
static int       g_status_id;
static std::wstring g_home;
static std::wstring g_start_file;
static ULONGLONG g_start_tick, g_last_tick, g_last_done;

/* ---------- i18n ---------- */

static std::wstring Str(UINT id)
{
    wchar_t buf[2048] = L"";
    LoadStringW(g_inst, id, buf, 2048);
    return buf;
}

static void ShowError(HWND owner, UINT title, UINT body, ...)
{
    wchar_t msg[2048] = L"";
    va_list ap;
    va_start(ap, body);
    vswprintf(msg, 2048, Str(body).c_str(), ap);
    va_end(ap);
    MessageBoxW(owner, msg, Str(title).c_str(), MB_OK | MB_ICONERROR);
}

static void ShowInfo(HWND owner, UINT title, UINT body)
{
    MessageBoxW(owner, Str(body).c_str(), Str(title).c_str(), MB_OK | MB_ICONINFORMATION);
}

static std::wstring SystemError(DWORD err)
{
    wchar_t buf[512] = L"";
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, err, g_lang, buf, 512, NULL))
        swprintf(buf, 512, L"(error %u)", (unsigned)err);
    size_t n = wcslen(buf);
    while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' '))
        buf[--n] = 0;
    return buf;
}

static void ErrLast(HWND owner, UINT title, UINT body)
{
    DWORD err = GetLastError();
    ShowError(owner, title, body, (unsigned)err, SystemError(err).c_str());
}

/* ---------- small helpers ---------- */

static std::wstring GetFileText(HWND h)
{
    wchar_t buf[1024] = L"";
    GetDlgItemTextW(h, IDC_EDIT_FILE, buf, 1024);
    return buf;
}

static std::wstring ResolvePath(std::wstring path)
{
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == L'/') path[i] = L'\\';
    if (path.empty()) return path;
    bool absolute = (path.size() > 1 && path[1] == L':') || path[0] == L'\\';
    return absolute ? path : g_home + L"\\" + path;
}

static bool FileReadable(const std::wstring &path, ULONGLONG *size, bool *exists)
{
    if (exists) *exists = false;
    if (size) *size = 0;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    if (exists) *exists = true;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz = {};
    BOOL ok = GetFileSizeEx(f, &sz);
    CloseHandle(f);
    if (ok && size) *size = (ULONGLONG)sz.QuadPart;
    return ok != 0;
}

static int SelectedVolume(HWND h)
{
    wchar_t text[32] = L"";
    GetDlgItemTextW(h, IDC_COMBO_DEVICE, text, 32);
    if (text[0] == L'[' && text[2] == L':' && text[3] == L'\\')
        return text[1] - L'A';
    return -1;
}

static bool SameDrive(const std::wstring &path, int letter)
{
    return path.size() > 1 && towupper(path[0]) == (wchar_t)(L'A' + letter) && path[1] == L':';
}

/* ---------- disk layer ---------- */

struct AutoHandle
{
    HANDLE h;
    explicit AutoHandle(HANDLE x) : h(x) {}
    ~AutoHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    operator HANDLE() const { return h; }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    AutoHandle(const AutoHandle &) = delete;
    AutoHandle &operator=(const AutoHandle &) = delete;
};

static void UnlockVolume(HANDLE vol)
{
    DWORD bytes = 0;
    DeviceIoControl(vol, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytes, NULL);
}

struct AutoVolume
{
    HANDLE h;
    explicit AutoVolume(HANDLE x) : h(x) {}
    ~AutoVolume()
    {
        if (h != INVALID_HANDLE_VALUE) {
            UnlockVolume(h);
            CloseHandle(h);
        }
    }
    operator HANDLE() const { return h; }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    AutoVolume(const AutoVolume &) = delete;
    AutoVolume &operator=(const AutoVolume &) = delete;
};

static HANDLE OpenVolume(int letter, DWORD access)
{
    wchar_t name[8] = L"\\\\.\\A:";
    name[4] = (wchar_t)(L'A' + letter);
    HANDLE h = CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        ErrLast(g_hwnd, IDS_VOLUME_ERROR, IDS_ERR_VOLUME_HANDLE);
    return h;
}

static HANDLE OpenDisk(DWORD number, DWORD access)
{
    wchar_t name[64];
    swprintf(name, 64, L"\\\\.\\PhysicalDrive%u", (unsigned)number);
    HANDLE h = CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        ErrLast(g_hwnd, IDS_DEVICE_ERROR, IDS_ERR_DEVICE_HANDLE);
    return h;
}

static HANDLE OpenImage(const std::wstring &path, DWORD access)
{
    DWORD share = (access == GENERIC_READ) ? FILE_SHARE_READ : 0;
    DWORD disp = (access == GENERIC_READ) ? OPEN_EXISTING : CREATE_ALWAYS;
    HANDLE h = CreateFileW(path.c_str(), access, share, NULL, disp, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        ErrLast(g_hwnd, IDS_FILE_ERROR, IDS_ERR_FILE_HANDLE);
    return h;
}

static bool VolumeDiskNumber(HANDLE vol, DWORD *number)
{
    VOLUME_DISK_EXTENTS ext = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(vol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                         &ext, sizeof(ext), &bytes, NULL)) {
        ErrLast(g_hwnd, IDS_VOLUME_ERROR, IDS_ERR_VOLUME_INFO);
        return false;
    }
    *number = ext.Extents[0].DiskNumber;
    return true;
}

static bool LockVolume(HANDLE vol)
{
    DWORD bytes = 0;
    if (!DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytes, NULL)) {
        ErrLast(g_hwnd, IDS_LOCK_ERROR, IDS_ERR_LOCK);
        return false;
    }
    return true;
}

static bool DismountVolume(HANDLE vol)
{
    DWORD bytes = 0;
    if (!DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytes, NULL)) {
        ErrLast(g_hwnd, IDS_DISMOUNT_ERROR, IDS_ERR_DISMOUNT);
        return false;
    }
    return true;
}

static bool ReadSectors(HANDLE h, ULONGLONG start, DWORD count, DWORD sector_size, BYTE *buf)
{
    DWORD want = count * sector_size, got = 0;
    LARGE_INTEGER pos = {};
    pos.QuadPart = start * sector_size;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    if (!ReadFile(h, buf, want, &got, NULL)) {
        ErrLast(g_hwnd, IDS_READ_ERROR, IDS_ERR_READ);
        return false;
    }
    if (got < want)
        memset(buf + got, 0, want - got);
    return true;
}

static bool WriteSectors(HANDLE h, ULONGLONG start, DWORD count, DWORD sector_size, const BYTE *buf)
{
    DWORD want = count * sector_size, got = 0;
    LARGE_INTEGER pos = {};
    pos.QuadPart = start * sector_size;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    if (!WriteFile(h, buf, want, &got, NULL) || got != want) {
        ErrLast(g_hwnd, IDS_WRITE_ERROR, IDS_ERR_WRITE);
        return false;
    }
    return true;
}

static ULONGLONG DiskSectors(HANDLE disk, DWORD *sector_size)
{
    DISK_GEOMETRY_EX geo = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                         &geo, sizeof(geo), &bytes, NULL)) {
        ErrLast(g_hwnd, IDS_DEVICE_ERROR, IDS_ERR_GEOMETRY);
        return 0;
    }
    *sector_size = geo.Geometry.BytesPerSector;
    return geo.DiskSize.QuadPart / geo.Geometry.BytesPerSector;
}

static ULONGLONG FileSectors(HANDLE file, DWORD sector_size)
{
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size)) {
        ErrLast(g_hwnd, IDS_FILE_ERROR, IDS_ERR_FSIZE);
        return 0;
    }
    ULONGLONG n = (ULONGLONG)size.QuadPart;
    return n / sector_size + ((n % sector_size) ? 1 : 0);
}

static bool FreeSpaceAvailable(const std::wstring &path, ULONGLONG needed)
{
    wchar_t root[4] = { path[0], L':', L'\\', 0 };
    ULARGE_INTEGER free_bytes = {};
    if (!GetDiskFreeSpaceExW(root, NULL, NULL, &free_bytes)) {
        DWORD err = GetLastError();
        ShowError(g_hwnd, IDS_FREE_SPACE_ERROR, IDS_ERR_FREE_SPACE,
                  root, (unsigned)err, SystemError(err).c_str());
        return true;
    }
    return needed <= free_bytes.QuadPart;
}

static void DriveLabel(int letter, wchar_t *label, int cch)
{
    wchar_t root[4] = { (wchar_t)(L'A' + letter), L':', L'\\', 0 };
    label[0] = 0;
    GetVolumeInformationW(root, label, cch, NULL, NULL, NULL, NULL, 0);
}

static ULONGLONG PartitionEnd(const BYTE *mbr)
{
    ULONGLONG end = 1;
    for (int i = 0; i < 4; ++i) {
        const BYTE *e = mbr + 0x1BE + 16 * i;
        DWORD start = *(const DWORD *)(e + 8);
        DWORD count = *(const DWORD *)(e + 12);
        if (count && (ULONGLONG)start + count > end)
            end = (ULONGLONG)start + count;
    }
    return end;
}

/* Enumerate one drive; true when it is a removable/USB/SD/MMC media device. */
static bool CheckDriveType(const wchar_t *root, DWORD *disk_number)
{
    UINT type = GetDriveTypeW(root);
    if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED)
        return false;

    wchar_t name[8] = L"\\\\.\\A:";
    name[4] = root[0];
    HANDLE h = CreateFileW(name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD bytes = 0;
    DISK_GEOMETRY geo = {};
    bool media = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                                 &geo, sizeof(geo), &bytes, NULL) &&
                 (geo.MediaType == FixedMedia || geo.MediaType == RemovableMedia);

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE desc_buf[1024] = {};
    STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)desc_buf;
    desc->Size = sizeof(desc_buf);
    STORAGE_DEVICE_NUMBER number = {};
    bool prop = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                desc, desc->Size, &bytes, NULL) != 0;
    bool num_ok = prop && DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0,
                                          &number, sizeof(number), &bytes, NULL) != 0;

    bool wanted = media && num_ok &&
        ((type == DRIVE_REMOVABLE && desc->BusType != BusTypeSata) ||
         (type == DRIVE_FIXED && (desc->BusType == BusTypeUsb ||
                                  desc->BusType == BusTypeSd ||
                                  desc->BusType == BusTypeMmc)));

    bool ok = false;
    if (wanted) {
        ok = DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY2, NULL, 0, NULL, 0, &bytes, NULL) != 0;
        if (!ok)
            ok = DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &bytes, NULL) != 0;
    }
    CloseHandle(h);
    if (ok && disk_number)
        *disk_number = number.DeviceNumber;
    return ok;
}

/* ---------- status / progress ---------- */

static void SetStatusText(std::wstring text)
{
    if (g_status)
        SendMessageW(g_status, SB_SETTEXTW, 0 | SBT_NOBORDERS, (LPARAM)text.c_str());
}

static void ProgressStart(std::wstring text)
{
    SetStatusText(text);
    SendDlgItemMessageW(g_hwnd, IDC_PROGRESS, PBM_SETPOS, 0, 0);
    SetDlgItemTextW(g_hwnd, IDC_STATIC_PERCENT, L"0%");
    g_start_tick = g_last_tick = GetTickCount64();
    g_last_done = 0;
}

static void ProgressUpdate(ULONGLONG done, ULONGLONG total, DWORD sector_size)
{
    int pct = total ? (int)(done * 100 / total) : 0;
    if (pct > 100) pct = 100;
    SendDlgItemMessageW(g_hwnd, IDC_PROGRESS, PBM_SETPOS, pct, 0);
    wchar_t text[32];
    swprintf(text, 32, L"%d%%", pct);
    SetDlgItemTextW(g_hwnd, IDC_STATIC_PERCENT, text);

    ULONGLONG now = GetTickCount64();
    if (now - g_last_tick < 1000)
        return;

    double mbps = (double)sector_size * (double)(done - g_last_done) / 1048576.0 *
                  1000.0 / (double)(now - g_last_tick);
    swprintf(text, 32, L"%.1f MB/s", mbps);
    SendMessageW(g_status, SB_SETTEXTW, 0 | SBT_NOBORDERS, (LPARAM)text);

    ULONGLONG elapsed = (now - g_start_tick) / 1000;
    ULONGLONG remain = done ? (ULONGLONG)((double)elapsed * (double)total / (double)done) : 0;
    swprintf(text, 32, L"%u:%02u/%u:%02u", (unsigned)(elapsed / 60), (unsigned)(elapsed % 60),
             (unsigned)(remain / 60), (unsigned)(remain % 60));
    SendMessageW(g_status, SB_SETTEXTW, 1 | SBT_NOBORDERS, (LPARAM)text);

    g_last_tick = now;
    g_last_done = done;
}

static void Pump(HWND h)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(0);
            return;
        }
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

/* ---------- hashing ---------- */

static bool HashFile(const std::wstring &path, int alg, std::wstring &hex)
{
    static const wchar_t *algorithms[] = {
        BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM
    };
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        ShowError(g_hwnd, IDS_HASH_ERROR, IDS_HASH_OPEN_FAILED);
        return false;
    }

    BCRYPT_ALG_HANDLE provider = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    std::vector<BYTE> object, digest, buffer(65536);
    DWORD object_len = 0, digest_len = 0, cb = 0;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&provider, algorithms[alg], NULL, 0);
    bool ok = false;
    if (st >= 0) {
        if (BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_len,
                              sizeof(object_len), &cb, 0) >= 0 &&
            BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_len,
                              sizeof(digest_len), &cb, 0) >= 0) {
            object.resize(object_len);
            digest.resize(digest_len);
            st = BCryptCreateHash(provider, &hash, object.data(), object_len, NULL, 0, 0);
            if (st >= 0) {
                DWORD read = 0;
                while (ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, NULL) && read)
                    BCryptHashData(hash, buffer.data(), read, 0);
                st = BCryptFinishHash(hash, digest.data(), digest_len, 0);
                if (st >= 0) {
                    wchar_t byte[3];
                    hex.clear();
                    for (size_t i = 0; i < digest.size(); ++i) {
                        swprintf(byte, 3, L"%02x", digest[i]);
                        hex += byte;
                    }
                    ok = true;
                }
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (provider) BCryptCloseAlgorithmProvider(provider, 0);
    CloseHandle(file);
    if (!ok)
        ShowError(g_hwnd, IDS_HASH_ERROR, IDS_HASH_FAILED, (unsigned)st);
    return ok;
}

/* ---------- image operations ---------- */

static bool ExtraDataPresent(HANDLE image, ULONGLONG from, ULONGLONG total, DWORD sector_size)
{
    std::vector<BYTE> buffer((size_t)CHUNK_SECTORS * sector_size);
    for (ULONGLONG i = from; i < total; i += CHUNK_SECTORS) {
        DWORD count = (DWORD)((total - i >= CHUNK_SECTORS) ? CHUNK_SECTORS : (total - i));
        if (!ReadSectors(image, i, count, sector_size, buffer.data()))
            return false;
        for (DWORD j = 0; j < count * sector_size; ++j)
            if (buffer[j]) return true;
    }
    return false;
}

static bool WriteImage(const std::wstring &path, int letter)
{
    AutoVolume volume(OpenVolume(letter, GENERIC_WRITE));
    if (!volume.ok()) return false;
    DWORD disk_number = 0;
    if (!VolumeDiskNumber(volume, &disk_number)) return false;
    if (!LockVolume(volume)) return false;
    if (!DismountVolume(volume)) return false;
    AutoHandle image(OpenImage(path, GENERIC_READ));
    if (!image.ok()) return false;
    AutoHandle disk(OpenDisk(disk_number, GENERIC_WRITE));
    if (!disk.ok()) return false;

    DWORD sector_size = 0;
    ULONGLONG available = DiskSectors(disk, &sector_size);
    if (!available) return false;
    ULONGLONG total = FileSectors(image, sector_size);
    if (!total) return false;

    if (total > available) {
        bool extra = ExtraDataPresent(image, available, total, sector_size);
        wchar_t msg[1024];
        swprintf(msg, 1024, Str(IDS_WRITE_SIZE_WARN).c_str(), total, available,
                 (unsigned)sector_size, Str(extra ? IDS_DOES : IDS_DOES_NOT).c_str());
        if (MessageBoxW(g_hwnd, msg, Str(IDS_NOT_ENOUGH).c_str(),
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return false;
        total = available;
    }

    std::vector<BYTE> buffer((size_t)CHUNK_SECTORS * sector_size);
    ProgressStart(Str(IDS_WAITING));
    for (ULONGLONG i = 0; i < total && g_status_id == STATUS_WRITING; i += CHUNK_SECTORS) {
        DWORD count = (DWORD)((total - i >= CHUNK_SECTORS) ? CHUNK_SECTORS : (total - i));
        if (!ReadSectors(image, i, count, sector_size, buffer.data())) return false;
        if (!WriteSectors(disk, i, count, sector_size, buffer.data())) return false;
        ProgressUpdate(i + count, total, sector_size);
        Pump(g_hwnd);
    }
    return g_status_id != STATUS_CANCELED;
}

static bool ReadImage(const std::wstring &path, int letter)
{
    AutoVolume volume(OpenVolume(letter, GENERIC_READ));
    if (!volume.ok()) return false;
    DWORD disk_number = 0;
    if (!VolumeDiskNumber(volume, &disk_number)) return false;
    if (!LockVolume(volume)) return false;
    if (!DismountVolume(volume)) return false;
    AutoHandle disk(OpenDisk(disk_number, GENERIC_READ));
    if (!disk.ok()) return false;

    DWORD sector_size = 0;
    ULONGLONG total = DiskSectors(disk, &sector_size);
    if (!total) return false;

    if (IsDlgButtonChecked(g_hwnd, IDC_CHK_PARTITIONS) == BST_CHECKED) {
        BYTE mbr[512];
        if (ReadSectors(disk, 0, 1, 512, mbr))
            total = PartitionEnd(mbr);
    }

    AutoHandle image(OpenImage(path, GENERIC_WRITE));
    if (!image.ok()) return false;

    ULONGLONG existing = FileSectors(image, sector_size);
    ULONGLONG needed = total > existing ? (total - existing) * sector_size : 0;
    if (!FreeSpaceAvailable(path, needed)) {
        ShowError(g_hwnd, IDS_WRITE_ERROR, IDS_DISK_TOO_SMALL);
        return false;
    }

    std::vector<BYTE> buffer((size_t)CHUNK_SECTORS * sector_size);
    ProgressStart(Str(IDS_WAITING));
    for (ULONGLONG i = 0; i < total && g_status_id == STATUS_READING; i += CHUNK_SECTORS) {
        DWORD count = (DWORD)((total - i >= CHUNK_SECTORS) ? CHUNK_SECTORS : (total - i));
        if (!ReadSectors(disk, i, count, sector_size, buffer.data())) return false;
        if (!WriteSectors(image, i, count, sector_size, buffer.data())) return false;
        ProgressUpdate(i + count, total, sector_size);
        Pump(g_hwnd);
    }
    return g_status_id != STATUS_CANCELED;
}

static bool VerifyImage(const std::wstring &path, int letter)
{
    AutoVolume volume(OpenVolume(letter, GENERIC_READ));
    if (!volume.ok()) return false;
    DWORD disk_number = 0;
    if (!VolumeDiskNumber(volume, &disk_number)) return false;
    if (!LockVolume(volume)) return false;
    if (!DismountVolume(volume)) return false;
    AutoHandle image(OpenImage(path, GENERIC_READ));
    if (!image.ok()) return false;
    AutoHandle disk(OpenDisk(disk_number, GENERIC_READ));
    if (!disk.ok()) return false;

    DWORD sector_size = 0;
    ULONGLONG available = DiskSectors(disk, &sector_size);
    if (!available) return false;
    ULONGLONG total = FileSectors(image, sector_size);
    if (!total) return false;

    if (total > available) {
        bool extra = ExtraDataPresent(image, available, total, sector_size);
        wchar_t msg[1024];
        swprintf(msg, 1024, Str(IDS_VERIFY_SIZE_WARN).c_str(), total, available,
                 (unsigned)sector_size, Str(extra ? IDS_DOES : IDS_DOES_NOT).c_str());
        if (MessageBoxW(g_hwnd, msg, Str(IDS_SIZE_MISMATCH).c_str(),
                        MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return false;
        total = available;
    }

    std::vector<BYTE> image_data((size_t)CHUNK_SECTORS * sector_size);
    std::vector<BYTE> disk_data((size_t)CHUNK_SECTORS * sector_size);
    ProgressStart(Str(IDS_WAITING));
    for (ULONGLONG i = 0; i < total && g_status_id == STATUS_VERIFYING; i += CHUNK_SECTORS) {
        DWORD count = (DWORD)((total - i >= CHUNK_SECTORS) ? CHUNK_SECTORS : (total - i));
        if (!ReadSectors(image, i, count, sector_size, image_data.data())) return false;
        if (!ReadSectors(disk, i, count, sector_size, disk_data.data())) return false;
        if (memcmp(image_data.data(), disk_data.data(), (size_t)count * sector_size) != 0) {
            wchar_t msg[256];
            swprintf(msg, 256, Str(IDS_VERIFY_FAILED_AT).c_str(), i);
            MessageBoxW(g_hwnd, msg, Str(IDS_VERIFY_FAILURE).c_str(), MB_OK | MB_ICONERROR);
            return false;
        }
        ProgressUpdate(i + count, total, sector_size);
        Pump(g_hwnd);
    }
    return g_status_id != STATUS_CANCELED;
}

/* ---------- UI ---------- */

static void UpdateButtons(HWND h);
static void UpdateHash(HWND h, bool clear);

static void Layout(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);
    if (!g_status) return;
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int height = sr.bottom - sr.top;
    MoveWindow(g_status, 0, rc.bottom - height, rc.right, height, TRUE);
    int parts[2] = { rc.right - 130, -1 };
    SendMessageW(g_status, SB_SETPARTS, 2, (LPARAM)parts);
}

static void CreateStatusBar(HWND h)
{
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, h, NULL, g_inst, NULL);
}

static void CenterWindow(HWND h)
{
    RECT rc, work;
    GetWindowRect(h, &rc);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - (rc.right - rc.left)) / 2;
    int y = work.top + ((work.bottom - work.top) - (rc.bottom - rc.top)) / 2;
    SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void RefreshDevices(HWND h)
{
    wchar_t previous[32] = L"";
    GetDlgItemTextW(h, IDC_COMBO_DEVICE, previous, 32);
    SendDlgItemMessageW(h, IDC_COMBO_DEVICE, CB_RESETCONTENT, 0, 0);

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        DWORD disk = 0;
        if (!CheckDriveType(root, &disk)) continue;
        wchar_t text[8];
        swprintf(text, 8, L"[%c:\\]", (wchar_t)(L'A' + i));
        SendDlgItemMessageW(h, IDC_COMBO_DEVICE, CB_ADDSTRING, 0, (LPARAM)text);
    }

    int selection = 0;
    if (previous[0]) {
        LRESULT found = SendDlgItemMessageW(h, IDC_COMBO_DEVICE, CB_FINDSTRINGEXACT,
                                            (WPARAM)-1, (LPARAM)previous);
        if (found != CB_ERR) selection = (int)found;
    }
    SendDlgItemMessageW(h, IDC_COMBO_DEVICE, CB_SETCURSEL, selection, 0);
    UpdateButtons(h);
}

static void UpdateHash(HWND h, bool clear)
{
    if (clear)
        SetDlgItemTextW(h, IDC_EDIT_HASH, L"");
    UpdateButtons(h);
}

static void UpdateButtons(HWND h)
{
    std::wstring raw = GetFileText(h);
    std::wstring path = ResolvePath(raw);
    ULONGLONG size = 0;
    bool exists = false;
    bool readable = !raw.empty() && FileReadable(path, &size, &exists);
    DWORD attr = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    bool directory = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool writable = !exists || !(attr & FILE_ATTRIBUTE_READONLY);
    bool device = SelectedVolume(h) >= 0;

    EnableWindow(GetDlgItem(h, IDC_BTN_READ), device && !raw.empty() && !directory && writable);
    EnableWindow(GetDlgItem(h, IDC_BTN_WRITE), device && readable);
    EnableWindow(GetDlgItem(h, IDC_BTN_VERIFY), device && readable);

    int hash_type = (int)SendDlgItemMessageW(h, IDC_COMBO_HASH, CB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(h, IDC_BTN_HASHGEN), hash_type > 0 && readable && size > 0);

    wchar_t digest[1024] = L"";
    GetDlgItemTextW(h, IDC_EDIT_HASH, digest, 1024);
    EnableWindow(GetDlgItem(h, IDC_BTN_HASHCOPY), digest[0] != 0);
}

static void BeginOp(HWND h)
{
    EnableWindow(GetDlgItem(h, IDC_BTN_CANCEL), TRUE);
    EnableWindow(GetDlgItem(h, IDC_BTN_READ), FALSE);
    EnableWindow(GetDlgItem(h, IDC_BTN_WRITE), FALSE);
    EnableWindow(GetDlgItem(h, IDC_BTN_VERIFY), FALSE);
    EnableWindow(GetDlgItem(h, IDC_BTN_BROWSE), FALSE);
    EnableWindow(GetDlgItem(h, IDC_BTN_HASHGEN), FALSE);
    EnableWindow(GetDlgItem(h, IDC_BTN_HASHCOPY), FALSE);
    EnableWindow(GetDlgItem(h, IDC_COMBO_DEVICE), FALSE);
    EnableWindow(GetDlgItem(h, IDC_COMBO_HASH), FALSE);
    EnableWindow(GetDlgItem(h, IDC_CHK_PARTITIONS), FALSE);
    EnableWindow(GetDlgItem(h, IDC_EDIT_FILE), FALSE);
    SendDlgItemMessageW(h, IDC_PROGRESS, PBM_SETPOS, 0, 0);
    SetDlgItemTextW(h, IDC_STATIC_PERCENT, L"0%");
    SendMessageW(g_status, SB_SETTEXTW, 1 | SBT_NOBORDERS, (LPARAM)L"");
}

static void EndOp(HWND h)
{
    EnableWindow(GetDlgItem(h, IDC_BTN_CANCEL), FALSE);
    EnableWindow(GetDlgItem(h, IDC_COMBO_DEVICE), TRUE);
    EnableWindow(GetDlgItem(h, IDC_COMBO_HASH), TRUE);
    EnableWindow(GetDlgItem(h, IDC_CHK_PARTITIONS), TRUE);
    EnableWindow(GetDlgItem(h, IDC_EDIT_FILE), TRUE);
    EnableWindow(GetDlgItem(h, IDC_BTN_BROWSE), TRUE);
    SendDlgItemMessageW(h, IDC_PROGRESS, PBM_SETPOS, 0, 0);
    SetDlgItemTextW(h, IDC_STATIC_PERCENT, L"0%");
    SendMessageW(g_status, SB_SETTEXTW, 1 | SBT_NOBORDERS, (LPARAM)L"");
    SetStatusText(Str(IDS_DONE));
    g_status_id = STATUS_IDLE;
    UpdateButtons(h);
}

static void OnBrowse(HWND h)
{
    std::wstring current = GetFileText(h);
    wchar_t file[MAX_PATH * 4] = L"";
    if (!current.empty())
        wcsncpy(file, current.c_str(), MAX_PATH * 4 - 1);

    std::wstring filter = Str(IDS_FILTER_LABEL);
    filter += L'\0'; filter += L"*.img;*.IMG";
    filter += L'\0'; filter += L"*.*";
    filter += L'\0'; filter += L'\0';

    std::wstring directory = g_home;
    if (!current.empty()) {
        std::wstring full = ResolvePath(current);
        size_t slash = full.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            directory = full.substr(0, slash);
    }
    std::wstring title = Str(IDS_SELECT_IMAGE);

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = h;
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH * 4;
    dialog.lpstrInitialDir = directory.c_str();
    dialog.lpstrTitle = title.c_str();
    dialog.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&dialog))
        return;

    SetDlgItemTextW(h, IDC_EDIT_FILE, file);
    std::wstring full = file;
    size_t slash = full.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        g_home = full.substr(0, slash);
    UpdateHash(h, true);
}

static void OnGenerateHash(HWND h)
{
    int type = (int)SendDlgItemMessageW(h, IDC_COMBO_HASH, CB_GETCURSEL, 0, 0);
    if (type <= 0)
        return;

    std::wstring path = ResolvePath(GetFileText(h));
    SetDlgItemTextW(h, IDC_EDIT_HASH, Str(IDS_GENERATING).c_str());
    UpdateWindow(GetDlgItem(h, IDC_EDIT_HASH));
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    std::wstring digest;
    bool ok = HashFile(path, type - 1, digest);
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    SetDlgItemTextW(h, IDC_EDIT_HASH, ok ? digest.c_str() : L"");
    UpdateButtons(h);
}

static void OnCopyHash(HWND h)
{
    wchar_t digest[1024] = L"";
    GetDlgItemTextW(h, IDC_EDIT_HASH, digest, 1024);
    if (!digest[0])
        return;
    if (!OpenClipboard(h))
        return;
    EmptyClipboard();
    size_t bytes = (wcslen(digest) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void *dst = GlobalLock(mem);
        if (dst) {
            memcpy(dst, digest, bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

static void OnRead(HWND h)
{
    std::wstring path = GetFileText(h);
    if (path.empty()) {
        ShowError(h, IDS_FILE_INFO, IDS_SPECIFY_SAVE);
        return;
    }
    path = ResolvePath(path);
    int letter = SelectedVolume(h);
    if (letter < 0)
        return;
    if (SameDrive(path, letter)) {
        ShowError(h, IDS_WRITE_ERROR, IDS_IMAGE_ON_DEVICE);
        return;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (MessageBoxW(h, Str(IDS_READ_OVERWRITE).c_str(), Str(IDS_CONFIRM_OVERWRITE).c_str(),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }

    BeginOp(h);
    g_status_id = STATUS_READING;
    bool ok = ReadImage(path, letter);
    int result = g_status_id;
    EndOp(h);
    if (result == STATUS_EXIT) {
        EndDialog(h, 0);
        return;
    }
    if (ok)
        ShowInfo(h, IDS_COMPLETE, IDS_READ_OK);
    else if (result == STATUS_CANCELED)
        ShowInfo(h, IDS_COMPLETE, IDS_READ_CANCELED);
    UpdateHash(h, true);
}

static bool ValidateImageFile(HWND h, std::wstring &path)
{
    path = ResolvePath(GetFileText(h));
    if (path.empty()) {
        ShowError(h, IDS_FILE_ERROR, IDS_SPECIFY_IMAGE);
        return false;
    }
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ShowError(h, IDS_FILE_ERROR, IDS_FILE_MISSING);
        return false;
    }
    ULONGLONG size = 0;
    bool exists = false;
    if (!FileReadable(path, &size, &exists)) {
        ShowError(h, IDS_FILE_ERROR, IDS_NO_PERMISSION);
        return false;
    }
    if (size == 0) {
        ShowError(h, IDS_FILE_ERROR, IDS_EMPTY_FILE);
        return false;
    }
    return true;
}

static void OnWrite(HWND h)
{
    std::wstring path;
    if (!ValidateImageFile(h, path))
        return;
    int letter = SelectedVolume(h);
    if (letter < 0)
        return;
    if (SameDrive(path, letter)) {
        ShowError(h, IDS_WRITE_ERROR, IDS_IMAGE_ON_DEVICE);
        return;
    }

    wchar_t device[32] = L"";
    GetDlgItemTextW(h, IDC_COMBO_DEVICE, device, 32);
    wchar_t label[MAX_PATH + 1] = L"";
    DriveLabel(letter, label, MAX_PATH);

    wchar_t prompt[1024];
    swprintf(prompt, 1024, Str(IDS_WRITE_CONFIRM).c_str(), device, label);
    if (MessageBoxW(h, prompt, Str(IDS_CONFIRM_OVERWRITE).c_str(),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    BeginOp(h);
    g_status_id = STATUS_WRITING;
    bool ok = WriteImage(path, letter);
    int result = g_status_id;
    EndOp(h);
    if (result == STATUS_EXIT) {
        EndDialog(h, 0);
        return;
    }
    if (ok)
        ShowInfo(h, IDS_COMPLETE, IDS_WRITE_OK);
}

static void OnVerify(HWND h)
{
    std::wstring path;
    if (!ValidateImageFile(h, path))
        return;
    int letter = SelectedVolume(h);
    if (letter < 0)
        return;
    if (SameDrive(path, letter)) {
        ShowError(h, IDS_VERIFY_ERROR, IDS_IMAGE_ON_DEVICE);
        return;
    }

    BeginOp(h);
    g_status_id = STATUS_VERIFYING;
    bool ok = VerifyImage(path, letter);
    int result = g_status_id;
    EndOp(h);
    if (result == STATUS_EXIT) {
        EndDialog(h, 0);
        return;
    }
    if (ok)
        ShowInfo(h, IDS_COMPLETE, IDS_VERIFY_OK);
}

static void OnCancel(HWND h)
{
    if (g_status_id == STATUS_READING || g_status_id == STATUS_WRITING) {
        if (MessageBoxW(h, Str(IDS_CANCEL_CORRUPT).c_str(), Str(IDS_CANCEL_TITLE).c_str(),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            g_status_id = STATUS_CANCELED;
    } else if (g_status_id == STATUS_VERIFYING) {
        if (MessageBoxW(h, Str(IDS_CANCEL_VERIFY).c_str(), Str(IDS_CANCEL_TITLE).c_str(),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            g_status_id = STATUS_CANCELED;
    }
}

static void OnClose(HWND h)
{
    if (g_status_id == STATUS_READING || g_status_id == STATUS_WRITING ||
        g_status_id == STATUS_VERIFYING) {
        UINT message = g_status_id == STATUS_READING ? IDS_EXIT_READ :
                       g_status_id == STATUS_WRITING ? IDS_EXIT_WRITE : IDS_EXIT_VERIFY;
        if (MessageBoxW(h, Str(message).c_str(), Str(IDS_EXIT_TITLE).c_str(),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
            g_status_id = STATUS_EXIT;
        return;
    }
    EndDialog(h, 0);
}

static void LoadSettings()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Win32DiskImager\\Settings", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return;
    wchar_t buffer[1024] = L"";
    DWORD size = sizeof(buffer), type = 0;
    if (RegQueryValueExW(key, L"ImageDir", NULL, &type, (BYTE *)buffer, &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        buffer[1023] = 0;
        g_home = buffer;
    }
    RegCloseKey(key);
}

static void SaveSettings()
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Win32DiskImager\\Settings", 0, NULL,
                        0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, L"ImageDir", 0, REG_SZ, (const BYTE *)g_home.c_str(),
                   (DWORD)((g_home.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

static void InitHome()
{
    static const GUID downloads = {
        0x374de290, 0x123f, 0x4565, { 0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b }
    };
    PWSTR path = NULL;
    if (SHGetKnownFolderPath(downloads, 0, NULL, &path) == S_OK && path) {
        g_home = path;
        CoTaskMemFree(path);
    } else {
        wchar_t buffer[1024] = L"";
        DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, 1024);
        if (length && length < 1024)
            g_home = buffer;
    }
}

static WNDPROC g_edit_proc;

static LRESULT CALLBACK EditProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_DROPFILES) {
        SendMessageW(GetParent(h), WM_DROPFILES, wparam, lparam);
        return 0;
    }
    return CallWindowProcW(g_edit_proc, h, message, wparam, lparam);
}

static void InitDialog(HWND h)
{
    g_hwnd = h;
    SetWindowTextW(h, Str(IDS_TITLE).c_str());
    SetDlgItemTextW(h, IDC_STATIC_FILE, Str(IDS_GROUP_FILE).c_str());
    SetDlgItemTextW(h, IDC_STATIC_DEVICE, Str(IDS_GROUP_DEVICE).c_str());
    SetDlgItemTextW(h, IDC_STATIC_HASH, Str(IDS_GROUP_HASH).c_str());
    SetDlgItemTextW(h, IDC_STATIC_PROGRESS, Str(IDS_GROUP_PROGRESS).c_str());
    SetDlgItemTextW(h, IDC_CHK_PARTITIONS, Str(IDS_CHK_PARTITIONS).c_str());
    SetDlgItemTextW(h, IDC_BTN_HASHGEN, Str(IDS_HASH_GENERATE).c_str());
    SetDlgItemTextW(h, IDC_BTN_HASHCOPY, Str(IDS_HASH_COPY).c_str());
    SetDlgItemTextW(h, IDC_BTN_CANCEL, Str(IDS_BTN_CANCEL).c_str());
    SetDlgItemTextW(h, IDC_BTN_READ, Str(IDS_BTN_READ).c_str());
    SetDlgItemTextW(h, IDC_BTN_WRITE, Str(IDS_BTN_WRITE).c_str());
    SetDlgItemTextW(h, IDC_BTN_VERIFY, Str(IDS_BTN_VERIFY).c_str());
    SetDlgItemTextW(h, IDC_BTN_EXIT, Str(IDS_BTN_EXIT).c_str());
    SetDlgItemTextW(h, IDC_BTN_BROWSE, L"...");

    HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON small = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small);

    SendDlgItemMessageW(h, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
    SendDlgItemMessageW(h, IDC_PROGRESS, PBM_SETPOS, 0, 0);

    CreateStatusBar(h);
    Layout(h);

    std::wstring none = Str(IDS_HASH_NONE);
    SendDlgItemMessageW(h, IDC_COMBO_HASH, CB_ADDSTRING, 0, (LPARAM)none.c_str());
    SendDlgItemMessageW(h, IDC_COMBO_HASH, CB_ADDSTRING, 0, (LPARAM)L"MD5");
    SendDlgItemMessageW(h, IDC_COMBO_HASH, CB_ADDSTRING, 0, (LPARAM)L"SHA1");
    SendDlgItemMessageW(h, IDC_COMBO_HASH, CB_ADDSTRING, 0, (LPARAM)L"SHA256");
    SendDlgItemMessageW(h, IDC_COMBO_HASH, CB_SETCURSEL, 0, 0);

    RefreshDevices(h);

    if (!g_start_file.empty())
        SetDlgItemTextW(h, IDC_EDIT_FILE, g_start_file.c_str());
    UpdateHash(h, true);
    EnableWindow(GetDlgItem(h, IDC_BTN_CANCEL), FALSE);
    SetStatusText(Str(IDS_WAITING));

    CenterWindow(h);
    SetFocus(GetDlgItem(h, IDC_EDIT_FILE));
    DragAcceptFiles(h, TRUE);
    HWND edit = GetDlgItem(h, IDC_EDIT_FILE);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    DragAcceptFiles(edit, TRUE);
}

static INT_PTR CALLBACK MainProc(HWND h, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG:
        InitDialog(h);
        return FALSE;

    case WM_SIZE:
        Layout(h);
        return TRUE;

    case WM_COMMAND: {
        int id = LOWORD(wparam);
        int code = HIWORD(wparam);
        switch (id) {
        case IDOK:
            return TRUE;
        case IDCANCEL:
            OnClose(h);
            return TRUE;
        case IDC_BTN_BROWSE:
            if (code == BN_CLICKED) OnBrowse(h);
            return TRUE;
        case IDC_BTN_HASHGEN:
            if (code == BN_CLICKED) OnGenerateHash(h);
            return TRUE;
        case IDC_BTN_HASHCOPY:
            if (code == BN_CLICKED) OnCopyHash(h);
            return TRUE;
        case IDC_BTN_READ:
            if (code == BN_CLICKED) OnRead(h);
            return TRUE;
        case IDC_BTN_WRITE:
            if (code == BN_CLICKED) OnWrite(h);
            return TRUE;
        case IDC_BTN_VERIFY:
            if (code == BN_CLICKED) OnVerify(h);
            return TRUE;
        case IDC_BTN_CANCEL:
            if (code == BN_CLICKED) OnCancel(h);
            return TRUE;
        case IDC_BTN_EXIT:
            if (code == BN_CLICKED) SendMessageW(h, WM_CLOSE, 0, 0);
            return TRUE;
        case IDC_EDIT_FILE:
            if (code == EN_CHANGE) UpdateHash(h, true);
            return TRUE;
        case IDC_COMBO_HASH:
            if (code == CBN_SELCHANGE) UpdateHash(h, true);
            return TRUE;
        case IDC_COMBO_DEVICE:
            if (code == CBN_SELCHANGE) UpdateButtons(h);
            return TRUE;
        }
        return FALSE;
    }

    case WM_CLOSE:
        OnClose(h);
        return TRUE;

    case WM_DESTROY:
        SaveSettings();
        return TRUE;

    case WM_DEVICECHANGE:
        if ((wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) &&
            g_status_id == STATUS_IDLE) {
            DEV_BROADCAST_HDR *header = (DEV_BROADCAST_HDR *)lparam;
            if (header && header->dbch_devicetype == DBT_DEVTYP_VOLUME)
                RefreshDevices(h);
        }
        return TRUE;

    case WM_DROPFILES: {
        wchar_t path[MAX_PATH * 4] = L"";
        if (DragQueryFileW((HDROP)wparam, 0, path, MAX_PATH * 4)) {
            SetDlgItemTextW(h, IDC_EDIT_FILE, path);
            UpdateHash(h, true);
        }
        DragFinish((HDROP)wparam);
        return TRUE;
    }
    }
    return FALSE;
}

static LANGID PickLanguage(const wchar_t *arg)
{
    if (arg) {
        if (!_wcsicmp(arg, L"zh") || !_wcsicmp(arg, L"zh-cn") || !_wcsicmp(arg, L"zh_cn") ||
            !_wcsicmp(arg, L"804"))
            return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
        if (!_wcsicmp(arg, L"en") || !_wcsicmp(arg, L"en-us") || !_wcsicmp(arg, L"409"))
            return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    }
    LANGID system = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(system) == LANG_CHINESE)
        return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
    return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g_inst = instance;

    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&controls);

    const wchar_t *lang_arg = NULL;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (!_wcsicmp(argv[i], L"--lang") && i + 1 < argc)
                lang_arg = argv[++i];
            else if (!_wcsnicmp(argv[i], L"--lang=", 7))
                lang_arg = argv[i] + 7;
            else if (g_start_file.empty())
                g_start_file = argv[i];
        }
        LocalFree(argv);
    }
    g_lang = PickLanguage(lang_arg);
    SetThreadUILanguage(g_lang);

    LoadSettings();
    if (g_home.empty())
        InitHome();

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainProc, 0);
    return 0;
}
