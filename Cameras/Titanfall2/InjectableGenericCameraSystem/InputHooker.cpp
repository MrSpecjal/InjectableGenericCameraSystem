////////////////////////////////////////////////////////////////////////////////////////////////////////
// Part of Injectable Generic Camera System
// Copyright(c) 2017, Frans Bouma
// All rights reserved.
// https://github.com/FransBouma/InjectableGenericCameraSystem
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met :
//
//  * Redistributions of source code must retain the above copyright notice, this
//	  list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////////////////////////////
#include "stdafx.h"
#include "Utils.h"
#include "MinHook.h"
#include "Gamepad.h"
#include "Globals.h"
#include "input.h"
#include "OverlayConsole.h"
#include "OverlayControl.h"
#include <mutex>

using namespace std;

namespace IGCS::InputHooker
{
	//--------------------------------------------------------------------------------------------------------------------------------
	// Forward declarations
	void processMessage(LPMSG lpMsg, bool removeIfRequired);

	//--------------------------------------------------------------------------------------------------------------------------------
	// Typedefs of functions to hook
	typedef DWORD(WINAPI *XINPUTGETSTATE)(DWORD, XINPUT_STATE*);
	typedef BOOL(WINAPI *GETMESSAGEA)(LPMSG, HWND, UINT, UINT);
	typedef BOOL(WINAPI *GETMESSAGEW)(LPMSG, HWND, UINT, UINT);
	typedef BOOL(WINAPI *PEEKMESSAGEA)(LPMSG, HWND, UINT, UINT, UINT);
	typedef BOOL(WINAPI *PEEKMESSAGEW)(LPMSG, HWND, UINT, UINT, UINT);
	typedef BOOL(WINAPI *POSTMESSAGEA)(HWND, UINT, WPARAM, LPARAM);
	typedef BOOL(WINAPI *POSTMESSAGEW)(HWND, UINT, WPARAM, LPARAM);
	typedef BOOL(WINAPI *SETCURSORPOS)(int, int);
	typedef BOOL(WINAPI *GETCURSORPOS)(LPPOINT);

	//--------------------------------------------------------------------------------------------------------------------------------
	// Pointers to the original hooked functions
	static XINPUTGETSTATE hookedXInputGetState = nullptr;
	static GETMESSAGEA hookedGetMessageA = nullptr;
	static GETMESSAGEW hookedGetMessageW = nullptr;
	static PEEKMESSAGEA hookedPeekMessageA = nullptr;
	static PEEKMESSAGEW hookedPeekMessageW = nullptr;
	static POSTMESSAGEA hookedPostMessageA = nullptr;
	static POSTMESSAGEW hookedPostMessageW = nullptr;
	static SETCURSORPOS hookedSetCursorPos = nullptr;
	static GETCURSORPOS hookedGetCursorPos = nullptr;

	//-----------------------------------------------
	// statics
	static CRITICAL_SECTION _messageProcessCriticalSection;

	POINT _lastCursorPositionWhenBlocked = {};

	//--------------------------------------------------------------------------------------------------------------------------------
	// Implementations

	// wrapper for easier setting up hooks for MinHook
	template <typename T>
	inline MH_STATUS MH_CreateHookEx(LPVOID pTarget, LPVOID pDetour, T** ppOriginal)
	{
		return MH_CreateHook(pTarget, pDetour, reinterpret_cast<LPVOID*>(ppOriginal));
	}

	template <typename T>
	inline MH_STATUS MH_CreateHookApiEx(LPCWSTR pszModule, LPCSTR pszProcName, LPVOID pDetour, T** ppOriginal)
	{
		return MH_CreateHookApi(pszModule, pszProcName, pDetour, reinterpret_cast<LPVOID*>(ppOriginal));
	}


	// Our own version of XInputGetState
	DWORD WINAPI detourXInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState)
	{
		// first call the original function
		DWORD toReturn = hookedXInputGetState(dwUserIndex, pState);
		// check if the passed in pState is equal to our gamestate. If so, always allow.
		if (g_cameraEnabled && pState != Globals::instance().gamePad().getState())
		{
			// check if input is blocked. If so, zero the state, so the host will see no input data
			if (Globals::instance().inputBlocked() && Globals::instance().controllerControlsCamera())
			{
				ZeroMemory(pState, sizeof(XINPUT_STATE));
			}
		}
		return toReturn;
	}


	// Our own version of GetMessageA
	BOOL WINAPI detourGetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
	{
		// first call the original function
		if (!hookedGetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax))
		{
			return FALSE;
		}
		processMessage(lpMsg, true);
		return TRUE;
	}


	// Our own version of GetMessageW
	BOOL WINAPI detourGetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
	{
		// first call the original function
		if (!hookedGetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax))
		{
			return FALSE;
		}
		processMessage(lpMsg, true);
		return TRUE;
	}


	// Our own version of PeekMessageA
	BOOL WINAPI detourPeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
	{
		// first call the original function
		if (!hookedPeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg))
		{
			return FALSE;
		}
		processMessage(lpMsg, wRemoveMsg & PM_REMOVE);
		return TRUE;
	}

	// Our own version of PeekMessageW
	BOOL WINAPI detourPeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
	{
		// first call the original function
		if (!hookedPeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg))
		{
			return FALSE;
		}
		processMessage(lpMsg, wRemoveMsg & PM_REMOVE);
		return TRUE;
	}

	// Our own version of PostMessageA
	BOOL WINAPI detourPostMessageA(HWND hWnd, UINT wMsgFilterMin, WPARAM wParam, LPARAM lParam)
	{
		if (wMsgFilterMin==WM_MOUSEMOVE && ((Globals::instance().inputBlocked() && Globals::instance().keyboardMouseControlCamera()) || OverlayControl::isMainMenuVisible()))
		{
			return TRUE;
		}
		return hookedPostMessageA(hWnd, wMsgFilterMin, wParam, lParam);
	}

	// Our own version of PostMessageW
	BOOL WINAPI detourPostMessageW(HWND hWnd, UINT wMsgFilterMin, WPARAM wParam, LPARAM lParam)
	{
		if (wMsgFilterMin == WM_MOUSEMOVE && ((Globals::instance().inputBlocked() && Globals::instance().keyboardMouseControlCamera()) || OverlayControl::isMainMenuVisible()))
		{
			return TRUE;
		}
		return hookedPostMessageW(hWnd, wMsgFilterMin, wParam, lParam);
	}
	
	BOOL WINAPI detourSetCursorPos(int X, int Y)
	{
		if ((Globals::instance().inputBlocked() && Globals::instance().keyboardMouseControlCamera()) || OverlayControl::isMainMenuVisible())
		{
			_lastCursorPositionWhenBlocked.x = X;
			_lastCursorPositionWhenBlocked.y = Y;
			return TRUE;
		}
		return hookedSetCursorPos(X, Y);
	}

	BOOL WINAPI detourGetCursorPos(LPPOINT lpPoint)
	{
		if ((Globals::instance().inputBlocked() && Globals::instance().keyboardMouseControlCamera()) || OverlayControl::isMainMenuVisible())
		{
			if (nullptr != lpPoint)
			{
				*lpPoint = _lastCursorPositionWhenBlocked;
			}
			return TRUE;
		}
		return hookedGetCursorPos(lpPoint);
	}


	void processMessage(LPMSG lpMsg, bool removeIfRequired)
	{
		EnterCriticalSection(&_messageProcessCriticalSection);
			if (lpMsg->hwnd != nullptr /* && removeIfRequired */ && Input::handleMessage(lpMsg))
			{
				// message was handled by our code. This means it's a message we want to block if input blocking is enabled or the overlay / menu is shown
				if ((Globals::instance().inputBlocked() && Globals::instance().keyboardMouseControlCamera()) || OverlayControl::isMainMenuVisible())
				{
					lpMsg->message = WM_NULL;	// reset to WM_NULL so the host will receive a dummy message instead.
				}
			}
		LeaveCriticalSection(&_messageProcessCriticalSection);
	}


	// Sets the input hooks for the various input related functions we defined own wrapper functions for. After a successful hook setup
	// they're enabled. 
	void setInputHooks()
	{
		InitializeCriticalSectionAndSpinCount(&_messageProcessCriticalSection, 0x400);

		if (MH_CreateHookApiEx(L"xinput1_3", "XInputGetState", &detourXInputGetState, &hookedXInputGetState) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking XInput1_3 failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to XInputSetState");

		if (MH_CreateHookApiEx(L"user32", "GetMessageA", &detourGetMessageA, &hookedGetMessageA) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking GetMessageA failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to GetMessageA");

		if (MH_CreateHookApiEx(L"user32", "GetMessageW", &detourGetMessageW, &hookedGetMessageW) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking GetMessageW failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to GetMessageW");

		if (MH_CreateHookApiEx(L"user32", "PeekMessageA", &detourPeekMessageA, &hookedPeekMessageA) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking PeekMessageA failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to PeekMessageA");

		if (MH_CreateHookApiEx(L"user32", "PeekMessageW", &detourPeekMessageW, &hookedPeekMessageW) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking PeekMessageW failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to PeekMessageW");

		if (MH_CreateHookApiEx(L"user32", "PostMessageA", &detourPostMessageA, &hookedPostMessageA) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking PostMessageA failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to PostMessageA");

		if (MH_CreateHookApiEx(L"user32", "PostMessageW", &detourPostMessageW, &hookedPostMessageW) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking PostMessageW failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to PostMessageW");

		if (MH_CreateHookApiEx(L"user32", "SetCursorPos", &detourSetCursorPos, &hookedSetCursorPos) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking SetCursorPos failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to SetCursorPos");

		if (MH_CreateHookApiEx(L"user32", "GetCursorPos", &detourGetCursorPos, &hookedGetCursorPos) != MH_OK)
		{
			OverlayConsole::instance().logError("Hooking GetCursorPos failed!");
		}
		OverlayConsole::instance().logDebug("Hook set to GetCursorPos");

		// Enable all hooks
		if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
		{
			OverlayConsole::instance().logLine("All hooks enabled.");
		}
		else
		{
			OverlayConsole::instance().logError("Enabling hooks failed.");
		}
	}
}