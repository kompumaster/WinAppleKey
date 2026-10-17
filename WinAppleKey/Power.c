#include "Driver.h"

///////////////////////////////////////////////////////////////////////////////
// Fn+F5 -> system sleep, Fn+F6 -> hibernate.
//
// Neither action can be expressed as a HID usage that Windows understands, so
// the driver has to initiate the power transition itself:
//
//   * NtSetSystemPowerState() is exported by ntoskrnl but is not declared in
//     the WDK headers, so it is resolved with MmGetSystemRoutineAddress().
//   * It requires PASSIVE_LEVEL, while the key reports are parsed in an I/O
//     completion routine that may run at DISPATCH_LEVEL, so the call is
//     deferred to a system worker thread.
//   * A legacy WORK_QUEUE_ITEM is used on purpose: IoAllocateWorkItem() would
//     take a reference on a device object and hold it until IoFreeWorkItem(),
//     which would get in the way of removing that device.
///////////////////////////////////////////////////////////////////////////////

// NtSetSystemPowerState is NOT exported by ntoskrnl (verified with
// "link /dump /exports"), the exported entry point for initiating a power
// transition is ZwInitiatePowerAction:
//
//   NTSTATUS ZwInitiatePowerAction(POWER_ACTION SystemAction,
//                                  SYSTEM_POWER_STATE MinSystemState,
//                                  ULONG Flags, BOOLEAN Asynchronous);
typedef NTSTATUS(*PFN_INITIATE_POWER_ACTION)(POWER_ACTION SystemAction, SYSTEM_POWER_STATE MinSystemState, ULONG Flags, BOOLEAN Asynchronous);

static PFN_INITIATE_POWER_ACTION g_InitiatePowerAction = NULL;
static WORK_QUEUE_ITEM g_PowerWorkItem;
static LONG g_PowerWorkItemQueued = 0;
static LONG g_PendingPowerAction = (LONG)PowerActionNone;

static VOID PowerWorkItemRoutine(PVOID Context)
{
	POWER_ACTION action;
	SYSTEM_POWER_STATE minState;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(Context);

	action = (POWER_ACTION)InterlockedExchange(&g_PendingPowerAction, (LONG)PowerActionNone);
	InterlockedExchange(&g_PowerWorkItemQueued, 0);

	if (action == PowerActionNone || !g_InitiatePowerAction)
		return;

	minState = (action == PowerActionHibernate) ? PowerSystemHibernate : PowerSystemSleeping1;

	// Note: fails harmlessly (and changes nothing) when hibernation is disabled.
	status = g_InitiatePowerAction(action, minState, 0, TRUE);

	if (!NT_SUCCESS(status))
		DebugPrint("Power: ZwInitiatePowerAction(0x%x) failed 0x%x\n", action, status);
}

VOID PowerInitialize(VOID)
{
	UNICODE_STRING name;

	RtlInitUnicodeString(&name, L"ZwInitiatePowerAction");
	g_InitiatePowerAction = (PFN_INITIATE_POWER_ACTION)MmGetSystemRoutineAddress(&name);

	ExInitializeWorkItem(&g_PowerWorkItem, PowerWorkItemRoutine, NULL);
}

// Must only be called on the key-down transition: a HID report is level
// triggered, so every report while the key is held would otherwise sleep the
// machine again right after it wakes up.
VOID WinAppleKeyRequestPowerAction(POWER_ACTION action)
{
	if (action == PowerActionNone || !g_InitiatePowerAction)
		return;

	InterlockedExchange(&g_PendingPowerAction, (LONG)action);

	// Guard against re-queueing the (non re-entrant) WORK_QUEUE_ITEM.
	if (InterlockedCompareExchange(&g_PowerWorkItemQueued, 1, 0) == 0)
		ExQueueWorkItem(&g_PowerWorkItem, DelayedWorkQueue);
}
