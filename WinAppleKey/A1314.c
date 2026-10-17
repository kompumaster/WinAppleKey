#include "driver.h"

// Apple Wireless Keyboard (A1314), connected over Bluetooth.
//
// Unlike the Magic Keyboard (A1644), which reports fixed 11-byte input
// reports, the A1314 emits two different kinds of L2CAP ACL transfers on
// the Bluetooth HID interrupt channel:
//
//   * 3-byte  "Apple vendor" special key reports. Only the low bits of the
//             last byte are used here: 0x8 = Eject, 0x10 = Fn.
//   * 10-byte regular keyboard reports ("A1 01 <modifier> <reserved>
//             <key1>..<key6>").
//
// Both are rewritten in place into a well formed 10-byte keyboard report so
// that HIDCLASS/KBDCLASS can process them further up the stack. This also
// guarantees that KdPrintBuffer()/logic below never sees a stale buffer.

static BOOLEAN FakeFnActive = 0;
static BOOLEAN RightCmdAsMenu = FALSE;
static UCHAR ConsumerBits = 0;
static UCHAR ConsumerBitsPrev = 0;
static UCHAR VhfBits = 0;
static UCHAR VhfBitsPrev = 0;
static POWER_ACTION PowerAction = PowerActionNone;
static POWER_ACTION PowerActionPrev = PowerActionNone;
static UCHAR TmpBuf[10] = { 0 };
static UCHAR SpecialKey = 0;
static UCHAR SpecialKeyModifier = 0;

// Media keys are reported through the device's own Consumer Control collection
// (usage page 0x0C). The A1314 declares it as report 0x12 with its buttons in
// this order:
//   bit 0 = 0xCD Play/Pause, bit 1 = 0xB3 Fast Forward, bit 2 = 0xB4 Rewind,
//   bit 3 = 0xB5 Scan Next Track, bit 4 = 0xB6 Scan Previous Track
// The collection has no Volume/Mute usages, so those cannot be produced this
// way and need the (todo) virtual HID device.
#define ConsumerReportId 0x12
#define ConsumerPlayPause 0x01
#define ConsumerScanNext 0x08
#define ConsumerScanPrev 0x10

void ProcessA1314Block(PBRB pbrb)
{
	PUCHAR buf = (PUCHAR)pbrb->BrbL2caAclTransfer.Buffer;
	ULONG size = pbrb->BrbL2caAclTransfer.BufferSize;
	UCHAR modifiers;
	UCHAR menuMask;

	DebugPrintBuffer("ProcessA1314Block(): <= ", buf, size);

	if (size == 3) // Apple vendor special key report (Eject/Fn)
	{
		SpecialKey = 0;
		SpecialKeyModifier = 0;

		if (buf[2] & 0x8) // Eject pressed
			SpecialKey = HidDel; // Map to Del

		if (g_dwSwapFnCtrl)
		{
			if (buf[2] & 0x10) // Fn pressed
				SpecialKeyModifier |= HidLCtrlMask; // Map to LCtrl
		}
		else
			FakeFnActive = buf[2] & 0x10; // Set FakeFn state from the physical Fn state
	}
	else if (size == 10) // Regular keyboard report
	{
		RtlCopyMemory(TmpBuf, buf, 10);
		ConsumerBits = 0;
		VhfBits = 0;
		PowerAction = PowerActionNone;

		// On the right side one of the two modifiers acts as the Application
		// (context menu) key and the other one stays Right Alt:
		//   SwapRightCmdAlt=0 -> Right Command = menu, Right Option = Alt (default)
		//   SwapRightCmdAlt=1 -> Right Command = Alt,  Right Option = menu
		menuMask = g_dwSwapRightCmdAlt ? HidRAltMask : HidRCmdMask;
		RightCmdAsMenu = (g_dwRightCmdAsMenu && (TmpBuf[2] & menuMask)) != 0;
		if (RightCmdAsMenu)
			TmpBuf[2] &= ~menuMask; // Clear the modifier bit

		if (g_dwSwapFnCtrl)
		{
			// Physical LCtrl pressed (and alone) drives the fake Fn state
			if ((TmpBuf[2] & HidLCtrlMask) && !TmpBuf[4])
				FakeFnActive = TRUE;
			else if (!TmpBuf[4])
				FakeFnActive = FALSE;

			TmpBuf[2] &= ~HidLCtrlMask; // Clear LCtrl modifier
		}

		// Optionally process the Alt-Cmd swap. Left side only: the right-hand
		// modifiers are governed by RightCmdAsMenu / SwapRightCmdAlt below, where
		// one of them is the Application key and the other one stays Right Alt.
		if (g_dwSwapAltCmd)
		{
			if (TmpBuf[2] & HidLAltMask)
			{
				TmpBuf[2] &= ~HidLAltMask;
				TmpBuf[2] |= HidLCmdMask;
			}
			else if (TmpBuf[2] & HidLCmdMask)
			{
				TmpBuf[2] &= ~HidLCmdMask;
				TmpBuf[2] |= HidLAltMask;
			}
		}

		// Right Command becomes Alt, unless it has just become the menu key.
		// This has to run *after* the Alt/Cmd swap above: that one turns a left
		// Alt into the Windows key, so converting Right Command into a left Alt
		// any earlier would give you Win+Tab instead of Alt+Tab. Left Alt is also
		// the only one that works - Windows treats Right Alt as AltGr, and the
		// task switcher never sees a VK_RMENU release as "commit".
		if (TmpBuf[2] & HidRCmdMask)
		{
			TmpBuf[2] &= ~HidRCmdMask;
			TmpBuf[2] |= HidLAltMask;
		}

		// Process FakeFn+[key] combination
		if (FakeFnActive && (TmpBuf[4] || TmpBuf[2]))
		{
			switch (TmpBuf[4])
			{
			case HidLeft: TmpBuf[4] = HidHome; break;
			case HidRight: TmpBuf[4] = HidEnd; break;
			case HidUp: TmpBuf[4] = HidPgUp; break;
			case HidDown: TmpBuf[4] = HidPgDown; break;
			case HidEnter: TmpBuf[4] = HidInsert; break;
			// Fn+F1/F2 -> Brightness Decrement/Increment. Windows has no
			// keyboard-page usage for these, so they go to the virtual HID device
			// as Consumer page 0x70/0x6F - which the OS does turn into a brightness
			// change for the internal panel. Verified on Windows 11 24H2.
			case HidF1: VhfBits = VhfBitBrightnessDown; TmpBuf[4] = 0; break;
			case HidF2: VhfBits = VhfBitBrightnessUp; TmpBuf[4] = 0; break;
			case HidF3: TmpBuf[4] = HidTab; TmpBuf[2] |= HidLCmdMask; break; // Win+Tab (Task View)
			case HidF4: TmpBuf[4] = HidKeyZ; TmpBuf[2] |= HidLCmdMask; break; // Win+Z (Snap Layouts)
			case HidF5: PowerAction = PowerActionSleep; TmpBuf[4] = 0; break; // System sleep
			case HidF6: PowerAction = PowerActionHibernate; TmpBuf[4] = 0; break; // Hibernate
			case HidF7: ConsumerBits = ConsumerScanPrev; break; // Scan Previous Track
			case HidF8: ConsumerBits = ConsumerPlayPause; break; // Play/Pause
			case HidF9: ConsumerBits = ConsumerScanNext; break; // Scan Next Track
			case HidF10: VhfBits = VhfBitMute; TmpBuf[4] = 0; break; // Volume Mute
			case HidF11: VhfBits = VhfBitVolumeDown; TmpBuf[4] = 0; break; // Volume Down
			case HidF12: VhfBits = VhfBitVolumeUp; TmpBuf[4] = 0; break; // Volume Up
			case HidKeyP: TmpBuf[4] = HidPrtScr; break;
			case HidKeyB: TmpBuf[4] = HidPauseBreak; break;
			case HidKeyS: TmpBuf[4] = HidScrLck; break;
			default:
				// Map Fn+LCtrl to RCtrl; ignore all other Fn+[key] combinations.
				// NOTE: in SwapFnCtrl mode the logical Left Ctrl (i.e. the physical
				// Fn key) lives in SpecialKeyModifier, not in TmpBuf, because the
				// 10-byte reports keep overwriting TmpBuf - hence the OR below.
				if ((TmpBuf[2] | SpecialKeyModifier) & HidLCtrlMask)
				{
					SpecialKeyModifier &= ~HidLCtrlMask; // Clear the logical LCtrl
					TmpBuf[2] &= ~HidLCtrlMask; // Clear LCtrl
					TmpBuf[2] |= HidRCtrlMask; // Set RCtrl
				}
				else
					RtlZeroMemory(TmpBuf + 2, sizeof(TmpBuf) - 2);
				break;
			}
		}
	}

	// Volume/Mute go to the virtual HID device (see Vhf.c). Level triggered, so
	// the cleared state is submitted on release as well.
	if (VhfBits || VhfBitsPrev)
	{
		if (VhfBits != VhfBitsPrev)
			VhfSendReport(VhfBits);

		VhfBitsPrev = VhfBits;
	}

	// Fn+F5 / Fn+F6 -> system sleep / hibernate. Edge triggered, see Power.c.
	if (PowerAction != PowerActionPrev)
	{
		if (PowerAction != PowerActionNone)
			WinAppleKeyRequestPowerAction(PowerAction);

		PowerActionPrev = PowerAction;
	}

	// The media keys are sent as a Consumer Control report instead of the usual
	// keyboard report. HID input reports are level triggered, not events, so the
	// previous state is re-sent (with the bits cleared) on release - otherwise
	// Windows would never see the button go up.
	if (ConsumerBits || ConsumerBitsPrev)
	{
		pbrb->BrbL2caAclTransfer.BufferSize = 3;
		buf[0] = 0xa1;
		buf[1] = ConsumerReportId;
		buf[2] = ConsumerBits;

		ConsumerBitsPrev = ConsumerBits;

		DebugPrintBuffer("ProcessA1314Block(): => consumer ", buf, 3);
		return;
	}

	// Always emit a well formed 10-byte keyboard report upstream.
	// NOTE: hidbth allocates the interrupt channel buffer using the largest
	// input report of the device (10 bytes for the A1314), so growing a
	// 3-byte special key report into a 10-byte report is safe here.
	pbrb->BrbL2caAclTransfer.BufferSize = 10;
	RtlZeroMemory(buf, 10);

	// Build the modifier byte. In SwapFnCtrl mode the physical Fn key (which is
	// the logical Left Ctrl) is tracked in SpecialKeyModifier rather than in
	// TmpBuf, because the 10-byte reports keep overwriting TmpBuf. That is also
	// why the logical Fn + logical Left Ctrl -> Right Ctrl translation has to be
	// done here as well: when no other key is pressed the switch above is not
	// even entered.
	modifiers = TmpBuf[2] | SpecialKeyModifier;
	if (FakeFnActive && (modifiers & HidLCtrlMask))
	{
		modifiers &= ~HidLCtrlMask; // Clear the logical Left Ctrl
		modifiers |= HidRCtrlMask; // Set Right Ctrl
	}

	buf[0] = 0xa1;
	buf[1] = 0x1;
	buf[2] = modifiers;
	buf[3] = TmpBuf[3];

	int k = 4;
	while (k < 10 && TmpBuf[k] != 0)
	{
		buf[k] = TmpBuf[k];
		k++;
	}

	// Append the keys generated by the driver itself into the first free slot
	if (k < 10 && SpecialKey)
		buf[k++] = SpecialKey; // Eject -> Del
	if (k < 10 && RightCmdAsMenu)
		buf[k++] = HidMenu; // Right Command -> Application (context menu) key

	DebugPrintBuffer("ProcessA1314Block(): => ", buf, pbrb->BrbL2caAclTransfer.BufferSize);
}
