/*
 * MacroQuest2: The extension platform for EverQuest
 * Copyright (C) 2002-2019 MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "pch.h"
#include "MQ2Main.h"
#include "MQ2DeveloperTools.h"

// "LoginFrontend" or just "Frontend" refers to the UI part of EQ that contains login
// and server select. This is contained in eqmain, and its functions are only available
// while this dll is loaded at startup.

// Once login is completed, this dll is unloaded and the functions are no longer available.

namespace mq {

//============================================================================

// From MQ2Pulse.cpp
void DoLoginPulse();

// From MQ2Overlay.cpp
bool OverlayWndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, bool fromLogin);

void InitializeLoginDetours();

static uintptr_t __FreeLibrary = 0;
static bool gbDetoursInstalled = false;
static bool gbWaitingForFrontend = false;
static bool gbInFrontend = false;

//----------------------------------------------------------------------------
// Login Pulse detour

// Dynamic trampoline allocated for the login pulse. We create this on the process heap, and then
// intentionally leak it. This allows us to exit mq2 from the login pulse. Basically, we end up
// leaving this detour trampoline behind so that the call can unwind after we leave.
void* pLoginController_GiveTime_Trampoline = nullptr;

// 0x20 bytes to create the trampoline. These are the same bytes created by a
// DETOUR_WITH_EMPTY_TRAMPOLINE macro
static uint8_t TrampolineData[] = {
	0x90, 0x90, 0x33, 0xc0, 0x8b, 0x00, 0xc3, 0x90,  0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
	0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
};

// A helper to turn a pointer-to-member into a pointer-to-void.
template <typename T>
T CoerceImpl(int dummy, ...)
{
	va_list marker;
	va_start(marker, dummy);

	void* ptr = va_arg(marker, void*);
	va_end(marker);
	return ptr;
}

template <typename T, typename U>
T Coerce(U thing) { return CoerceImpl<T>(0, thing); }

static __declspec(naked) void GiveTime_JumpToTrampoline(void* LoginController_Hook)
{
	__asm {
		mov ecx, [esp+4]
		mov eax, [pLoginController_GiveTime_Trampoline]
		jmp eax
	}
}

class LoginController_Hook
{
public:
	// This is called continually during the login mainloop so we can use it as our pulse when the MAIN
	// gameloop pulse is not active but login is.
	// that will allow plugins to work and execute commands all the way back pre login and server select etc.
	void GiveTime_Trampoline();
	void GiveTime_Detour()
	{
		gbInFrontend = true;

		if (gbWaitingForFrontend)
		{
			// Only do this on the first pass through the login main loop.
			gbWaitingForFrontend = false;

			// Redirect CXWndManager and CSidlManager to the login instances now that we know that the
			// frontend is actually running now.
			pWndMgr = EQMain__CXWndManager;
			pSidlMgr = EQMain__CSidlManager;

			// Signal to others that we are loaded properly.
			// Note: this is not really the proper way to do this since this isn't a game state, but autologin
			// is the only one listening for it.
			PluginsSetGameState(GAMESTATE_POSTFRONTLOAD);
		}

		DoLoginPulse();

		if (g_pLoginController)
		{
			if (ImGui::GetCurrentContext() != nullptr)
			{
				ImGuiIO& io = ImGui::GetIO();

				g_pLoginController->bIsKeyboardActive = !io.WantCaptureKeyboard;
				g_pLoginController->bIsMouseActive = !io.WantCaptureMouse;
			}
		}

		GiveTime_JumpToTrampoline(this);
	}
};
DETOUR_TRAMPOLINE_EMPTY(void LoginController_Hook::GiveTime_Trampoline());

// End Login pulse detour
//----------------------------------------------------------------------------

// Forwards events to ImGui. If ImGui consumes the event, we won't pass it to the game.
DETOUR_TRAMPOLINE_EMPTY(LRESULT WINAPI EQMain__WndProc_Trampoline(HWND, UINT, WPARAM, LPARAM));
LRESULT WINAPI EQMain__WndProc_Detour(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (OverlayWndProcHandler(hWnd, msg, wParam, lParam, true))
		return 1;

	return EQMain__WndProc_Trampoline(hWnd, msg, wParam, lParam);
}

// LoginViewController will continuously override the cursor. Do not let it change the cursor
// if we want to change the cursor.
class CXWndManager_Hook
{
public:
	HCURSOR GetCursorToDisplay_Trampoline() const;
	HCURSOR GetCursorToDisplay_Detour() const
	{
		if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
		{
			return GetCursor();
		}

		return GetCursorToDisplay_Trampoline();
	}
};
DETOUR_TRAMPOLINE_EMPTY(HCURSOR CXWndManager_Hook::GetCursorToDisplay_Trampoline() const);

void InitializeLoginDetours()
{
	if (gbDetoursInstalled)
		return;

	DebugSpewAlways("Initializing Login Detours");

	// Create this trampoline on the process heap so that we can abandon it after we exit.
	// If for whatever reason we can't allocate on the heap, just use the old trampoline. It'll
	// crash if we unload but at least we can get further along (and maybe the user won't try to
	// unload...)
	if (!pLoginController_GiveTime_Trampoline)
	{
		HANDLE hProcessHeap = GetProcessHeap();
		pLoginController_GiveTime_Trampoline = HeapAlloc(hProcessHeap, 0, lengthof(TrampolineData));

		if (!pLoginController_GiveTime_Trampoline)
		{
			pLoginController_GiveTime_Trampoline = Coerce<void*>(&LoginController_Hook::GiveTime_Trampoline);
		}
		else
		{
			// Initialize the trampoline with the expected payload.
			memcpy(pLoginController_GiveTime_Trampoline, TrampolineData, lengthof(TrampolineData));
		}
	}

	EzDetour(LoginController__GiveTime, &LoginController_Hook::GiveTime_Detour, pLoginController_GiveTime_Trampoline);
	EzDetour(EQMain__WndProc, EQMain__WndProc_Detour, EQMain__WndProc_Trampoline);

	if (EQMain__CXWndManager__GetCursorToDisplay)
	{
		EzDetour(EQMain__CXWndManager__GetCursorToDisplay, &CXWndManager_Hook::GetCursorToDisplay_Detour,
			&CXWndManager_Hook::GetCursorToDisplay_Trampoline);
	}

	gbDetoursInstalled = true;
}

void RemoveLoginDetours()
{
	if (!gbDetoursInstalled)
		return;

	DebugSpewAlways("Removing Login Detours");

	DWORD detours[] = {
		LoginController__GiveTime,
		EQMain__WndProc,
		EQMain__CXWndManager__GetCursorToDisplay
	};

	if (::GetModuleHandleA("eqmain.dll") != nullptr)
	{
		for (DWORD detour : detours)
			RemoveDetour(detour);
	}
	else
	{
		for (DWORD detour : detours)
			DeleteDetour(detour);
	}

	gbDetoursInstalled = false;
}

void TryInitializeLoginDetours()
{
	// leave if the dll isn't loaded
	if (!*ghEQMainInstance)
		return;

	if (InitializeEQMainOffsets())
	{
		// these are the offsets that we need to move forward.
		bool pulseSuccess = LoginController__GiveTime != 0
			&& EQMain__CXWndManager != 0
			&& EQMain__CSidlManager != 0;

		if (pulseSuccess)
		{
			InitializeLoginDetours();
		}

		bool overlaySuccess = EQMain__WndProc != 0
			&& LoginController__ProcessKeyboardEvents
			&& LoginController__ProcessMouseEvents
			&& LoginController__FlushDxKeyboard;

		// We'll continue in the first iteration of LoginPulse. This is important
		// because it means the frontend is actually running.
	}
	else
	{
		MessageBox(nullptr, "MQ2 needs an update.", "Failed to locate offsets required by MQ2LoginFrontend", MB_SYSTEMMODAL | MB_OK);
	}
}

void TryRemoveLoginDetours()
{
	if (gbInFrontend)
	{
		gbInFrontend = false;
		gbWaitingForFrontend = true;

		DebugSpewAlways("Cleaning up EQMain Offsets");

		DeveloperTools_CloseLoginFrontend();

		RemoveLoginDetours();
		CleanupEQMainOffsets();
	}
}


// Purpose of this hook is to detect when we are loading the frontend. This is only necessary if MQ2 is
// injected before eqmain.dll is loaded.
DETOUR_TRAMPOLINE_EMPTY(int LoadFrontEnd_Trampoline());
int LoadFrontEnd_Detour()
{
	gGameState = GetGameState();

	DebugTry(Benchmark(bmPluginsSetGameState, PluginsSetGameState(gGameState)));

	int ret = LoadFrontEnd_Trampoline();
	if (ret)
	{
		TryInitializeLoginDetours();
	}

	return ret;
}

// Right after leaving the frontend, we get a call to FlushDxKeyboard in ExecuteEverQuest(). We
// hook this function and use it to determine that we've exited the frontend.
DETOUR_TRAMPOLINE_EMPTY(int FlushDxKeyboard_Trampoline());
int FlushDxKeyboard_Detour()
{
	TryRemoveLoginDetours();
	return FlushDxKeyboard_Trampoline();
}

void InitializeLoginFrontend()
{
	EzDetour(__LoadFrontEnd, LoadFrontEnd_Detour, LoadFrontEnd_Trampoline);
	EzDetour(__FlushDxKeyboard, FlushDxKeyboard_Detour, FlushDxKeyboard_Trampoline);

	gbWaitingForFrontend = true;

	// Try to initialize login detours. This will succeed is eqmain.dll is already loaded. If it isn't,
	// well try again in LoadFrontend_Detour(), which is called when eqmain.dll is actually loaded.
	TryInitializeLoginDetours();
}

void ShutdownLoginFrontend()
{
	RemoveDetour(__LoadFrontEnd);
	RemoveDetour(__FlushDxKeyboard);
	RemoveLoginDetours();
}

} // namespace mq