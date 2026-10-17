// DeviceTool.exe - creates and removes the root-enumerated device that hosts
// the virtual HID device.
//
// The installer needs this because pnputil can only install a driver onto a
// device that already exists, and there is no built-in way to create a root
// node. devcon does it, but bundling a WDK tool in a package is a licensing
// question I'd rather not have, so this is the 200 lines that matter.

#include <windows.h>
#include <winsvc.h>
#include <setupapi.h>
#include <newdev.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")

// {745a17a0-74d3-11d0-b6fe-00a0c90f57da}
static const GUID HIDClassGuid =
    { 0x745a17a0, 0x74d3, 0x11d0, { 0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda } };

static void Usage(void)
{
	fwprintf(stderr,
		L"Usage:\n"
		L"  DeviceTool create <hardware-id> [inf-path]\n"
		L"  DeviceTool remove <hardware-id>\n"
		L"  DeviceTool cleanup <service-name> [file-name]\n"
		L"\n"
		L"Example:\n"
		L"  DeviceTool create root\\WinAppleKeyVhf C:\\Program Files\\WinAppleKey\\WinAppleKey.inf\n");
}

static void PrintError(const WCHAR *what, DWORD code)
{
	WCHAR buffer[512] = L"";
	HMODULE module;

	buffer[0] = 0;
	module = LoadLibraryW(L"setupapi.dll");
	if (module)
	{
		FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
			module, code, 0, buffer, ARRAYSIZE(buffer), NULL);
		FreeLibrary(module);
	}
	if (!buffer[0])
		FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL, code, 0, buffer, ARRAYSIZE(buffer), NULL);

	// Trim the trailing newline FormatMessage likes to add.
	size_t len = wcslen(buffer);
	while (len > 0 && (buffer[len - 1] == L'\r' || buffer[len - 1] == L'\n'))
		buffer[--len] = 0;

	fwprintf(stderr, L"%s failed: %lu", what, code);
	if (buffer[0])
		fwprintf(stderr, L" (%s)", buffer);
	fwprintf(stderr, L"\n");
}

// Compares a REG_MULTI_SZ hardware ID list against a single ID, ignoring case.
static BOOL ListContainsId(const WCHAR *list, DWORD bytes, const WCHAR *id)
{
	const WCHAR *p = list;
	const WCHAR *end = (const WCHAR *)((const BYTE *)list + bytes);

	while (p < end && *p)
	{
		if (_wcsicmp(p, id) == 0)
			return TRUE;
		p += wcslen(p) + 1;
	}
	return FALSE;
}

static int CreateDevice(const WCHAR *hwid, const WCHAR *infPath)
{
	HDEVINFO set;
	SP_DEVINFO_DATA devInfo;
	WCHAR hwidList[512];
	DWORD hwidBytes;
	BOOL reboot = FALSE;
	DWORD error;
	const WCHAR *name = hwid;

	// SetupDiCreateDeviceInfo wants the enumerator name on its own: given
	// "WinAppleKeyVhf" with DICD_GENERATE_ID the system builds the instance ID
	// ROOT\WINAPPLEKEYVHF\0000. Passing the full "root\..." ID here fails with
	// 0xE0000205, which is not the most helpful error message in the world.
	if (_wcsnicmp(name, L"root\\", 5) == 0)
		name += 5;

	ZeroMemory(&devInfo, sizeof(devInfo));
	devInfo.cbSize = sizeof(devInfo);

	set = SetupDiCreateDeviceInfoList(&HIDClassGuid, NULL);
	if (set == INVALID_HANDLE_VALUE)
	{
		PrintError(L"SetupDiCreateDeviceInfoList", GetLastError());
		return 1;
	}

	if (!SetupDiCreateDeviceInfoW(set, name, &HIDClassGuid, NULL, NULL,
			DICD_GENERATE_ID, &devInfo))
	{
		PrintError(L"SetupDiCreateDeviceInfo", GetLastError());
		SetupDiDestroyDeviceInfoList(set);
		return 1;
	}

	wcscpy_s(hwidList, ARRAYSIZE(hwidList), hwid);
	hwidBytes = (DWORD)((wcslen(hwid) + 2) * sizeof(WCHAR)); // string + its NUL + list NUL

	if (!SetupDiSetDeviceRegistryPropertyW(set, &devInfo, SPDRP_HARDWAREID,
			(const BYTE *)hwidList, hwidBytes))
	{
		PrintError(L"Setting the hardware ID", GetLastError());
		SetupDiDestroyDeviceInfoList(set);
		return 1;
	}

	if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &devInfo))
	{
		PrintError(L"DIF_REGISTERDEVICE", GetLastError());
		SetupDiDestroyDeviceInfoList(set);
		return 1;
	}

	SetupDiDestroyDeviceInfoList(set);

	// Bind the driver. PnP would usually find the package in the driver store
	// on its own, but doing it explicitly is what devcon does and it gives a
	// useful error instead of a "device has no driver" warning in Device
	// Manager.
	if (infPath && *infPath)
	{
		if (!UpdateDriverForPlugAndPlayDevicesW(NULL, hwid, infPath,
				INSTALLFLAG_FORCE, &reboot))
		{
			error = GetLastError();
			fwprintf(stderr, L"Driver installation failed for %s\n", infPath);
			PrintError(L"UpdateDriverForPlugAndPlayDevices", error);
			return 1;
		}
	}

	wprintf(L"Device %s created.\n", hwid);
	return 0;
}

static int RemoveDevice(const WCHAR *hwid)
{
	HDEVINFO set;
	SP_DEVINFO_DATA devInfo;
	DWORD index = 0;
	int removed = 0;
	int failed = 0;

	ZeroMemory(&devInfo, sizeof(devInfo));
	devInfo.cbSize = sizeof(devInfo);

	set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE)
	{
		fwprintf(stderr, L"SetupDiGetClassDevs failed: %lu\n", GetLastError());
		return 1;
	}

	for (index = 0; SetupDiEnumDeviceInfo(set, index, &devInfo); index++)
	{
		BYTE buffer[1024];
		DWORD bytes = 0;
		DWORD type = 0;

		if (!SetupDiGetDeviceRegistryPropertyW(set, &devInfo, SPDRP_HARDWAREID,
				&type, buffer, sizeof(buffer), &bytes))
			continue;
		if (type != REG_MULTI_SZ)
			continue;
		if (!ListContainsId((const WCHAR *)buffer, bytes, hwid))
			continue;

		if (SetupDiSetSelectedDevice(set, &devInfo) &&
			SetupDiCallClassInstaller(DIF_REMOVE, set, &devInfo))
		{
			wprintf(L"Device %s removed.\n", hwid);
			removed++;
		}
		else
		{
			fwprintf(stderr, L"Could not remove the device (error %lu).\n", GetLastError());
			failed++;
		}

		// DIF_REMOVE invalidates the enumeration; start over.
		SetupDiDestroyDeviceInfoList(set);
		set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
		if (set == INVALID_HANDLE_VALUE)
			break;
		index = (DWORD)-1;
	}

	SetupDiDestroyDeviceInfoList(set);

	if (failed)
		return 1;
	if (!removed)
		wprintf(L"No device with hardware ID %s was found.\n", hwid);
	return 0;
}

// Deletes the driver's service and the copy of the .sys that the INF put into
// System32\drivers. DIFxApp removes the driver package but leaves both behind.
//
// This has to run at the very end of the uninstall. A filter driver that is
// still bound to a device cannot be stopped - the call simply does not return -
// which is why the MSI does not use ServiceControl with Stop here.
static int CleanUp(const WCHAR *serviceName, const WCHAR *fileName)
{
	SC_HANDLE scm;
	SC_HANDLE service;
	SERVICE_STATUS status;
	WCHAR sysDir[MAX_PATH];
	WCHAR path[MAX_PATH];

	scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (scm)
	{
		service = OpenServiceW(scm, serviceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
		if (service)
		{
			DWORD error;
			int i;

			ControlService(service, SERVICE_CONTROL_STOP, &status); // best effort

			for (i = 0; i < 30; i++)
			{
				if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED)
					break;
				Sleep(100);
			}

			if (DeleteService(service))
				wprintf(L"Service %s deleted.\n", serviceName);
			else
			{
				error = GetLastError();
				if (error == ERROR_SERVICE_MARKED_FOR_DELETE)
					wprintf(L"Service %s is already marked for deletion.\n", serviceName);
				else
					PrintError(L"DeleteService", error);
			}

			CloseServiceHandle(service);
		}
		else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
			PrintError(L"OpenService", GetLastError());

		CloseServiceHandle(scm);
	}
	else
		PrintError(L"OpenSCManager", GetLastError());

	if (!GetSystemDirectoryW(sysDir, ARRAYSIZE(sysDir)))
	{
		PrintError(L"GetSystemDirectory", GetLastError());
		return 1;
	}

	swprintf_s(path, ARRAYSIZE(path), L"%s\\drivers\\%s", sysDir, fileName);

	// PnP re-enumerates the keyboard asynchronously after the package is
	// removed, so the file can still be mapped for a moment even though
	// DIFxApp is done. Retry briefly instead of giving up on the first try.
	{
		BOOL deleted = FALSE;
		BOOL missing = FALSE;
		int i;

		for (i = 0; i < 12; i++)
		{
			if (DeleteFileW(path))
			{
				deleted = TRUE;
				break;
			}
			if (GetLastError() == ERROR_FILE_NOT_FOUND)
			{
				missing = TRUE;
				break;
			}
			Sleep(150);
		}

		if (deleted)
			wprintf(L"%s deleted.\n", path);
		else if (missing)
			wprintf(L"%s was already gone.\n", path);
		else
		{
			// Do NOT schedule this with MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT):
			// during an upgrade the old product is removed while the driver is
			// still loaded, and a pending delete of this path would then hit the
			// file the new product has just installed. A leftover file is
			// harmless - nothing loads it once the service and the package are
			// gone - and the next install overwrites it.
			wprintf(L"%s is still in use, leaving it in place.\n", path);
		}
	}

	return 0;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 3)
	{
		Usage();
		return 2;
	}

	if (_wcsicmp(argv[1], L"create") == 0)
		return CreateDevice(argv[2], (argc > 3) ? argv[3] : NULL);

	if (_wcsicmp(argv[1], L"remove") == 0)
		return RemoveDevice(argv[2]);

	if (_wcsicmp(argv[1], L"cleanup") == 0)
		return CleanUp(argv[2], (argc > 3) ? argv[3] : L"WinAppleKey.sys");

	Usage();
	return 2;
}
