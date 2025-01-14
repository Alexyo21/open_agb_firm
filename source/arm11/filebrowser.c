/*
 *   This file is part of open_agb_firm
 *   Copyright (C) 2021 derrek, profi200
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "error_codes.h"
#include "fs.h"
#include "util.h"
#include "arm11/drivers/hid.h"
#include "arm11/fmt.h"
#include "drivers/gfx.h"
#include "arm11/filebrowser.h"


int dlistCompare(const void *a, const void *b)
{
	const char *entA = *(char**)a;
	const char *entB = *(char**)b;

	// Compare the entry type. Dirs have priority over files.
	if(*entA != *entB) return (int)*entB - *entA;

	// Compare the string.
	int res;
	do
	{
		res = *++entA - *++entB;
	} while(res == 0 && *entA != '\0' && *entB != '\0');

	return res;
}

Result scanDir(const char *const path, DirList *const dList, const char *const filter)
{
	FILINFO *const fis = (FILINFO*)malloc(sizeof(FILINFO) * DIR_READ_BLOCKS);
	if(fis == NULL) return RES_OUT_OF_MEM;

	dList->num = 0;

	Result res;
	DHandle dh;
	if((res = fOpenDir(&dh, path)) == RES_OK)
	{
		u32 read;           // Number of entries read by fReadDir().
		u32 numEntries = 0; // Total number of processed entries.
		u32 entBufPos = 0;  // Entry buffer position/number of bytes used.
		const u32 filterLen = strlen(filter);
		do
		{
			if((res = fReadDir(dh, fis, DIR_READ_BLOCKS, &read)) != RES_OK) break;
			read = (read <= MAX_DIR_ENTRIES - numEntries ? read : MAX_DIR_ENTRIES - numEntries);

			for(u32 i = 0; i < read; i++)
			{
				const char entType = (fis[i].fattrib & AM_DIR ? ENT_TYPE_DIR : ENT_TYPE_FILE);
				const u32 nameLen = strlen(fis[i].fname);
				if(entType == ENT_TYPE_FILE)
				{
					if(nameLen <= filterLen || strcmp(filter, fis[i].fname + nameLen - filterLen) != 0)
						continue;
				}

				// nameLen does not include the entry type and NULL termination.
				if(entBufPos + nameLen + 2 > MAX_ENT_BUF_SIZE) goto scanEnd;

				char *const entry = &dList->entBuf[entBufPos];
				*entry = entType;
				safeStrcpy(&entry[1], fis[i].fname, 256);
				dList->ptrs[numEntries++] = entry;
				entBufPos += nameLen + 2;
			}
		} while(read == DIR_READ_BLOCKS);

scanEnd:
		dList->num = numEntries;

		fCloseDir(dh);
	}

	free(fis);

	qsort(dList->ptrs, dList->num, sizeof(char*), dlistCompare);

	return res;
}

void showDirList(const DirList *const dList, u32 start)
{
	// Clear screen.
	ee_printf("\x1b[2J");

	const u32 listLength = (dList->num - start > SCREEN_ROWS ? start + SCREEN_ROWS : dList->num);
	for(u32 i = start; i < listLength; i++)
	{
		const char *const printStr =
			(*dList->ptrs[i] == ENT_TYPE_FILE ? "\x1b[%lu;H\x1b[37m %.51s" : "\x1b[%lu;H\x1b[33m %.51s");

		ee_printf(printStr, i - start, &dList->ptrs[i][1]);
	}
}

Result browseFiles(const char *const basePath, char selected[512])
{
	if(basePath == NULL || selected == NULL) return RES_INVALID_ARG;
	// TODO: Check if the base path is empty.

	char *curDir = (char*)malloc(512);
	if(curDir == NULL) return RES_OUT_OF_MEM;
	safeStrcpy(curDir, basePath, 512);

	DirList *const dList = (DirList*)malloc(sizeof(DirList));
	if(dList == NULL) return RES_OUT_OF_MEM;

	Result res;
	if((res = scanDir(curDir, dList, ".gba")) != RES_OK) goto end;
	showDirList(dList, 0);

	s32 cursorPos = 0; // Within the entire list.
	u32 windowPos = 0; // Window start position within the list.
	s32 oldCursorPos = 0;
	while(1)
	{
		ee_printf("\x1b[%lu;H ", oldCursorPos - windowPos);      // Clear old cursor.
		ee_printf("\x1b[%lu;H\x1b[37m>", cursorPos - windowPos); // Draw cursor.

		u32 kDown;
		do
		{
			GFX_waitForVBlank0();

			hidScanInput();
			if(hidGetExtraKeys(0) & (KEY_POWER_HELD | KEY_POWER)) goto end;
			kDown = hidKeysDown();
		} while(kDown == 0);

		const u32 num = dList->num;
		if(num != 0)
		{
			oldCursorPos = cursorPos;
			if(kDown & KEY_DRIGHT)
			{
				cursorPos += SCREEN_ROWS;
				if((u32)cursorPos > num) cursorPos = num - 1;
			}
			if(kDown & KEY_DLEFT)
			{
				cursorPos -= SCREEN_ROWS;
				if(cursorPos < -1) cursorPos = 0;
			}
			if(kDown & KEY_DUP)    cursorPos -= 1;
			if(kDown & KEY_DDOWN)  cursorPos += 1;
		}

		if(cursorPos < 0)              cursorPos = num - 1; // Wrap to end of list.
		if((u32)cursorPos > (num - 1)) cursorPos = 0;       // Wrap to start of list.

		if((u32)cursorPos < windowPos)
		{
			windowPos = cursorPos;
			showDirList(dList, windowPos);
		}
		if((u32)cursorPos >= windowPos + SCREEN_ROWS)
		{
			windowPos = cursorPos - (SCREEN_ROWS - 1);
			showDirList(dList, windowPos);
		}

		if(kDown & (KEY_A | KEY_B))
		{
			u32 pathLen = strlen(curDir);

			if(kDown & KEY_A && num != 0)
			{
				// TODO: !!! Insecure !!!
				if(curDir[pathLen - 1] != '/') curDir[pathLen++] = '/';
				safeStrcpy(curDir + pathLen, &dList->ptrs[cursorPos][1], 256);

				if(*dList->ptrs[cursorPos] == ENT_TYPE_FILE)
				{
					safeStrcpy(selected, curDir, 512);
					break;
				}
			}
			if(kDown & KEY_B)
			{
				char *tmpPathPtr = curDir + pathLen;
				while(*--tmpPathPtr != '/');
				if(*(tmpPathPtr - 1) == ':') tmpPathPtr++;
				*tmpPathPtr = '\0';
			}

			if((res = scanDir(curDir, dList, ".gba")) != RES_OK) break;
			cursorPos = 0;
			windowPos = 0;
			showDirList(dList, 0);
		}
	}

end:
	free(dList);
	free(curDir);

	// Clear screen.
	ee_printf("\x1b[2J");

	return res;
}
