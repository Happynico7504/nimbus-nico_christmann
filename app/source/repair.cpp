#include <format>
#include "repair.hpp"
#include "common.hpp"

#define PROCESS_INFO_LUMA_CUSTOM_PROCESS_NAME 0x10000

Handle getProcessHandleByName(const char* targetProcessName) {
	s32 processCount;
	u32 processIds[0x40];
	Result res = svcGetProcessList(&processCount, processIds, 0x40);
	if (R_FAILED(res)) {
		printf("svcGetProcessList failed! res: 0x%08lx\n", res);
		return 0;
	}

	// Iterate through all processes to find our target
	Handle processHandle = 0;
	for (s32 i = 0; i < processCount; i++) {
		char processName[8];
		res = svcOpenProcess(&processHandle, processIds[i]);
		if (R_FAILED(res)) {
			printf("svcOpenProcess failed! res: 0x%08lx\n", res);
			continue;
		}

		res = svcGetProcessInfo(reinterpret_cast<s64*>(processName), processHandle, PROCESS_INFO_LUMA_CUSTOM_PROCESS_NAME);
		if (R_FAILED(res)) {
			printf("svcGetProcessInfo failed! res: 0x%08lx\n", res);
			svcCloseHandle(processHandle);
			processHandle = 0;
			continue;
		}

		if (strcmp(processName, targetProcessName) != 0) {
			svcCloseHandle(processHandle);
			processHandle = 0;
		} else {
			break;
		}
	}

	return processHandle;
}

bool terminateFriendsProcess(MainStruct* mainStruct) {
	Handle friendsHandle = getProcessHandleByName("friends");
	if (friendsHandle == 0) {
		LOG_NIMBUS_ERROR(mainStruct, "Could not find friends process");
		return false;
	}

	Result rc;
	handleResult(svcTerminateProcess(friendsHandle), mainStruct, "Terminate friends process");
	svcCloseHandle(friendsHandle);
	if (R_FAILED(rc)) {
		return false;
	}

	return true;
}

Result replaceFriendsAccountHMAC(MainStruct* mainStruct, const u16* hmac, size_t length) {
	Result rc;
	u32 friendsPath[2] = {MEDIATYPE_NAND, 0x10032}; // Friends save ID
	FS_Path friendsSavePath = {PATH_BINARY, sizeof(friendsPath), &friendsPath};
	FS_Archive friendsArchive = 0;
	handleResult(FSUSER_OpenArchive(&friendsArchive, ARCHIVE_SYSTEM_SAVEDATA, friendsSavePath), mainStruct, "Open friends archive");
	if (R_FAILED(rc)) {
		return rc;
	}

	Handle friendsFile = 0;
	handleResult(FSUSER_OpenFile(&friendsFile, friendsArchive, fsMakePath(PATH_UTF16, u"/2/account"), FS_OPEN_WRITE, 0), mainStruct, "Open friends account file");
	if (R_FAILED(rc)) {
		FSUSER_CloseArchive(friendsArchive);
		return rc;
	}

	u32 bytesWritten;
	handleResult(FSFILE_Write(friendsFile, &bytesWritten, 0x42, hmac, sizeof(u16) * length, FS_WRITE_FLUSH), mainStruct, "Write to account file");
	FSFILE_Close(friendsFile);
	if (R_FAILED(rc)) {
		FSUSER_CloseArchive(friendsArchive);
		return rc;
	}

	handleResult(FSUSER_ControlArchive(friendsArchive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, nullptr, 0, nullptr, 0), mainStruct, "Commit friends archive changes");

	FSUSER_CloseArchive(friendsArchive);
	return rc;
}

bool repairAccountHMAC(MainStruct* mainStruct, const char* hmac) {
	u16 newHMAC[9]; // 8 characters + NULL
	ssize_t hmacLength = utf8_to_utf16(newHMAC, reinterpret_cast<const u8*>(hmac), sizeof(newHMAC) - 1);
	if (hmacLength < 0) {
		LOGF_NIMBUS_ERROR(mainStruct, "Failed to convert HMAC: %s", hmac);
		return false;
	}

	constexpr u8 maxLength = sizeof(newHMAC) / sizeof(u16) - 1;

	if (hmacLength > maxLength) {
		hmacLength = maxLength;
	}

	// NULL-terminate string
	newHMAC[hmacLength] = 0;

	if (!terminateFriendsProcess(mainStruct))
		return false;

	if (R_FAILED(replaceFriendsAccountHMAC(mainStruct, newHMAC, hmacLength + 1))) {
		return false;
	}

	return true;
}
