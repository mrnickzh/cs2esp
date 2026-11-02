#include <windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <mutex>
#include <thread>

#include "buttons.hpp"
#include "client_dll.hpp"
#include "offsets.hpp"
#include "../kdriver/simple/shared.hpp"

int screenWidth = 1920;
int screenHeight = 1080;

float viewMatrix[16];

ULONG myPid = 0;
DWORD64 addr = 0;

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

PSHARED_MEMORY shared;
HANDLE hEvent;

int cnt = 0;

struct ESPBox {
	float x, y, w, h;
	int team;
};

std::vector<ESPBox> espBoxes;
std::mutex espMutex;

static std::uintptr_t get_module_base(const DWORD pid, const wchar_t* module_name) {
	std::uintptr_t module_base = 0;

	HANDLE snap_shot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	if (snap_shot == INVALID_HANDLE_VALUE)
		return module_base;

	MODULEENTRY32W entry = {};
	entry.dwSize = sizeof(decltype(entry));

	if (Module32FirstW(snap_shot, &entry) == TRUE) {
		if (wcsstr(module_name, entry.szModule) != nullptr)
			module_base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
		else {
			while (Module32NextW(snap_shot, &entry) == TRUE) {
				if (wcsstr(module_name, entry.szModule) != nullptr) {
					module_base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
					break;
				}
			}
		}
	}

	CloseHandle(snap_shot);

	return module_base;
}

int readMemory(ULONG pid, DWORD64 offset, char* buff, SIZE_T size) {
	shared->Operation     = OpReadMemory;
	shared->TargetPid     = pid;
	shared->Size = size;
	shared->Address = (PVOID)offset;

	SetEvent(hEvent);

	while (shared->Operation != OpComplete) {
		Sleep(0);
	}

	memcpy(buff, shared->Payload, size);

	return shared->Status;
}

int writeMemory(ULONG pid, DWORD64 offset, char* buff, SIZE_T size) {
	shared->Operation     = OpWriteMemory;
	shared->TargetPid     = pid;
	shared->Size = size;
	shared->Address = (PVOID)offset;

	memcpy(shared->Payload, buff, size);

	SetEvent(hEvent);

	while (shared->Operation != OpComplete) {
		Sleep(0);
	}

	return shared->Status;
}

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

bool WorldToScreen(const Vec3& pos, Vec2& out, const float matrix[16], int screenWidth, int screenHeight) {
	float clipW = matrix[12] * pos.x + matrix[13] * pos.y + matrix[14] * pos.z + matrix[15];

	if (clipW < 0.001f) return false;

	float clipX = matrix[0] * pos.x + matrix[1] * pos.y + matrix[2] * pos.z + matrix[3];
	float clipY = matrix[4] * pos.x + matrix[5] * pos.y + matrix[6] * pos.z + matrix[7];

	float ndcX = clipX / clipW;
	float ndcY = clipY / clipW;

	out.x = ((float)screenWidth / 2.0f) * (ndcX + 1.0f);
	out.y = ((float)screenHeight / 2.0f) * (1.0f - ndcY);

	return true;
}

std::vector<ESPBox> getBoxes(ULONG64 pid, std::uintptr_t clientBase) {
	// printf("getboxes call\n");
	std::vector<ESPBox> boxes;

	readMemory(pid, clientBase + cs2_dumper::offsets::client_dll::dwViewMatrix, reinterpret_cast<char*>(viewMatrix), sizeof(viewMatrix));

    // entitySystem
    std::uintptr_t entitySystem = 0;
    readMemory(pid, clientBase + cs2_dumper::offsets::client_dll::dwGameEntitySystem,
               reinterpret_cast<char*>(&entitySystem), sizeof(std::uintptr_t));
    if (!entitySystem) { printf("no entity system at address 0x%llX\n", clientBase + cs2_dumper::offsets::client_dll::dwGameEntitySystem); return boxes; }

    // entityListArray
    std::uintptr_t entityListArray = 0;
    readMemory(pid, entitySystem + 0x10,
               reinterpret_cast<char*>(&entityListArray), sizeof(std::uintptr_t));
    if (!entityListArray || entityListArray < 0x10000) { printf("no entity array\n"); return boxes; }

    // highestIndex
    std::uint32_t highestIndex = 0;
    readMemory(pid, entitySystem + cs2_dumper::offsets::client_dll::dwGameEntitySystem_highestEntityIndex,
               reinterpret_cast<char*>(&highestIndex), sizeof(std::uint32_t));
    if (highestIndex > 0x10000) highestIndex = 0x10000;

    // printf("highest_index %d\n", highestIndex);

    constexpr std::size_t kEntryStride = 0x70;
    constexpr std::size_t kPlayerPawn  = 0x92C;

    for (std::uint32_t i = 1; i <= highestIndex; ++i) {
    	// entityPtr
        std::uintptr_t entityPtr = 0;
        readMemory(pid, entityListArray + std::size_t(i) * kEntryStride,
                   reinterpret_cast<char*>(&entityPtr), sizeof(std::uintptr_t));

        if (!entityPtr || entityPtr < 0x10000) continue;

    	// printf("Entity[%d]: 0x%llX\n", i, entityPtr);

    	// isAlive
    	std::uint8_t isAlive = 0;
    	readMemory(pid, entityPtr + 0x934, reinterpret_cast<char*>(&isAlive), sizeof(uint8_t));
    	// printf("isAlive[%d]: 0x%hhu\n", i, isAlive);
    	if (isAlive > 1) continue;

    	// pawnHandle
        std::uint32_t pawnHandle = 0;
        readMemory(pid, entityPtr + kPlayerPawn,
                   reinterpret_cast<char*>(&pawnHandle), sizeof(std::uint32_t));
        if (pawnHandle == 0 || pawnHandle == 0xFFFFFFFF) continue;

    	// printf("Handle[%d]: 0x%X\n", i, pawnHandle);

    	// pawnIndex
    	std::uint32_t pawnIndex = pawnHandle & 0x7FFF;
    	if (pawnIndex == 0 || pawnIndex >= 512) continue;

    	// pawn
        std::uintptr_t pawn = 0;
        readMemory(pid, entityListArray + std::size_t(pawnIndex) * kEntryStride,
                   reinterpret_cast<char*>(&pawn), sizeof(std::uintptr_t));
        if (!pawn || pawn < 0x10000) continue;

        std::int32_t hp = 0;
        std::uint8_t tm = 0;
        readMemory(pid, pawn + cs2_dumper::schemas::client_dll::C_BaseEntity::m_iHealth,  reinterpret_cast<char*>(&hp), sizeof(std::int32_t));
        readMemory(pid, pawn + cs2_dumper::schemas::client_dll::C_BaseEntity::m_iTeamNum,  reinterpret_cast<char*>(&tm), sizeof(std::uint8_t));

    	// printf("Raw HP: %d, Team: %d\n", hp, tm);
        if (hp <= 0 || hp > 200 || (tm != 2 && tm != 3)) continue;

        // printf("ctrl[%u] pawn[%u] hp=%d team=%u\n", i, pawnIndex, hp, tm);

    	// gameSceneNode
    	std::uintptr_t gameSceneNode = 0;
    	readMemory(pid, pawn + cs2_dumper::schemas::client_dll::C_BaseEntity::m_pGameSceneNode, reinterpret_cast<char*>(&gameSceneNode), sizeof(std::uintptr_t));

    	Vec3 feetPos = { 0, 0, 0 };
    	if (gameSceneNode) {
    		// feetPos
    		readMemory(pid, gameSceneNode + cs2_dumper::schemas::client_dll::CGameSceneNode::m_vecAbsOrigin, reinterpret_cast<char*>(&feetPos), sizeof(Vec3));
    	}
    	Vec3 headPos = { feetPos.x, feetPos.y, feetPos.z + 72.0f }; // Adjust height as needed

    	Vec2 screenFeet, screenHead;

    	if (WorldToScreen(feetPos, screenFeet, viewMatrix, screenWidth, screenHeight) &&
			WorldToScreen(headPos, screenHead, viewMatrix, screenWidth, screenHeight))
    	{
    		// printf("ctrl[%u] pawn[%u] on screen\n", i, pawnIndex);

    		float boxHeight = screenFeet.y - screenHead.y;
    		float boxWidth = boxHeight / 2.4f;

    		float boxLeft = screenFeet.x - (boxWidth / 2.0f);
    		float boxTop = screenHead.y;

    		ESPBox box;
    		box.x = boxLeft;
    		box.y = boxTop;
    		box.w = boxWidth;
    		box.h = boxHeight;
    		box.team = tm;

    		boxes.push_back(box);
    	}
    }

	return boxes;
}

void LogicThread() {
	while (true) {
		if (cnt < 10) { printf("Waiting...\n"); Sleep(1000); cnt++; continue; }
		std::vector<ESPBox> currentBoxes;

		for (auto pawn : getBoxes(myPid, addr)) {
			currentBoxes.push_back({pawn.x, pawn.y, pawn.w, pawn.h, pawn.team});
		}

		{
			std::lock_guard<std::mutex> lock(espMutex);
			espBoxes = currentBoxes;
		}

		Sleep(1); // ~120 Hz
	}
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);

			HDC memDC = CreateCompatibleDC(hdc);
			HBITMAP memBitmap = CreateCompatibleBitmap(hdc, screenWidth, screenHeight);

			HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

			HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
			FillRect(memDC, &ps.rcPaint, blackBrush);
			DeleteObject(blackBrush);

			{
				std::lock_guard<std::mutex> lock(espMutex);
				for (const auto& box : espBoxes) {
					HBRUSH brush = CreateSolidBrush(box.team == 2 ? RGB(255, 100, 0) : RGB(0, 150, 255));
					RECT rect = { (LONG)box.x, (LONG)box.y, (LONG)(box.x + box.w), (LONG)(box.y + box.h) };
					FrameRect(memDC, &rect, brush);
					DeleteObject(brush);
				}
			}

			BitBlt(hdc, 0, 0, screenWidth, screenHeight, memDC, 0, 0, SRCCOPY);

			SelectObject(memDC, oldBitmap);
			DeleteObject(memBitmap);
			DeleteDC(memDC);
			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main(int argc, char** argv) {
	FILE* conin = stdin;
	FILE* conout = stdout;
	FILE* conerr = stderr;
	AllocConsole();
	AttachConsole(GetCurrentProcessId());
	freopen_s(&conin, "CONIN$", "r", stdin);
	freopen_s(&conout, "CONOUT$", "w", stdout);
	freopen_s(&conerr, "CONOUT$", "w", stderr);

    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SHARED_MEMORY),
        L"Global\\MySharedSection"
    );
    if (!hMap) return 1;

    shared = (PSHARED_MEMORY)MapViewOfFile(
        hMap,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        sizeof(SHARED_MEMORY)
    );
    if (!shared) return 1;

    hEvent = CreateEventW(
        NULL,
        FALSE,          // auto-reset
        FALSE,          // initial state
        L"Global\\MyKernelEvent"
    );
    if (!hEvent) return 1;

	HINSTANCE hInstance = GetModuleHandle(nullptr);
	const wchar_t CLASS_NAME[] = L"OverlayClass";

	WNDCLASS wc = { };
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = CLASS_NAME;
	RegisterClass(&wc);

	HWND overlayHwnd = CreateWindowEx(
		WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		CLASS_NAME, L"Overlay",
		WS_POPUP,
		0, 0, screenWidth, screenHeight,
		nullptr, nullptr, hInstance, nullptr
	);

	// pure black (RGB 0,0,0) = transparent
	SetLayeredWindowAttributes(overlayHwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
	ShowWindow(overlayHwnd, SW_SHOW);

	// Sleep(atoi(argv[2]));

	myPid = atol(argv[1]);
	addr = (DWORD64)get_module_base(myPid, L"client.dll");

	std::thread logic(LogicThread);
	logic.detach();

	printf("READY! PID: %lu, ADDR: 0x%llX\n", myPid, addr);

	MSG msg = { };
	while (msg.message != WM_QUIT) {
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		InvalidateRect(overlayHwnd, nullptr, FALSE);
		Sleep(1);
	}
	
	// while (true) {
	// 	addr = (DWORD64)get_module_base(myPid, L"client.dll");
	//
	// 	// uint32_t val = 65537;
	// 	// writeMemory(myPid, addr + cs2_dumper::buttons::jump, reinterpret_cast<char*>(&val), sizeof(uint32_t)); // 0x10001
	// 	// Sleep(100);
	// 	// val = 256;
	// 	// writeMemory(myPid, addr + cs2_dumper::buttons::jump, reinterpret_cast<char*>(&val), sizeof(uint32_t)); // 0x100
	//
	// 	drawBoxes(myPid, addr);
	//
	// 	// printf("Status from kernel: 0x%X\n", shared->Status);
	// }

    /*UnmapViewOfFile(shared);
    CloseHandle(hMap);
    CloseHandle(hEvent);*/
    return 0;
}