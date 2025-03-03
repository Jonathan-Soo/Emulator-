// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <functional>
#include <mutex>
#include <set>
#include <vector>

#include "Common/CommonFuncs.h"
#include "GPU/Debugger/Breakpoints.h"
#include "GPU/GPUState.h"

namespace GPUBreakpoints {

static std::mutex breaksLock;
static bool breakCmds[256];
static std::set<u32> breakPCs;
static std::set<u32> breakTextures;
static std::set<u32> breakRenderTargets;
// Small optimization to avoid a lock/lookup for the common case.
static size_t breakPCsCount = 0;
static size_t breakTexturesCount = 0;
static size_t breakRenderTargetsCount = 0;
static std::function<void(bool)> notifyBreakpoints;

// If these are set, the above are also, but they should be temporary.
static bool breakCmdsTemp[256];
static std::set<u32> breakPCsTemp;
static std::set<u32> breakTexturesTemp;
static std::set<u32> breakRenderTargetsTemp;
static bool textureChangeTemp = false;

static u32 lastTexture = 0xFFFFFFFF;

// These are commands we run before breaking on a texture.
// They are commands that affect the decoding of the texture.
const static u8 textureRelatedCmds[] = {
	GE_CMD_TEXADDR0, GE_CMD_TEXADDR1, GE_CMD_TEXADDR2, GE_CMD_TEXADDR3, GE_CMD_TEXADDR4, GE_CMD_TEXADDR5, GE_CMD_TEXADDR6, GE_CMD_TEXADDR7,
	GE_CMD_TEXBUFWIDTH0, GE_CMD_TEXBUFWIDTH1, GE_CMD_TEXBUFWIDTH2, GE_CMD_TEXBUFWIDTH3, GE_CMD_TEXBUFWIDTH4, GE_CMD_TEXBUFWIDTH5, GE_CMD_TEXBUFWIDTH6, GE_CMD_TEXBUFWIDTH7,
	GE_CMD_TEXSIZE0, GE_CMD_TEXSIZE1, GE_CMD_TEXSIZE2, GE_CMD_TEXSIZE3, GE_CMD_TEXSIZE4, GE_CMD_TEXSIZE5, GE_CMD_TEXSIZE6, GE_CMD_TEXSIZE7,

	GE_CMD_CLUTADDR, GE_CMD_CLUTADDRUPPER, GE_CMD_LOADCLUT, GE_CMD_CLUTFORMAT,
	GE_CMD_TEXFORMAT, GE_CMD_TEXMODE, GE_CMD_TEXTUREMAPENABLE,
	GE_CMD_TEXFILTER, GE_CMD_TEXWRAP,
	GE_CMD_TEXLEVEL,

	// Sometimes found between clut/texture params.
	GE_CMD_TEXFLUSH, GE_CMD_TEXSYNC,
};
static std::vector<bool> nonTextureCmds;

void Init(void (*hasBreakpoints)(bool flag)) {
	notifyBreakpoints = hasBreakpoints;
	ClearAllBreakpoints();

	nonTextureCmds.clear();
	nonTextureCmds.resize(256, true);
	for (size_t i = 0; i < ARRAY_SIZE(textureRelatedCmds); ++i) {
		nonTextureCmds[textureRelatedCmds[i]] = false;
	}
}

void AddNonTextureTempBreakpoints() {
	for (int i = 0; i < 256; ++i) {
		if (nonTextureCmds[i]) {
			AddCmdBreakpoint(i, true);
		}
	}
}

u32 GetAdjustedTextureAddress(u32 op) {
	const u8 cmd = op >> 24;
	bool interesting = (cmd >= GE_CMD_TEXADDR0 && cmd <= GE_CMD_TEXADDR7);
	interesting = interesting || (cmd >= GE_CMD_TEXBUFWIDTH0 && cmd <= GE_CMD_TEXBUFWIDTH7);

	if (!interesting) {
		return (u32)-1;
	}

	int level = cmd <= GE_CMD_TEXADDR7 ? cmd - GE_CMD_TEXADDR0 : cmd - GE_CMD_TEXBUFWIDTH0;
	u32 addr;

	// Okay, so would this op modify the low or high part?
	if (cmd <= GE_CMD_TEXADDR7) {
		addr = (op & 0xFFFFF0) | ((gstate.texbufwidth[level] << 8) & 0x0F000000);
	} else {
		addr = (gstate.texaddr[level] & 0xFFFFF0) | ((op << 8) & 0x0F000000);
	}

	return addr;
}

u32 GetAdjustedRenderTargetAddress(u32 op) {
	const u8 cmd = op >> 24;
	switch (cmd) {
	case GE_CMD_FRAMEBUFPTR:
	case GE_CMD_ZBUFPTR:
		return op & 0x003FFFF0;
	}

	return (u32)-1;
}

bool IsTextureChangeBreakpoint(u32 op, u32 addr) {
	if (!textureChangeTemp) {
		return false;
	}

	const u8 cmd = op >> 24;
	bool enabled = gstate.isTextureMapEnabled();

	// Only for level 0.
	if (cmd != GE_CMD_TEXADDR0 && cmd != GE_CMD_TEXBUFWIDTH0) {
		// But we don't break when it's not enabled.
		if (cmd == GE_CMD_TEXTUREMAPENABLE) {
			enabled = (op & 1) != 0;
		} else {
			return false;
		}
	}
	if (enabled && addr != lastTexture) {
		textureChangeTemp = false;
		lastTexture = addr;
		return true;
	} else {
		return false;
	}
}

bool IsTextureCmdBreakpoint(u32 op) {
	const u32 addr = GetAdjustedTextureAddress(op);
	if (addr != (u32)-1) {
		return IsTextureChangeBreakpoint(op, addr) || IsTextureBreakpoint(addr);
	} else {
		return IsTextureChangeBreakpoint(op, gstate.getTextureAddress(0));
	}
}

bool IsRenderTargetCmdBreakpoint(u32 op) {
	const u32 addr = GetAdjustedRenderTargetAddress(op);
	if (addr != (u32)-1) {
		return IsRenderTargetBreakpoint(addr);
	}
	return false;
}

bool IsBreakpoint(u32 pc, u32 op) {
	if (IsAddressBreakpoint(pc) || IsOpBreakpoint(op)) {
		return true;
	}

	if ((breakTexturesCount != 0 || textureChangeTemp) && IsTextureCmdBreakpoint(op)) {
		// Break on the next non-texture.
		AddNonTextureTempBreakpoints();
	}
	if (breakRenderTargetsCount != 0 && IsRenderTargetCmdBreakpoint(op)) {
		return true;
	}

	return false;
}

bool IsAddressBreakpoint(u32 addr, bool &temp) {
	if (breakPCsCount == 0) {
		temp = false;
		return false;
	}

	std::lock_guard<std::mutex> guard(breaksLock);
	temp = breakPCsTemp.find(addr) != breakPCsTemp.end();
	return breakPCs.find(addr) != breakPCs.end();
}

bool IsAddressBreakpoint(u32 addr) {
	if (breakPCsCount == 0) {
		return false;
	}

	std::lock_guard<std::mutex> guard(breaksLock);
	return breakPCs.find(addr) != breakPCs.end();
}

bool IsTextureBreakpoint(u32 addr, bool &temp) {
	if (breakTexturesCount == 0) {
		temp = false;
		return false;
	}

	std::lock_guard<std::mutex> guard(breaksLock);
	temp = breakTexturesTemp.find(addr) != breakTexturesTemp.end();
	return breakTextures.find(addr) != breakTextures.end();
}

bool IsTextureBreakpoint(u32 addr) {
	if (breakTexturesCount == 0) {
		return false;
	}

	std::lock_guard<std::mutex> guard(breaksLock);
	return breakTextures.find(addr) != breakTextures.end();
}

bool IsRenderTargetBreakpoint(u32 addr, bool &temp) {
	if (breakRenderTargetsCount == 0) {
		temp = false;
		return false;
	}

	addr &= 0x003FFFF0;

	std::lock_guard<std::mutex> guard(breaksLock);
	temp = breakRenderTargetsTemp.find(addr) != breakRenderTargetsTemp.end();
	return breakRenderTargets.find(addr) != breakRenderTargets.end();
}

bool IsRenderTargetBreakpoint(u32 addr) {
	if (breakRenderTargetsCount == 0) {
		return false;
	}

	addr &= 0x003FFFF0;

	std::lock_guard<std::mutex> guard(breaksLock);
	return breakRenderTargets.find(addr) != breakRenderTargets.end();
}

bool IsOpBreakpoint(u32 op, bool &temp) {
	return IsCmdBreakpoint(op >> 24, temp);
}

bool IsOpBreakpoint(u32 op) {
	return IsCmdBreakpoint(op >> 24);
}

bool IsCmdBreakpoint(u8 cmd, bool &temp) {
	temp = breakCmdsTemp[cmd];
	return breakCmds[cmd];
}

bool IsCmdBreakpoint(u8 cmd) {
	return breakCmds[cmd];
}

static bool HasAnyBreakpoints() {
	if (breakPCsCount != 0 || breakTexturesCount != 0 || breakRenderTargetsCount != 0)
		return true;
	if (textureChangeTemp)
		return true;

	for (int i = 0; i < 256; ++i) {
		if (breakCmds[i] || breakCmdsTemp[i])
			return true;
	}

	return false;
}

void AddAddressBreakpoint(u32 addr, bool temp) {
	std::lock_guard<std::mutex> guard(breaksLock);

	if (temp) {
		if (breakPCs.find(addr) == breakPCs.end()) {
			breakPCsTemp.insert(addr);
			breakPCs.insert(addr);
		}
		// Already normal breakpoint, let's not make it temporary.
	} else {
		// Remove the temporary marking.
		breakPCsTemp.erase(addr);
		breakPCs.insert(addr);
	}

	breakPCsCount = breakPCs.size();
	notifyBreakpoints(true);
}

void AddCmdBreakpoint(u8 cmd, bool temp) {
	if (temp) {
		if (!breakCmds[cmd]) {
			breakCmdsTemp[cmd] = true;
			breakCmds[cmd] = true;
		}
		// Ignore adding a temp breakpoint when a normal one exists.
	} else {
		// This is no longer temporary.
		breakCmdsTemp[cmd] = false;
		breakCmds[cmd] = true;
	}
	notifyBreakpoints(true);
}

void AddTextureBreakpoint(u32 addr, bool temp) {
	std::lock_guard<std::mutex> guard(breaksLock);

	if (temp) {
		if (breakTextures.find(addr) == breakTextures.end()) {
			breakTexturesTemp.insert(addr);
			breakTextures.insert(addr);
		}
	} else {
		breakTexturesTemp.erase(addr);
		breakTextures.insert(addr);
	}

	breakTexturesCount = breakTextures.size();
	notifyBreakpoints(true);
}

void AddRenderTargetBreakpoint(u32 addr, bool temp) {
	std::lock_guard<std::mutex> guard(breaksLock);

	addr &= 0x003FFFF0;

	if (temp) {
		if (breakRenderTargets.find(addr) == breakRenderTargets.end()) {
			breakRenderTargetsTemp.insert(addr);
			breakRenderTargets.insert(addr);
		}
	} else {
		breakRenderTargetsTemp.erase(addr);
		breakRenderTargets.insert(addr);
	}

	breakRenderTargetsCount = breakRenderTargets.size();
	notifyBreakpoints(true);
}

void AddTextureChangeTempBreakpoint() {
	textureChangeTemp = true;
	notifyBreakpoints(true);
}

void AddAnyTempBreakpoint() {
	for (int i = 0; i < 256; ++i) {
		AddCmdBreakpoint(i, true);
	}
	notifyBreakpoints(true);
}

void RemoveAddressBreakpoint(u32 addr) {
	std::lock_guard<std::mutex> guard(breaksLock);

	breakPCsTemp.erase(addr);
	breakPCs.erase(addr);

	breakPCsCount = breakPCs.size();
	notifyBreakpoints(HasAnyBreakpoints());
}

void RemoveCmdBreakpoint(u8 cmd) {
	std::lock_guard<std::mutex> guard(breaksLock);

	breakCmdsTemp[cmd] = false;
	breakCmds[cmd] = false;
	notifyBreakpoints(HasAnyBreakpoints());
}

void RemoveTextureBreakpoint(u32 addr) {
	std::lock_guard<std::mutex> guard(breaksLock);

	breakTexturesTemp.erase(addr);
	breakTextures.erase(addr);

	breakTexturesCount = breakTextures.size();
	notifyBreakpoints(HasAnyBreakpoints());
}

void RemoveRenderTargetBreakpoint(u32 addr) {
	std::lock_guard<std::mutex> guard(breaksLock);

	addr &= 0x003FFFF0;

	breakRenderTargetsTemp.erase(addr);
	breakRenderTargets.erase(addr);

	breakRenderTargetsCount = breakRenderTargets.size();
	notifyBreakpoints(HasAnyBreakpoints());
}

void RemoveTextureChangeTempBreakpoint() {
	std::lock_guard<std::mutex> guard(breaksLock);

	textureChangeTemp = false;
	notifyBreakpoints(HasAnyBreakpoints());
}

void UpdateLastTexture(u32 addr) {
	lastTexture = addr;
}

void ClearAllBreakpoints() {
	std::lock_guard<std::mutex> guard(breaksLock);

	for (int i = 0; i < 256; ++i) {
		breakCmds[i] = false;
		breakCmdsTemp[i] = false;
	}
	breakPCs.clear();
	breakTextures.clear();
	breakRenderTargets.clear();

	breakPCsTemp.clear();
	breakTexturesTemp.clear();
	breakRenderTargetsTemp.clear();

	breakPCsCount = breakPCs.size();
	breakTexturesCount = breakTextures.size();
	breakRenderTargetsCount = breakRenderTargets.size();

	textureChangeTemp = false;
	notifyBreakpoints(false);
}

void ClearTempBreakpoints() {
	std::lock_guard<std::mutex> guard(breaksLock);

	// Reset ones that were temporary back to non-breakpoints in the primary arrays.
	for (int i = 0; i < 256; ++i) {
		if (breakCmdsTemp[i]) {
			breakCmds[i] = false;
			breakCmdsTemp[i] = false;
		}
	}

	for (auto it = breakPCsTemp.begin(), end = breakPCsTemp.end(); it != end; ++it) {
		breakPCs.erase(*it);
	}
	breakPCsTemp.clear();
	breakPCsCount = breakPCs.size();

	for (auto it = breakTexturesTemp.begin(), end = breakTexturesTemp.end(); it != end; ++it) {
		breakTextures.erase(*it);
	}
	breakTexturesTemp.clear();
	breakTexturesCount = breakTextures.size();

	for (auto it = breakRenderTargetsTemp.begin(), end = breakRenderTargetsTemp.end(); it != end; ++it) {
		breakRenderTargets.erase(*it);
	}
	breakRenderTargetsTemp.clear();
	breakRenderTargetsCount = breakRenderTargets.size();

	textureChangeTemp = false;
	notifyBreakpoints(HasAnyBreakpoints());
}

};
