#include "Driver.h"

// vhf.h selects VHF_CONFIG.DeviceObject vs FileHandle on _KERNEL_MODE, and
// VHF_CONFIG_INIT() fills the matching field. Make sure the kernel-mode branch
// is taken.
#ifndef _KERNEL_MODE
#define _KERNEL_MODE 1
#endif
#include <vhf.h>

///////////////////////////////////////////////////////////////////////////////
// Virtual HID device (Virtual HID Framework client)
//
// The A1314's own report descriptor declares no Volume/Mute usages, and Windows
// does not translate the Keyboard/Keypad-page volume usages either, so the only
// way to produce real system volume keys from kernel mode is to expose an extra
// HID device of our own whose report descriptor we control.
//
// The driver is additionally installed as a root-enumerated device
// (root\WinAppleKeyVhf). AddDevice() recognises it, creates an FDO, attaches it
// (VhfCreate() locates vhf.sys with IoGetLowerDeviceObject(), so attaching is
// mandatory) and calls VhfCreate()/VhfStart().
//
// TEARDOWN MATTERS: vhf.sys is a KMDF lower filter, so VhfDelete() must run
// while that filter is still alive - i.e. *before* IRP_MN_REMOVE_DEVICE is
// forwarded down the stack. Doing it afterwards causes a
// PAGE_FAULT_IN_NONPAGED_AREA inside wdf01000.sys.
///////////////////////////////////////////////////////////////////////////////

// Consumer Control (usage page 0x0C) with Mute / Volume Increment /
// Volume Decrement / Brightness Increment / Brightness Decrement. Bit order
// follows the order of the declarations below.
static UCHAR g_VhfReportDescriptor[] =
{
	0x05, 0x0C, // Usage Page (Consumer)
	0x09, 0x01, // Usage (Consumer Control)
	0xA1, 0x01, // Collection (Application)
	0x85, 0x01, //   Report ID (1)
	0x09, 0xE2, //   Usage (Mute)
	0x09, 0xE9, //   Usage (Volume Increment)
	0x09, 0xEA, //   Usage (Volume Decrement)
	0x09, 0x6F, //   Usage (Brightness Increment)
	0x09, 0x70, //   Usage (Brightness Decrement)
	0x15, 0x00, //   Logical Minimum (0)
	0x25, 0x01, //   Logical Maximum (1)
	0x75, 0x01, //   Report Size (1)
	0x95, 0x05, //   Report Count (5)
	0x81, 0x02, //   Input (Data,Var,Abs)
	0x95, 0x03, //   Report Count (3)
	0x81, 0x03, //   Input (Cnst,Var,Abs)
	0xC0        // End Collection
};

#define VHF_REPORT_ID 1

static VHFHANDLE g_VhfHandle = NULL;
// HID_XFER_PACKET.reportBuffer must start with the report ID byte, followed by
// the report data (same convention as the WDK HID miniport samples, which
// offset into the buffer by sizeof(packet.reportId)).
static UCHAR g_VhfReport[2] = { VHF_REPORT_ID, 0 };
static KSPIN_LOCK g_VhfLock;
static BOOLEAN g_VhfDeleting = FALSE;

VOID VhfInitialize(VOID)
{
	KeInitializeSpinLock(&g_VhfLock);
}

NTSTATUS VhfInit(PDEVICE_OBJECT fdo)
{
	VHF_CONFIG config;
	NTSTATUS status;
	KIRQL irql;
	VHFHANDLE oldHandle;

	PAGED_CODE();

	// The Deleting flag is sticky across a device remove/re-add cycle, so it must
	// be reset here - otherwise volume would stay dead after a device restart.
	KeAcquireSpinLock(&g_VhfLock, &irql);
	oldHandle = g_VhfHandle;
	g_VhfHandle = NULL;
	g_VhfDeleting = FALSE;
	KeReleaseSpinLock(&g_VhfLock, irql);

	// Defensive: a handle still being present means a second VHF host device was
	// created (e.g. a leftover root devnode from a previous install). Delete the
	// old virtual device, otherwise its handle would be leaked.
	if (oldHandle)
		VhfDelete(oldHandle, TRUE);

	VHF_CONFIG_INIT(&config, fdo, (USHORT)sizeof(g_VhfReportDescriptor), g_VhfReportDescriptor);
	config.VendorID = 0x05AC; // Apple
	config.ProductID = 0x0256;
	config.VersionNumber = 0x0001;

	status = VhfCreate(&config, &g_VhfHandle);
	if (!NT_SUCCESS(status))
	{
		DebugPrint("VhfInit(): VhfCreate failed 0x%x\n", status);
		g_VhfHandle = NULL;
		return status;
	}

	status = VhfStart(g_VhfHandle);
	if (!NT_SUCCESS(status))
	{
		DebugPrint("VhfInit(): VhfStart failed 0x%x\n", status);
		VhfDelete(g_VhfHandle, TRUE);
		g_VhfHandle = NULL;
	}

	return status;
}

// Must be called while vhf.sys is still present below us (i.e. before the
// IRP_MN_REMOVE_DEVICE request is forwarded down).
VOID VhfDeinit(VOID)
{
	KIRQL irql;
	VHFHANDLE handle;

	PAGED_CODE();

	KeAcquireSpinLock(&g_VhfLock, &irql);
	handle = g_VhfHandle;
	g_VhfDeleting = TRUE;
	g_VhfHandle = NULL;
	KeReleaseSpinLock(&g_VhfLock, irql);

	if (handle)
		VhfDelete(handle, TRUE);
}

// Level triggered: the caller sends the report with the bits set when the key
// goes down and once more with them cleared when it is released.
VOID VhfSendReport(UCHAR bits)
{
	HID_XFER_PACKET packet;
	KIRQL irql;
	NTSTATUS status;

	// Taking the lock for the whole call is safe: VhfReadReportSubmit() only
	// queues the report and never blocks.
	KeAcquireSpinLock(&g_VhfLock, &irql);

	if (!g_VhfHandle || g_VhfDeleting)
	{
		KeReleaseSpinLock(&g_VhfLock, irql);
		return;
	}

	g_VhfReport[0] = VHF_REPORT_ID;
	g_VhfReport[1] = bits;

	packet.reportBuffer = g_VhfReport;
	packet.reportBufferLen = sizeof(g_VhfReport);
	packet.reportId = VHF_REPORT_ID;

	status = VhfReadReportSubmit(g_VhfHandle, &packet);

	KeReleaseSpinLock(&g_VhfLock, irql);

	if (!NT_SUCCESS(status))
		DebugPrint("VhfSendReport(): VhfReadReportSubmit failed 0x%x\n", status);
}
