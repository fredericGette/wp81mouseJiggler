// wp81mouseJiggler.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

typedef struct _AttributeRequested {
	BYTE opcode;
	BYTE startingHandle[2];
	BYTE endingHandle[2];
	BYTE uuid[2];
	BYTE value[2];
} AttributeRequested;

static HANDLE hEventCmdFinished = NULL;
static HANDLE hLogFile;
static BOOL readLoop_continue;
static BOOL mainLoop_continue;
static BYTE connectionHandle[2];
static HANDLE hEventConnCmpltReceived = NULL;
static HANDLE hEventLTKRqstReceived = NULL;
static HANDLE hEventAclDataReceived = NULL;
static BOOL isXchgMTURqstReceived = FALSE;
static BYTE pairingRequest[7]; // MSB first
static BYTE pairingResponse[7]; // MSB first
static BYTE iat; // Initiating device address type
static BYTE ia[6]; // Initiating device address (MSB first)
static BYTE rat = 0x00; // Responding device address type = public
static BYTE ra[6]; // Responding device address 
static BYTE mRand[16]; // Pairing random send by the initiator/master device (MSB first)
static BYTE mtu[2] = {23,0}; // MTU common to initiating and responding device (default minimum = 23 bytes)
static AttributeRequested* pAttributeRequested[2];
static int nbAttRqstReceived = 0;
static BYTE* aclData[3];
static int activeAclData;
static int passiveAclData;
static int passiveAclData2;
static DWORD aclDataSize[3];

// Debug helper
void printBuffer2HexString(BYTE* buffer, size_t bufSize, BOOL isReceived, CHAR source)
{
	if (bufSize < 1)
	{
		return;
	}

	FILETIME SystemFileTime;
	BYTE *p = buffer;
	UINT i = 0;
	CHAR *temp = (CHAR*)malloc(1024);

	GetSystemTimeAsFileTime(&SystemFileTime);
	sprintf_s(temp, 1024, "%c%s%010u.%010u ", source, isReceived?"=":"<",SystemFileTime.dwHighDateTime, SystemFileTime.dwLowDateTime);
	
	for (; i<bufSize; i++)
	{
		sprintf_s(temp + 24 + i*3, 1024 - 24 - i*3, "%02X ", p[i]);
	}
	sprintf_s(temp + 24 + i * 3, 1024 - 24 - i * 3, "\n");
	printf("%s", temp);

	if (hLogFile != NULL)
	{
		DWORD dwBytesToWrite = 25 + i * 3;
		DWORD dwBytesWritten = 0;
		WriteFile(
			hLogFile,           // open file handle
			temp,      // start of data to write
			dwBytesToWrite,  // number of bytes to write
			&dwBytesWritten, // number of bytes that were written
			NULL);            // no overlapped structure
	}

	free(temp);
}

BOOL WINAPI consoleHandler(DWORD signal)
{
	switch (signal)
	{
	case CTRL_C_EVENT:
		printf("Terminating...\n");
		mainLoop_continue = FALSE;
		// Signal is handled - don't pass it on to the next handler.
		return TRUE;
	default:
		// Pass signal on to the next handler.
		return FALSE;
	}
}

void storeInitiatingDeviceInformation(BYTE* evtConCompltMsgReceived)
{
	iat = evtConCompltMsgReceived[12];
	for (int i = 0; i < 6; i++)
	{
		ia[i] = evtConCompltMsgReceived[18 - i];
	}
	printf("Remote device address: %02X:%02X:%02X:%02X:%02X:%02X (type %s)\n", ia[0], ia[1], ia[2], ia[3], ia[4], ia[5], iat == 0 ? "public" : "random");
}

void storeRespondingDeviceInformation(BYTE* bthLocalRadioInfo)
{
	for (int i = 0; i < 6; i++)
	{
		ra[i] = bthLocalRadioInfo[13 - i];
	}
	printf("Local device address: %02X:%02X:%02X:%02X:%02X:%02X (type %s)\n", ra[0], ra[1], ra[2], ra[3], ra[4], ra[5], rat == 0 ? "public" : "random");
}

DWORD WINAPI readEvents(void* notUsed)
{
	HANDLE hciControlDeviceEvt;
	DWORD returned;
	BYTE* readEvent_inputBuffer;
	BYTE* readEvent_outputBuffer;
	BOOL success;

	hciControlDeviceEvt = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDeviceEvt == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	readEvent_inputBuffer = (BYTE*)malloc(4);
	readEvent_inputBuffer[0] = 0x04;
	readEvent_inputBuffer[1] = 0x00;
	readEvent_inputBuffer[2] = 0x00;
	readEvent_inputBuffer[3] = 0x00;

	readEvent_outputBuffer = (BYTE*)malloc(262);

	printf("Start listening to Events...\n");
	while (readLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceEvt, IOCTL_CONTROL_READ_HCI, readEvent_inputBuffer, 4, readEvent_outputBuffer, 262, &returned, NULL);
		if (success)
		{
			printBuffer2HexString(readEvent_outputBuffer, returned, TRUE, 'e');
			if (returned == 11 && readEvent_outputBuffer[5] == 0x0E)
			{
				printf("Received: Command complete\n");
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 26 && readEvent_outputBuffer[5] == 0x3E && readEvent_outputBuffer[7] == 0x01)
			{
				printf("Received: Connection complete\n");
				storeInitiatingDeviceInformation(readEvent_outputBuffer);
				SetEvent(hEventConnCmpltReceived);
			}
			else if (returned == 20 && readEvent_outputBuffer[5] == 0x3E && readEvent_outputBuffer[7] == 0x05)
			{
				printf("Received: Long Term Key Request\n");
				SetEvent(hEventLTKRqstReceived);
			}
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readEvent_inputBuffer);
	free(readEvent_outputBuffer);
	CloseHandle(hciControlDeviceEvt);

	return 0;
}

void storeConnectionHandle(BYTE* aclMsgReceived)
{
	memcpy(connectionHandle, aclMsgReceived + 5, 2);
}

void storePairingRequest(BYTE* preqMsgReceived)
{
	for (int i = 0; i < 7; i++)
	{
		pairingRequest[i] = preqMsgReceived[19 - i];
	}
}

void storePairingResponse(BYTE* presMsgSend)
{
	for (int i = 0; i < 7; i++)
	{
		pairingResponse[i] = presMsgSend[19 - i];
	}
}

void storePairingRandomReceived(BYTE* pairingRndReceived)
{
	for (int i = 0; i < 16; i++)
	{
		mRand[i] = pairingRndReceived[29 - i];
	}
}

void storeMTUReceived(BYTE* exchangeMTURequestReceived)
{
	mtu[0] = exchangeMTURequestReceived[14];
	mtu[1] = exchangeMTURequestReceived[15];
}

// Read By Type Request
// Read By Group Type Request
void storeReadAttributeRequest(BYTE* readAttributeRequestReceived)
{
	pAttributeRequested[nbAttRqstReceived]->opcode = readAttributeRequestReceived[13];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[0] = readAttributeRequestReceived[14];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[1] = readAttributeRequestReceived[15];
	pAttributeRequested[nbAttRqstReceived]->endingHandle[0] = readAttributeRequestReceived[16];
	pAttributeRequested[nbAttRqstReceived]->endingHandle[1] = readAttributeRequestReceived[17];
	pAttributeRequested[nbAttRqstReceived]->uuid[0] = readAttributeRequestReceived[18];
	pAttributeRequested[nbAttRqstReceived]->uuid[1] = readAttributeRequestReceived[19];
}

// Find By Type Value Request
void storeFindAttributeRequest(BYTE* findAttributeRequestReceived)
{
	pAttributeRequested[nbAttRqstReceived]->opcode = findAttributeRequestReceived[13];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[0] = findAttributeRequestReceived[14];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[1] = findAttributeRequestReceived[15];
	pAttributeRequested[nbAttRqstReceived]->endingHandle[0] = findAttributeRequestReceived[16];
	pAttributeRequested[nbAttRqstReceived]->endingHandle[1] = findAttributeRequestReceived[17];
	pAttributeRequested[nbAttRqstReceived]->uuid[0] = findAttributeRequestReceived[18];
	pAttributeRequested[nbAttRqstReceived]->uuid[1] = findAttributeRequestReceived[19];
	pAttributeRequested[nbAttRqstReceived]->value[0] = findAttributeRequestReceived[20];
	pAttributeRequested[nbAttRqstReceived]->value[1] = findAttributeRequestReceived[21];
}

// Find Information Request
void storeFindInformationRequest(BYTE* findInfoRequestReceived)
{
	pAttributeRequested[nbAttRqstReceived]->opcode = findInfoRequestReceived[13];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[0] = findInfoRequestReceived[14];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[1] = findInfoRequestReceived[15];
	pAttributeRequested[nbAttRqstReceived]->endingHandle[0] = findInfoRequestReceived[16];
	pAttributeRequested[nbAttRqstReceived]->endingHandle[1] = findInfoRequestReceived[17];
}

// Read Request
void storeReadRequest(BYTE* readRequestReceived)
{
	pAttributeRequested[nbAttRqstReceived]->opcode = readRequestReceived[13];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[0] = readRequestReceived[14];
	pAttributeRequested[nbAttRqstReceived]->startingHandle[1] = readRequestReceived[15];
}

// Write Request
void storeWriteRequest(BYTE* writeRequestReceived)
{
	pAttributeRequested[nbAttRqstReceived]->opcode = writeRequestReceived[13];
}

void parseAclData()
{
	BYTE* readAcl_outputBuffer = aclData[activeAclData];
	DWORD returned = aclDataSize[activeAclData];

	ResetEvent(hEventAclDataReceived);

	printBuffer2HexString(readAcl_outputBuffer, returned, TRUE, 'a');
	if (returned == 20
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x06 // Security Manager Protocol
		&& readAcl_outputBuffer[13] == 0x01 // Pairing request
		)
	{
		printf("Received Pairing Request\n");
		storeConnectionHandle(readAcl_outputBuffer);
		storePairingRequest(readAcl_outputBuffer);
	}
	else if (returned == 30
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x06 // Security Manager Protocol
		&& readAcl_outputBuffer[13] == 0x03 // Pairing confirm
		)
	{
		printf("Received Pairing Confirm\n");
	}
	else if (returned == 30
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x06 // Security Manager Protocol
		&& readAcl_outputBuffer[13] == 0x04 // Pairing random
		)
	{
		printf("Received Pairing Random\n");
		storePairingRandomReceived(readAcl_outputBuffer);
	}
	else if (returned == 16
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
		&& readAcl_outputBuffer[13] == 0x02 // Exchange MTU Request
		)
	{
		printf("Received Exchange MTU Request\n");
		storeMTUReceived(readAcl_outputBuffer);
		isXchgMTURqstReceived = TRUE;
	}
	else if (returned == 20
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
		&& readAcl_outputBuffer[13] == 0x08 // Read By Type Request
		)
	{
		printf("Received Read By Type Request (2 bytes UUID)\n");
		storeReadAttributeRequest(readAcl_outputBuffer);
	}
	else if (returned == 20
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
		&& readAcl_outputBuffer[13] == 0x10 // Read By Group Type Request
		)
	{
		printf("Received Read By Group Type Request (2 bytes UUID)\n");
		storeReadAttributeRequest(readAcl_outputBuffer);
	}
	else if (returned == 22
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
		&& readAcl_outputBuffer[13] == 0x06 // Find By Type Value Request
		)
	{
		printf("Received Find By Type Value Request\n");
		storeFindAttributeRequest(readAcl_outputBuffer);
	}
	else if (returned == 18
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
		&& readAcl_outputBuffer[13] == 0x04 // Find Information Request
		)
	{
		printf("Received Find Information Request\n");
		storeFindInformationRequest(readAcl_outputBuffer);
	}
	else if (returned == 16
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
		&& readAcl_outputBuffer[13] == 0x0A // Read Request
		)
	{
		printf("Received Read Request\n");
		storeReadRequest(readAcl_outputBuffer);
	}
	else if (returned >= 16
		&& readAcl_outputBuffer[4] == 0x02 // ACL message
		&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
		&& readAcl_outputBuffer[13] == 0x12 // Write Request
		)
	{
		printf("Received Write Request\n");
		storeWriteRequest(readAcl_outputBuffer);
	}
}

DWORD WINAPI readAclData(void* notUsed)
{
	HANDLE hciControlDeviceAcl;
	BYTE* readAcl_inputBuffer;
	BOOL success;
	DWORD temp;

	hciControlDeviceAcl = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDeviceAcl == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	readAcl_inputBuffer = (BYTE*)malloc(4);
	readAcl_inputBuffer[0] = 0x02;
	readAcl_inputBuffer[1] = 0x00;
	readAcl_inputBuffer[2] = 0x00;
	readAcl_inputBuffer[3] = 0x00;

	activeAclData = 1;
	passiveAclData = 0;

	printf("Start listening to ACL Data...\n");
	while (readLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceAcl, IOCTL_CONTROL_READ_HCI, readAcl_inputBuffer, 4, aclData[passiveAclData], 1024, &aclDataSize[passiveAclData], NULL);
		if (success)
		{
			printf("** Received ACL1 active=%d passive=%d size=%d\n", activeAclData, passiveAclData, aclDataSize[passiveAclData]);
			temp = activeAclData;
			activeAclData = passiveAclData;
			passiveAclData = temp;
			SetEvent(hEventAclDataReceived);
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readAcl_inputBuffer);
	CloseHandle(hciControlDeviceAcl);

	return 0;
}

DWORD WINAPI readAclData2(void* notUsed)
{
	HANDLE hciControlDeviceAcl;
	BYTE* readAcl_inputBuffer;
	BOOL success;
	DWORD temp;

	hciControlDeviceAcl = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDeviceAcl == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	readAcl_inputBuffer = (BYTE*)malloc(4);
	readAcl_inputBuffer[0] = 0x02;
	readAcl_inputBuffer[1] = 0x00;
	readAcl_inputBuffer[2] = 0x00;
	readAcl_inputBuffer[3] = 0x00;

	activeAclData = 1;
	passiveAclData2 = 2;

	printf("Start listening to ACL Data2...\n");
	while (readLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceAcl, IOCTL_CONTROL_READ_HCI, readAcl_inputBuffer, 4, aclData[passiveAclData2], 1024, &aclDataSize[passiveAclData2], NULL);
		if (success)
		{
			printf("** Received ACL2 active=%d passive=%d size=%d\n", activeAclData, passiveAclData, aclDataSize[passiveAclData]);
			temp = activeAclData;
			activeAclData = passiveAclData2;
			passiveAclData2 = temp;
			SetEvent(hEventAclDataReceived);
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readAcl_inputBuffer);
	CloseHandle(hciControlDeviceAcl);

	return 0;
}

int main()
{
	BYTE* cmd_inputBuffer;
	BYTE* cmd_outputBuffer;
	BOOL success;
	DWORD returned;
	HANDLE hciControlDeviceCmd;
	HANDLE bluetoothDevice;
	HANDLE hThreadEvent;
	HANDLE hThreadAclData;
	HANDLE hThreadAclData2;
	int i;
	int n;
	pAttributeRequested[0] = (AttributeRequested*)malloc(sizeof(AttributeRequested));
	pAttributeRequested[0]->opcode = 0x00;
	pAttributeRequested[1] = (AttributeRequested*)malloc(sizeof(AttributeRequested));
	aclData[0] = (BYTE*)malloc(1024);
	aclData[1] = (BYTE*)malloc(1024);
	aclData[2] = (BYTE*)malloc(1024);

	mainLoop_continue = TRUE;
	SetConsoleCtrlHandler(consoleHandler, TRUE);

	hLogFile = CreateFileA("C:\\Data\\USERS\\Public\\Documents\\wp81mouseJiggler.log",                // name of the write
		GENERIC_WRITE,          // open for writing
		FILE_SHARE_READ,        // share
		NULL,                   // default security
		CREATE_ALWAYS,          // always create new file 
		FILE_ATTRIBUTE_NORMAL,  // normal file
		NULL);                  // no attr. template
	if (hLogFile == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open log file! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	cmd_inputBuffer = (BYTE*)malloc(1024);
	cmd_outputBuffer = (BYTE*)malloc(1024);
	readLoop_continue = TRUE;
	i = 0;
	n = 0;

	// Lumia520
	bluetoothDevice = CreateFileA("\\??\\SystemBusQc#SMD_BT#4&315a27b&0&4097#{0850302a-b344-4fda-9be9-90576b8d46f0}", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (bluetoothDevice == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open bluetooth device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	printf("Query local bluetooth device information\n");
	success = DeviceIoControl(bluetoothDevice, IOCTL_BTH_GET_LOCAL_INFO, cmd_inputBuffer, 1024, cmd_outputBuffer, 1024, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'b');
		storeRespondingDeviceInformation(cmd_outputBuffer);
	}

	CloseHandle(bluetoothDevice);

	hciControlDeviceCmd = CreateFileA("\\\\.\\wp81controldevice", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (hciControlDeviceCmd == INVALID_HANDLE_VALUE)
	{
		printf("Failed to open wp81controldevice device! 0x%08X\n", GetLastError());
		return EXIT_FAILURE;
	}

	cmd_inputBuffer[0] = 1; // Block IOCTL_BTHX_WRITE_HCI and IOCTL_BTHX_READ_HCI coming from the Windows Bluetooth stack.
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_CMD, cmd_inputBuffer, 1, NULL, 0, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		printf("Check Bluetooth is enabled.\n");
		return EXIT_FAILURE;
	}

	// Want to execute only one command at a time
	hEventCmdFinished = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_CMD_IN_PROGRESS"
	);

	hEventAclDataReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_ACL_DATA_RECEIVED"
	);

	// Start "read events" thread
	hThreadEvent = CreateThread(NULL, 0, readEvents, NULL, 0, NULL);

	// Start "read ACL data" thread
	hThreadAclData = CreateThread(NULL, 0, readAclData, NULL, 0, NULL);
	hThreadAclData2 = CreateThread(NULL, 0, readAclData2, NULL, 0, NULL);

	printf("Send Reset command\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x03; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x01; // Command
	cmd_inputBuffer[i++] = 0x03; // OGF 0x03, OCF 0x0003
	cmd_inputBuffer[i++] = 0x0C; // // The OGF occupies the upper 6 bits of the Opcode, while the OCF occupies the remaining 10 bits.
	cmd_inputBuffer[i++] = 0x00; // Parameter Total Length
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	ResetEvent(hEventCmdFinished);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Reset sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE,'c');
	}

	printf("Wait for the end of the Reset command\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Send LE Set Advertising Parameters Command\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x11; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x01; // Command
	cmd_inputBuffer[i++] = 0x06; // OGF 0x08, OCF 0x0006
	cmd_inputBuffer[i++] = 0x20; // 
	cmd_inputBuffer[i++] = 0x0F; // Parameter Total Length
	cmd_inputBuffer[i++] = 0xA0; // Advertising_Interval_Min
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0xA0; // Advertising_Interval_Max:
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; // Advertising_Type
	cmd_inputBuffer[i++] =  rat; // Own_Address_Type
	cmd_inputBuffer[i++] = 0x00; // Direct_Address_Type
	cmd_inputBuffer[i++] = 0x00; // Direct_Address
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x07; // Advertising_Channel_Map
	cmd_inputBuffer[i++] = 0x00; // Advertising_Filter_Policy
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	ResetEvent(hEventCmdFinished);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Set Advertising Parameters sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Wait for the end of the Set Advertising Parameters command\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Send LE Set Advertising Data Command\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x23; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x01; // Command
	cmd_inputBuffer[i++] = 0x08; // OGF 0x08, OCF 0x0008
	cmd_inputBuffer[i++] = 0x20; // 
	cmd_inputBuffer[i++] = 0x20; // Parameter Total Length
	cmd_inputBuffer[i++] = 0x1A; // Advertising_Data_Length
	cmd_inputBuffer[i++] = 0x0E; // Length 1st field (Advertising_Data)
	cmd_inputBuffer[i++] = 0x09; // Type: Device Name (LumiaMouse520)
	cmd_inputBuffer[i++] = 0x4C; // 
	cmd_inputBuffer[i++] = 0x75; // 
	cmd_inputBuffer[i++] = 0x6D; // 
	cmd_inputBuffer[i++] = 0x69; // 
	cmd_inputBuffer[i++] = 0x61; // 
	cmd_inputBuffer[i++] = 0x4D; //
	cmd_inputBuffer[i++] = 0x6F; //
	cmd_inputBuffer[i++] = 0x75; //
	cmd_inputBuffer[i++] = 0x73; //
	cmd_inputBuffer[i++] = 0x65; //
	cmd_inputBuffer[i++] = 0x35; // 
	cmd_inputBuffer[i++] = 0x32; //
	cmd_inputBuffer[i++] = 0x30; //
	cmd_inputBuffer[i++] = 0x03; // Length 2nd field
	cmd_inputBuffer[i++] = 0x19; // Type: Appearance
	cmd_inputBuffer[i++] = 0xC2; // Mouse
	cmd_inputBuffer[i++] = 0x03; //
	cmd_inputBuffer[i++] = 0x02; // Length 3rd field
	cmd_inputBuffer[i++] = 0x01; // Type:Flags
	cmd_inputBuffer[i++] = 0x05; // BR/EDR Not Supported + LE Limited Discoverable Mode
	cmd_inputBuffer[i++] = 0x03; // Length 4th field
	cmd_inputBuffer[i++] = 0x03; // Type: 16-bit Service Class UUIDs
	cmd_inputBuffer[i++] = 0x12; // Human Interface Device
	cmd_inputBuffer[i++] = 0x18; //
	cmd_inputBuffer[i++] = 0x00; // Non-significant part
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x00; //
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	ResetEvent(hEventCmdFinished);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Set Advertising Data sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Wait for the end of the Set Advertising Data command\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	// Prepare to wait for the connection complete event
	hEventConnCmpltReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_CONNECTION_COMPLETE"
	);

	// Prepare to wait for the Long Term Key Request event
	hEventLTKRqstReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_LTK_REQUEST"
	);

	printf("Send LE Set Advertise Enable Command\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x04; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x01; // Command
	cmd_inputBuffer[i++] = 0x0A; // OGF 0x08, OCF 0x000A
	cmd_inputBuffer[i++] = 0x20; // 
	cmd_inputBuffer[i++] = 0x01; // Parameter Total Length
	cmd_inputBuffer[i++] = 0x01; // Advertising_Enable
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	ResetEvent(hEventCmdFinished);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Set Advertise Enable sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Wait for the end of the Set Advertise Enable command\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Long wait for the Pairing Request\n");
	WaitForSingleObject(hEventAclDataReceived, 20000);
	parseAclData();

	printf("Short wait for an optional Exchange MTU Request\n");
	if (WaitForSingleObject(hEventAclDataReceived, 500) == WAIT_OBJECT_0)
	{
		parseAclData();
	}
	else
	{
		ResetEvent(hEventAclDataReceived);
		printf("No Exchange MTU Request received\n");
	}

	printf("Send Pairing Response\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x0F; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x02; // ACL data
	cmd_inputBuffer[i++] = connectionHandle[0];
	cmd_inputBuffer[i++] = connectionHandle[1];
	cmd_inputBuffer[i++] = 0x0B; // Length of the ACL message
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x07; // Length of the L2CAP message
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x06; // Security Manager Protocol
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x02; // Pairing Response
	cmd_inputBuffer[i++] = 0x03; // IO Capability: No Input, No Output
	cmd_inputBuffer[i++] = 0x00; // OOB Data Flags: OOB Auth. Data Not Present
	cmd_inputBuffer[i++] = 0x01; // AuthReq: 0x01, Bonding Flags: Bonding
	cmd_inputBuffer[i++] = 0x10; // Max Encryption Key Size
	cmd_inputBuffer[i++] = 0x04; // Initiator Key Distribution: Signature Key (CSRK)
	cmd_inputBuffer[i++] = 0x01; // Responder Key Distribution: Encryption Key (LTK)
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Pairing Response sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
		storePairingResponse(cmd_inputBuffer);
	}

	// Our(slave) "random" value (TODO: we can generate a new random value each time)
	BYTE sRand[] = {
		0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA,
		0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA
	};

	printf("Wait for the Connection Complete\n");
	WaitForSingleObject(hEventConnCmpltReceived, 1000);

	printf("Wait for the Pairing Confirm\n");
	WaitForSingleObject(hEventAclDataReceived, 1000);
	parseAclData();

	printf("Compute pairing confirm\n");
	BYTE confirmValue[16];
	computeConfirmValue(sRand, pairingRequest, pairingResponse, iat, ia, rat, ra, confirmValue);
	printBuffer2HexString(sRand, 16, FALSE, 'r');
	printBuffer2HexString(pairingRequest, 7, FALSE, 'p');
	printBuffer2HexString(pairingResponse, 7, FALSE, 'p');
	printf("iat=0x%02X\n", iat);
	printBuffer2HexString(ia, 6, FALSE, 'i');
	printf("rat=0x%02X\n", rat);
	printBuffer2HexString(ra, 6, FALSE, 'r');
	printBuffer2HexString(confirmValue, 16, FALSE, 'v');

	printf("Send Pairing Confirm\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x19; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x02; // ACL data
	cmd_inputBuffer[i++] = connectionHandle[0];
	cmd_inputBuffer[i++] = connectionHandle[1];
	cmd_inputBuffer[i++] = 0x15; // Length of the ACL message
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x11; // Length of the L2CAP message
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x06; // Security Manager Protocol
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x03; // Pairing Confirm
	cmd_inputBuffer[i++] = confirmValue[15]; // Confirm value
	cmd_inputBuffer[i++] = confirmValue[14]; //
	cmd_inputBuffer[i++] = confirmValue[13]; //
	cmd_inputBuffer[i++] = confirmValue[12]; //
	cmd_inputBuffer[i++] = confirmValue[11]; //
	cmd_inputBuffer[i++] = confirmValue[10]; //
	cmd_inputBuffer[i++] = confirmValue[9]; //
	cmd_inputBuffer[i++] = confirmValue[8]; //
	cmd_inputBuffer[i++] = confirmValue[7]; //
	cmd_inputBuffer[i++] = confirmValue[6]; //
	cmd_inputBuffer[i++] = confirmValue[5]; //
	cmd_inputBuffer[i++] = confirmValue[4]; //
	cmd_inputBuffer[i++] = confirmValue[3]; //
	cmd_inputBuffer[i++] = confirmValue[2]; //
	cmd_inputBuffer[i++] = confirmValue[1]; //
	cmd_inputBuffer[i++] = confirmValue[0]; //
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Pairing Confirm sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}


	printf("Wait for the Pairing Random\n");
	WaitForSingleObject(hEventAclDataReceived, 1000);
	parseAclData();

	printf("Send Pairing Random\n");
	i = 0; 
	cmd_inputBuffer[i++] = 0x19; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x02; // ACL data
	cmd_inputBuffer[i++] = connectionHandle[0];
	cmd_inputBuffer[i++] = connectionHandle[1];
	cmd_inputBuffer[i++] = 0x15; // Length of the ACL message
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x11; // Length of the L2CAP message
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x06; // Security Manager Protocol
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x04; // Pairing Random
	cmd_inputBuffer[i++] = sRand[15]; // Random value
	cmd_inputBuffer[i++] = sRand[14]; //
	cmd_inputBuffer[i++] = sRand[13]; //
	cmd_inputBuffer[i++] = sRand[12]; //
	cmd_inputBuffer[i++] = sRand[11]; //
	cmd_inputBuffer[i++] = sRand[10]; //
	cmd_inputBuffer[i++] = sRand[9]; //
	cmd_inputBuffer[i++] = sRand[8]; //
	cmd_inputBuffer[i++] = sRand[7]; //
	cmd_inputBuffer[i++] = sRand[6]; //
	cmd_inputBuffer[i++] = sRand[5]; //
	cmd_inputBuffer[i++] = sRand[4]; //
	cmd_inputBuffer[i++] = sRand[3]; //
	cmd_inputBuffer[i++] = sRand[2]; //
	cmd_inputBuffer[i++] = sRand[1]; //
	cmd_inputBuffer[i++] = sRand[0]; //
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Pairing Random sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	// Our LTK (Long Term Key) value (TODO: we can generate a new random value each time)
	BYTE ltkValue[] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
	};

	// Our EDIV (Encrypted Diversifier) value (TODO: we can generate a new random value each time)
	BYTE edivValue[] = {
		0xA0, 0xB0
	};

	// Our "random" value (TODO: we can generate a new random value each time)
	BYTE randomValue[] = {
		0XCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA
	};

	printf("Wait for the LTK Request\n");
	WaitForSingleObject(hEventLTKRqstReceived, 1000);

	printf("Compute STK\n");
	BYTE stkValue[16];
	printBuffer2HexString(sRand, 16, FALSE, 's');
	printBuffer2HexString(mRand, 16, FALSE, 'm');
	computeStk(sRand, mRand, stkValue);
	printBuffer2HexString(stkValue, 16, FALSE, 'k');

	printf("Send Long Term Key Request Reply command\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x15; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x01; // Command
	cmd_inputBuffer[i++] = 0x1A; // OGF 0x08, OCF 0x001A
	cmd_inputBuffer[i++] = 0x20; // 
	cmd_inputBuffer[i++] = 0x12; // Parameter Total Length
	cmd_inputBuffer[i++] = connectionHandle[0];
	cmd_inputBuffer[i++] = connectionHandle[1];
	cmd_inputBuffer[i++] = stkValue[15]; // Long Term Key (in fact the Short Term Key because we were not bond with the master device before)
	cmd_inputBuffer[i++] = stkValue[14]; //
	cmd_inputBuffer[i++] = stkValue[13]; //
	cmd_inputBuffer[i++] = stkValue[12]; //
	cmd_inputBuffer[i++] = stkValue[11]; //
	cmd_inputBuffer[i++] = stkValue[10]; //
	cmd_inputBuffer[i++] = stkValue[9]; //
	cmd_inputBuffer[i++] = stkValue[8]; //
	cmd_inputBuffer[i++] = stkValue[7]; //
	cmd_inputBuffer[i++] = stkValue[6]; //
	cmd_inputBuffer[i++] = stkValue[5]; //
	cmd_inputBuffer[i++] = stkValue[4]; //
	cmd_inputBuffer[i++] = stkValue[3]; //
	cmd_inputBuffer[i++] = stkValue[2]; //
	cmd_inputBuffer[i++] = stkValue[1]; //
	cmd_inputBuffer[i++] = stkValue[0]; //
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	ResetEvent(hEventCmdFinished);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Long Term Key Request Reply sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Wait for the end of the Long Term Key Request Reply command\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Send Encryption Information\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x19; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x02; // ACL data
	cmd_inputBuffer[i++] = connectionHandle[0];
	cmd_inputBuffer[i++] = connectionHandle[1];
	cmd_inputBuffer[i++] = 0x15; // Length of the ACL message
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x11; // Length of the L2CAP message
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x06; // Security Manager Protocol
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x06; // Encryption Information
	cmd_inputBuffer[i++] = ltkValue[15]; // LTK value
	cmd_inputBuffer[i++] = ltkValue[14]; //
	cmd_inputBuffer[i++] = ltkValue[13]; //
	cmd_inputBuffer[i++] = ltkValue[12]; //
	cmd_inputBuffer[i++] = ltkValue[11]; //
	cmd_inputBuffer[i++] = ltkValue[10]; //
	cmd_inputBuffer[i++] = ltkValue[9]; //
	cmd_inputBuffer[i++] = ltkValue[8]; //
	cmd_inputBuffer[i++] = ltkValue[7]; //
	cmd_inputBuffer[i++] = ltkValue[6]; //
	cmd_inputBuffer[i++] = ltkValue[5]; //
	cmd_inputBuffer[i++] = ltkValue[4]; //
	cmd_inputBuffer[i++] = ltkValue[3]; //
	cmd_inputBuffer[i++] = ltkValue[2]; //
	cmd_inputBuffer[i++] = ltkValue[1]; //
	cmd_inputBuffer[i++] = ltkValue[0]; //
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Encryption Information sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}


	printf("Send Master Identification\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x13; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x02; // ACL data
	cmd_inputBuffer[i++] = connectionHandle[0];
	cmd_inputBuffer[i++] = connectionHandle[1];
	cmd_inputBuffer[i++] = 0x0F; // Length of the ACL message
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x0B; // Length of the L2CAP message
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x06; // Security Manager Protocol
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x07; // Master Identification
	cmd_inputBuffer[i++] = edivValue[1]; // EDIV value
	cmd_inputBuffer[i++] = edivValue[0]; //
	cmd_inputBuffer[i++] = randomValue[7]; // Random value
	cmd_inputBuffer[i++] = randomValue[6]; //
	cmd_inputBuffer[i++] = randomValue[5]; //
	cmd_inputBuffer[i++] = randomValue[4]; //
	cmd_inputBuffer[i++] = randomValue[3]; //
	cmd_inputBuffer[i++] = randomValue[2]; //
	cmd_inputBuffer[i++] = randomValue[1]; //
	cmd_inputBuffer[i++] = randomValue[0]; //
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Master Identification sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Was an Exchange MTU Request received ?\n");
	if (isXchgMTURqstReceived == TRUE)
	{
		printf("Yes, send Exchange MTU Response\n");
		i = 0;
		cmd_inputBuffer[i++] = 0x0B; // Length of the IOCTL message
		cmd_inputBuffer[i++] = 0x00;
		cmd_inputBuffer[i++] = 0x00;
		cmd_inputBuffer[i++] = 0x00;
		cmd_inputBuffer[i++] = 0x02; // ACL data
		cmd_inputBuffer[i++] = connectionHandle[0];
		cmd_inputBuffer[i++] = connectionHandle[1];
		cmd_inputBuffer[i++] = 0x07; // Length of the ACL message
		cmd_inputBuffer[i++] = 0x00; // 
		cmd_inputBuffer[i++] = 0x03; // Length of the L2CAP message
		cmd_inputBuffer[i++] = 0x00; //
		cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
		cmd_inputBuffer[i++] = 0x00; //
		cmd_inputBuffer[i++] = 0x03; // Exchange MTU Response
		cmd_inputBuffer[i++] = mtu[0]; // EDIV value
		cmd_inputBuffer[i++] = mtu[1]; //
		printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		}
		else
		{
			printf("Exchange MTU Response sent\n");
			printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
		}
	}
	else
	{
		printf("No Exchange MTU Request received\n");
	}

	printf("Main loop...\n");
	int attRqstIndex = 0;
	while (mainLoop_continue)
	{
		if (pAttributeRequested[attRqstIndex]->opcode == 0x08		// Read By Type Request
			&& pAttributeRequested[attRqstIndex]->uuid[0] == 0x03	// GATT Characteristic Declaration
			&& pAttributeRequested[attRqstIndex]->uuid[1] == 0x28	//
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x01
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read By Type Response for GATT Characteristic Declaration\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x57; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x53; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x4F; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x09; // Read By Type Response
			cmd_inputBuffer[i++] = 0x07; // Size of each Attribute Data
			cmd_inputBuffer[i++] = 0x02; // AttributeData.Handle: 0x0002
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0A; // AttributeData.CharacteristicProperties: 0x0A = 0x08 Write | 0x02 Read
			cmd_inputBuffer[i++] = 0x03; // AttributeData.CharacteristicValueHandle : 0x0003
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x00; // AttributeData.CharacteristicUUID: Device Name
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x04; // AttributeData.Handle: 0x0004
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x02; // AttributeData.CharacteristicProperties: 0x02 Read
			cmd_inputBuffer[i++] = 0x05; // AttributeData.CharacteristicValueHandle : 0x0005
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x01; // AttributeData.CharacteristicUUID: Appearance
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x06; // AttributeData.Handle: 0x0006
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x02; // AttributeData.CharacteristicProperties: 0x02 Read
			cmd_inputBuffer[i++] = 0x07; // AttributeData.CharacteristicValueHandle : 0x0007
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // AttributeData.CharacteristicUUID: Peripheral Preferred Connection Parameters
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x0A; // AttributeData.Handle: 0x000A
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x02; // AttributeData.CharacteristicProperties: 0x02 Read
			cmd_inputBuffer[i++] = 0x0B; // AttributeData.CharacteristicValueHandle : 0x000B
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x29; // AttributeData.CharacteristicUUID: Manufacturer Name String
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x0C; // AttributeData.Handle: 0x000C
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x02; // AttributeData.CharacteristicProperties: 0x02 Read
			cmd_inputBuffer[i++] = 0x0D; // AttributeData.CharacteristicValueHandle : 0x000D
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x50; // AttributeData.CharacteristicUUID: PnP ID
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x0F; // AttributeData.Handle: 0x000F
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x06; // AttributeData.CharacteristicProperties: 0x06 = 0x04 Write Without Response | 0x02 Read
			cmd_inputBuffer[i++] = 0x10; // AttributeData.CharacteristicValueHandle : 0x0010
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x4E; // AttributeData.CharacteristicUUID: Protocol Mode
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x11; // AttributeData.Handle: 0x0011
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x1A; // AttributeData.CharacteristicProperties: 0x1A = 0x10 Notify | 0x08 Write | 0x02 Read
			cmd_inputBuffer[i++] = 0x12; // AttributeData.CharacteristicValueHandle : 0x0012
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x4D; // AttributeData.CharacteristicUUID: Report
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x15; // AttributeData.Handle: 0x0015
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x02; // AttributeData.CharacteristicProperties: 0x02 Read
			cmd_inputBuffer[i++] = 0x16; // AttributeData.CharacteristicValueHandle : 0x0016
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x4B; // AttributeData.CharacteristicUUID: Report Map
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x17; // AttributeData.Handle: 0x0017
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x1A; // AttributeData.CharacteristicProperties: 0x1A = 0x10 Notify | 0x08 Write | 0x02 Read
			cmd_inputBuffer[i++] = 0x18; // AttributeData.CharacteristicValueHandle : 0x0018
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x33; // AttributeData.CharacteristicUUID: Boot Mouse Input Report
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x1A; // AttributeData.Handle: 0x001A
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x02; // AttributeData.CharacteristicProperties: 0x02 Read
			cmd_inputBuffer[i++] = 0x1B; // AttributeData.CharacteristicValueHandle : 0x001B
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x4A; // AttributeData.CharacteristicUUID: HID Information
			cmd_inputBuffer[i++] = 0x2A; //
			cmd_inputBuffer[i++] = 0x1C; // AttributeData.Handle: 0x001C
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // AttributeData.CharacteristicProperties: 0x04 Write Without Response
			cmd_inputBuffer[i++] = 0x1D; // AttributeData.CharacteristicValueHandle : 0x001D
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x4C; // AttributeData.CharacteristicUUID: HID Control Point
			cmd_inputBuffer[i++] = 0x2A; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read By Type Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x10	// Read By Group Type Request
			&& pAttributeRequested[attRqstIndex]->uuid[0] == 0x00	// GATT Primary Service Declaration
			&& pAttributeRequested[attRqstIndex]->uuid[1] == 0x28	//
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x01
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read By Group Type Response for GATT Primary Service Declaration\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x22; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x1E; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x1A; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x11; // Read By Group Type Response
			cmd_inputBuffer[i++] = 0x06; // Size of each Attribute Data
			cmd_inputBuffer[i++] = 0x01; // AttributeData.Handle: 0x0001
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x07; // AttributeData.GroupEndHandle: 0x0007
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x00; // AttributeData.UUID: Generic Access Profile
			cmd_inputBuffer[i++] = 0x18; //
			cmd_inputBuffer[i++] = 0x08; // AttributeData.Handle: 0x0008
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x08; // AttributeData.GroupEndHandle: 0x0008
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x01; // AttributeData.UUID: Generic Attribute Profile
			cmd_inputBuffer[i++] = 0x18; //
			cmd_inputBuffer[i++] = 0x09; // AttributeData.Handle: 0x0009
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0D; // AttributeData.GroupEndHandle: 0x000D
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0A; // AttributeData.UUID: Device Information
			cmd_inputBuffer[i++] = 0x18; //
			cmd_inputBuffer[i++] = 0x0E; // AttributeData.Handle: 0x000E
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0xFF; // AttributeData.GroupEndHandle: 0xFFFF
			cmd_inputBuffer[i++] = 0xFF; //
			cmd_inputBuffer[i++] = 0x12; // AttributeData.UUID: Human Interface Device
			cmd_inputBuffer[i++] = 0x18; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read By Group Type Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x06	// Find By Type Value Request
			&& pAttributeRequested[attRqstIndex]->uuid[0] == 0x00	// GATT Primary Service Declaration
			&& pAttributeRequested[attRqstIndex]->uuid[1] == 0x28	//
			&& pAttributeRequested[attRqstIndex]->value[0] == 0x01	// Generic Attribute Profile
			&& pAttributeRequested[attRqstIndex]->value[1] == 0x18	//
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x01
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Find By Type Value Response for type GATT Primary Service Declaration and value Generic Attribute Profile\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x0D; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x09; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x05; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x07; // Find By Type Value Response
			cmd_inputBuffer[i++] = 0x08; // AttributeData.Handle: 0x0008
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x08; // AttributeData.GroupEndHandle: 0x0008
			cmd_inputBuffer[i++] = 0x00; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Find By Type Value Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x04 	// Find Information Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x13
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00
			&& pAttributeRequested[attRqstIndex]->endingHandle[0] == 0x14
			&& pAttributeRequested[attRqstIndex]->endingHandle[1] == 0x00)
		{
			printf("Find Information Response for Report\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x12; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x0E; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x0A; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x05; // Find Information Response
			cmd_inputBuffer[i++] = 0x01; // Format : Handle(s) and 16-bit Bluetooth	UUID(s)
			cmd_inputBuffer[i++] = 0x13; // InformationData.Handle: 0x0013
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x02; // InformationData.UUID: Client Characteristic Configuration
			cmd_inputBuffer[i++] = 0x29; //
			cmd_inputBuffer[i++] = 0x14; // InformationData.Handle: 0x0014
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x08; // InformationData.UUID: Report Reference
			cmd_inputBuffer[i++] = 0x29; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Find Information Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if(pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x03
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Generic Access Profile: Device Name\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x16; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x12; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x0E; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			cmd_inputBuffer[i++] = 0x4C; // Device Name=LumiaMouse520
			cmd_inputBuffer[i++] = 0x75; // 
			cmd_inputBuffer[i++] = 0x6D; // 
			cmd_inputBuffer[i++] = 0x69; // 
			cmd_inputBuffer[i++] = 0x61; // 
			cmd_inputBuffer[i++] = 0x4D; //
			cmd_inputBuffer[i++] = 0x6F; //
			cmd_inputBuffer[i++] = 0x75; //
			cmd_inputBuffer[i++] = 0x73; //
			cmd_inputBuffer[i++] = 0x65; //
			cmd_inputBuffer[i++] = 0x35; // 
			cmd_inputBuffer[i++] = 0x32; //
			cmd_inputBuffer[i++] = 0x30; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x0D
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Device Information: PnP ID\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x10; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x0C; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x08; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			cmd_inputBuffer[i++] = 0x02; // Vendor ID Source = USB Implementer's Forum
			cmd_inputBuffer[i++] = 0x5E; // Vendor ID: Microsoft Corp.
			cmd_inputBuffer[i++] = 0x04; // 
			cmd_inputBuffer[i++] = 0x16; // Product ID
			cmd_inputBuffer[i++] = 0x09; // 
			cmd_inputBuffer[i++] = 0x10; // Version
			cmd_inputBuffer[i++] = 0x01; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x10
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Human Interface Device: Protocol Mode\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x0A; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x06; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x02; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			cmd_inputBuffer[i++] = 0x01; // Protocol Mode: Report Protocol Mode
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x12
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Human Interface Device: Report\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x09; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x05; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x01; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x13
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Human Interface Device: Report: Client Characteristic Configuration\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x0B; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x07; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x03; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			cmd_inputBuffer[i++] = 0x01; // Characteristic Configuration Client: 0x0001, Notification
			cmd_inputBuffer[i++] = 0x00; // 
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x14
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Human Interface Device: Report: Report Reference\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x0B; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x07; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x03; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			cmd_inputBuffer[i++] = 0x1A; // Report ID
			cmd_inputBuffer[i++] = 0x01; // Report Type: Input Report
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x16
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Human Interface Device: Report Map\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x73; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x6F; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x6B; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			cmd_inputBuffer[i++] = 0x05; // Usage Page (Generic Desktop Ctrls)
			cmd_inputBuffer[i++] = 0x01; // 
			cmd_inputBuffer[i++] = 0x09; // Usage (Mouse)
			cmd_inputBuffer[i++] = 0x02; // 
			cmd_inputBuffer[i++] = 0xA1; // Collection (Application)
			cmd_inputBuffer[i++] = 0x01; // 
			cmd_inputBuffer[i++] = 0x05; //   Usage Page (Generic Desktop Ctrls)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x09; //   Usage (Mouse)
			cmd_inputBuffer[i++] = 0x02; //
			cmd_inputBuffer[i++] = 0xA1; //   Collection (Logical)
			cmd_inputBuffer[i++] = 0x02; //
			cmd_inputBuffer[i++] = 0x85; //     Report ID (0x1A)
			cmd_inputBuffer[i++] = 0x1A; //
			cmd_inputBuffer[i++] = 0x09; //     Usage (Pointer)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0xA1; //     Collection (Physical)
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x05; //       Usage Page (Button)
			cmd_inputBuffer[i++] = 0x09; //
			cmd_inputBuffer[i++] = 0x19; //       Usage Minimum (0x01)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x29; //       Usage Maximum (0x05)  	-> 5 buttons
			cmd_inputBuffer[i++] = 0x05; //
			cmd_inputBuffer[i++] = 0x95; //       Report Count (5)			-> //
			cmd_inputBuffer[i++] = 0x05; //
			cmd_inputBuffer[i++] = 0x75; //       Report Size (1)			-> 1 bit per button
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x15; //       Logical Minimum (0)		-> binary buttons (value=0 or 1)
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x25; //       Logical Maximum (1)		-> //
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x81; //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
			cmd_inputBuffer[i++] = 0x02; //
			cmd_inputBuffer[i++] = 0x75; //       Report Size (3)			-> 3 bits constant field
			cmd_inputBuffer[i++] = 0x03; //
			cmd_inputBuffer[i++] = 0x95; //       Report Count (1)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x81; //       Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x05; //       Usage Page (Generic Desktop Ctrls)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x09; //       Usage (X)
			cmd_inputBuffer[i++] = 0x30; //
			cmd_inputBuffer[i++] = 0x09; //       Usage (Y)
			cmd_inputBuffer[i++] = 0x31; //
			cmd_inputBuffer[i++] = 0x95; //       Report Count (2)
			cmd_inputBuffer[i++] = 0x02; //
			cmd_inputBuffer[i++] = 0x75; //       Report Size (16)
			cmd_inputBuffer[i++] = 0x10; //
			cmd_inputBuffer[i++] = 0x16; //       Logical Minimum (-32767)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x80; //
			cmd_inputBuffer[i++] = 0x26; //       Logical Maximum (32767)
			cmd_inputBuffer[i++] = 0xFF; //
			cmd_inputBuffer[i++] = 0x7F; //
			cmd_inputBuffer[i++] = 0x81; //       Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
			cmd_inputBuffer[i++] = 0x06; //
			cmd_inputBuffer[i++] = 0xA1; //       Collection (Logical)
			cmd_inputBuffer[i++] = 0x02; //
			cmd_inputBuffer[i++] = 0x85; //         Report ID (0x1A)
			cmd_inputBuffer[i++] = 0x1A; //
			cmd_inputBuffer[i++] = 0x09; //         Usage (Wheel)
			cmd_inputBuffer[i++] = 0x38; //
			cmd_inputBuffer[i++] = 0x35; //         Physical Minimum (0)
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x45; //         Physical Maximum (0)
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x95; //         Report Count (1)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x75; //         Report Size (16)
			cmd_inputBuffer[i++] = 0x10; //
			cmd_inputBuffer[i++] = 0x16; //         Logical Minimum (-32767)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x80; //
			cmd_inputBuffer[i++] = 0x26; //         Logical Maximum (32767)
			cmd_inputBuffer[i++] = 0xFF; //
			cmd_inputBuffer[i++] = 0x7F; //
			cmd_inputBuffer[i++] = 0x81; //         Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
			cmd_inputBuffer[i++] = 0x06; //
			cmd_inputBuffer[i++] = 0xC0; //       End Collection
			cmd_inputBuffer[i++] = 0xA1; //       Collection (Logical)
			cmd_inputBuffer[i++] = 0x02; //
			cmd_inputBuffer[i++] = 0x85; //         Report ID (0x1A)
			cmd_inputBuffer[i++] = 0x1A; //
			cmd_inputBuffer[i++] = 0x05; //         Usage Page (Consumer)
			cmd_inputBuffer[i++] = 0x0D; //
			cmd_inputBuffer[i++] = 0x95; //         Report Count (1)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x75; //         Report Size (16)
			cmd_inputBuffer[i++] = 0x10; //
			cmd_inputBuffer[i++] = 0x16; //         Logical Minimum (-32767)
			cmd_inputBuffer[i++] = 0x01; //
			cmd_inputBuffer[i++] = 0x80; //
			cmd_inputBuffer[i++] = 0x26; //         Logical Maximum (32767)
			cmd_inputBuffer[i++] = 0xFF; //
			cmd_inputBuffer[i++] = 0x7F; //
			cmd_inputBuffer[i++] = 0x0A; //         Usage (AC Pan)		-> Horizontal wheel
			cmd_inputBuffer[i++] = 0x38; //
			cmd_inputBuffer[i++] = 0x02; //
			cmd_inputBuffer[i++] = 0x81; //         Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
			cmd_inputBuffer[i++] = 0x06; //
			cmd_inputBuffer[i++] = 0xC0; //       End Collection
			cmd_inputBuffer[i++] = 0xC0; //     End Collection
			cmd_inputBuffer[i++] = 0xC0; //   End Collection
			cmd_inputBuffer[i++] = 0xC0; // End Collection
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
				break; // start to send notifications
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x0A // Read Request
			&& pAttributeRequested[attRqstIndex]->startingHandle[0] == 0x1B
			&& pAttributeRequested[attRqstIndex]->startingHandle[1] == 0x00)
		{
			printf("Read Response for Human Interface Device: HID Information\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x0D; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x09; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x05; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			cmd_inputBuffer[i++] = 0x01; // bcdHID
			cmd_inputBuffer[i++] = 0x01; // 
			cmd_inputBuffer[i++] = 0x00; // bCountryCode: Not Supported
			cmd_inputBuffer[i++] = 0x03; // Flags: 0x03, Normally Connectable, Remote Wake
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Read Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode == 0x12) // Write Request
		{
			// We always acknowledge Write Requests
			printf("Write Response\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x09; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x05; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x01; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x13; // Write Response
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Write Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested[attRqstIndex]->opcode != 0x00)
		{
			// Unknown attribute
			printf("Send Error Response\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x0D; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x09; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x05; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x01; // Error Response
			cmd_inputBuffer[i++] = pAttributeRequested[attRqstIndex]->opcode; // Request Opcode in Error
			cmd_inputBuffer[i++] = pAttributeRequested[attRqstIndex]->startingHandle[0]; // Attribute Handle In Error
			cmd_inputBuffer[i++] = pAttributeRequested[attRqstIndex]->startingHandle[1]; //
			cmd_inputBuffer[i++] = 0x0A; // Error Code = Attribute Not Found
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
			}
			else
			{
				printf("Error Response sent\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}

		

		printf("Wait for the new ACL data\n");
		if (WaitForSingleObject(hEventAclDataReceived, 20000) == WAIT_OBJECT_0)
		{
			parseAclData();
		}
		else
		{
			ResetEvent(hEventAclDataReceived);
			pAttributeRequested[attRqstIndex]->opcode = 0x00;
		}


	}


	while (mainLoop_continue)
	{
		Sleep(1000);
		printf("Handle Value Notification\n");
		i = 0;
		cmd_inputBuffer[i++] = 0x14; // Length of the IOCTL message
		cmd_inputBuffer[i++] = 0x00;
		cmd_inputBuffer[i++] = 0x00;
		cmd_inputBuffer[i++] = 0x00;
		cmd_inputBuffer[i++] = 0x02; // ACL data
		cmd_inputBuffer[i++] = connectionHandle[0];
		cmd_inputBuffer[i++] = connectionHandle[1];
		cmd_inputBuffer[i++] = 0x10; // Length of the ACL message
		cmd_inputBuffer[i++] = 0x00; // 
		cmd_inputBuffer[i++] = 0x0C; // Length of the L2CAP message
		cmd_inputBuffer[i++] = 0x00; //
		cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
		cmd_inputBuffer[i++] = 0x00; //
		cmd_inputBuffer[i++] = 0x1B; // Write Response
		cmd_inputBuffer[i++] = 0x12; // Attribute Handle
		cmd_inputBuffer[i++] = 0x00; //
		cmd_inputBuffer[i++] = 0x00; // 5 Buttons 0b00011111
		cmd_inputBuffer[i++] = 0x01; // lsb X
		cmd_inputBuffer[i++] = 0x00; // msb X
		cmd_inputBuffer[i++] = 0x00; // lsb Y
		cmd_inputBuffer[i++] = 0x00; // msb Y
		cmd_inputBuffer[i++] = 0x00; // lsb Wheel
		cmd_inputBuffer[i++] = 0x00; // msb Wheel
		cmd_inputBuffer[i++] = 0x00; // lsb Horizontal Wheel
		cmd_inputBuffer[i++] = 0x00; // msb Horizontal Wheel
		printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		}
		else
		{
			printf("Write Response sent\n");
			printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
		}
	}

	//printf("Send LE Set Advertise Disable Command\n");
	//i = 0;
	//cmd_inputBuffer[i++] = 0x04; // Length of the IOCTL message
	//cmd_inputBuffer[i++] = 0x00;
	//cmd_inputBuffer[i++] = 0x00;
	//cmd_inputBuffer[i++] = 0x00;
	//cmd_inputBuffer[i++] = 0x01; // Command
	//cmd_inputBuffer[i++] = 0x0A; // OGF 0x08, OCF 0x000A
	//cmd_inputBuffer[i++] = 0x20; // 
	//cmd_inputBuffer[i++] = 0x01; // Parameter Total Length
	//cmd_inputBuffer[i++] = 0x00; // Advertising_Disable
	//printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	//success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	//if (!success)
	//{
	//	printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
	//}
	//else
	//{
	//	printf("Set Advertise Disable sent\n");
	//	printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	//	ResetEvent(hEventCmdFinished);
	//}

	//printf("Wait for the end of the Set Advertise Disable command\n");
	//WaitForSingleObject(hEventCmdFinished, 10000);

	readLoop_continue = FALSE;
	printf("Stop then start the Bluetooth of the phone to exit this program.\n");

	// Wait for the end of the "read events" and "read ACL data" threads.
	WaitForSingleObject(hThreadEvent, INFINITE);
	WaitForSingleObject(hThreadAclData, INFINITE);
	WaitForSingleObject(hThreadAclData2, INFINITE);
	CloseHandle(hThreadEvent);
	CloseHandle(hThreadAclData);
	CloseHandle(hThreadAclData2);
	CloseHandle(hEventCmdFinished);
	CloseHandle(hEventAclDataReceived);

	//cmd_inputBuffer[0] = 0; // Unblock IOCTL_BTHX_WRITE_HCI and IOCTL_BTHX_READ_HCI coming from the Windows Bluetooth stack.
	//success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_CMD, cmd_inputBuffer, 1, NULL, 0, &returned, NULL);
	//if (!success)
	//{
	//	printf("Failed to send DeviceIoControl! 0x%08X", GetLastError());
	//	return EXIT_FAILURE;
	//}

	CloseHandle(hciControlDeviceCmd);
	CloseHandle(hLogFile);
	free(cmd_inputBuffer);
	free(cmd_outputBuffer);
	free(pAttributeRequested[0]);
	free(pAttributeRequested[1]);
	free(aclData[0]);
	free(aclData[1]);
	free(aclData[2]);

    return 0;
}

