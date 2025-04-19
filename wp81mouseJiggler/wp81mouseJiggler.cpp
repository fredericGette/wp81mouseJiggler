// wp81mouseJiggler.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

typedef struct _AttributeRequested {
	BYTE opcode;
	BYTE startingHandle[2];
	BYTE endingHandle[2];
	BYTE uuid[2];
	BYTE value[2];
	BYTE valueOffset[2];
} AttributeRequested;

typedef struct _AttributeData {
	BYTE handle[2];
	BYTE endGroupHandle[2];
	BYTE characteristicProperties;
	BYTE characteristicValueHandle[2];
	BYTE UUID16bits[2];
} AttributeData;

typedef struct _MouseData {
	BYTE xlsb;
	BYTE xmsb;
	BYTE ylsb;
	BYTE ymsb;
} MouseData;

static HANDLE hEventCmdFinished = NULL;
static HANDLE hLogFile;
static BOOL readLoop_continue;
static BOOL mainLoop_continue;
static BYTE connectionHandle[2];
static HANDLE hEventConnCmpltReceived = NULL;
static HANDLE hEventLTKRqstReceived = NULL;
static HANDLE hEventAclDataReceived = NULL;
static BYTE pairingRequest[7]; // MSB first
static BYTE pairingResponse[7]; // MSB first
static BYTE iat; // Initiating device address type
static BYTE ia[6]; // Initiating device address (MSB first)
static BYTE rat = 0x00; // Responding device address type = public
static BYTE ra[6]; // Responding device address 
static BYTE mRand[16]; // Pairing random send by the initiator/master device (MSB first)
static BYTE mtu[2] = {23,0}; // MTU common to initiating and responding device (default minimum = 23 bytes)
static AttributeRequested* pAttributeRequested;
static BYTE* aclData[2];
static int activeAclData;
static int passiveAclData;
static DWORD aclDataSize[2];
static BYTE* attMsgReceived; // Last Attribut Protocol message received
static BYTE* sigMsgReceived; // Last Signaling Protocol message received
static BYTE* secMsgReceived; // Last Security Manager Protocol message received


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
	//printf("%s", temp);

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

void storeRespondingDeviceInformation(BYTE* evtReadBdAddrCmdCompltMsgReceived)
{
	for (int i = 0; i < 6; i++)
	{
		ra[i] = evtReadBdAddrCmdCompltMsgReceived[16 - i];
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
				printf("Received: Command complete.\n");
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 17 && readEvent_outputBuffer[5] == 0x0E && readEvent_outputBuffer[8] == 0x09 && readEvent_outputBuffer[9] == 0x10)
			{
				printf("Received: Read BD ADDR Command complete.\n");
				storeRespondingDeviceInformation(readEvent_outputBuffer);
				SetEvent(hEventCmdFinished);
			}
			else if (returned == 26 && readEvent_outputBuffer[5] == 0x3E && readEvent_outputBuffer[7] == 0x01)
			{
				printf("Received: Connection complete.\n");
				storeInitiatingDeviceInformation(readEvent_outputBuffer);
				SetEvent(hEventConnCmpltReceived);
			}
			else if (returned == 17 && readEvent_outputBuffer[5] == 0x3E && readEvent_outputBuffer[7] == 0x03)
			{
				printf("Received: Connection update complete.\n");
			}
			else if (returned == 20 && readEvent_outputBuffer[5] == 0x3E && readEvent_outputBuffer[7] == 0x05)
			{
				printf("Received: LTK Request.\n");
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
	pAttributeRequested->opcode = readAttributeRequestReceived[13];
	pAttributeRequested->startingHandle[0] = readAttributeRequestReceived[14];
	pAttributeRequested->startingHandle[1] = readAttributeRequestReceived[15];
	pAttributeRequested->endingHandle[0] = readAttributeRequestReceived[16];
	pAttributeRequested->endingHandle[1] = readAttributeRequestReceived[17];
	pAttributeRequested->uuid[0] = readAttributeRequestReceived[18];
	pAttributeRequested->uuid[1] = readAttributeRequestReceived[19];
}

// Find By Type Value Request
void storeFindAttributeRequest(BYTE* findAttributeRequestReceived)
{
	pAttributeRequested->opcode = findAttributeRequestReceived[13];
	pAttributeRequested->startingHandle[0] = findAttributeRequestReceived[14];
	pAttributeRequested->startingHandle[1] = findAttributeRequestReceived[15];
	pAttributeRequested->endingHandle[0] = findAttributeRequestReceived[16];
	pAttributeRequested->endingHandle[1] = findAttributeRequestReceived[17];
	pAttributeRequested->uuid[0] = findAttributeRequestReceived[18];
	pAttributeRequested->uuid[1] = findAttributeRequestReceived[19];
	pAttributeRequested->value[0] = findAttributeRequestReceived[20];
	pAttributeRequested->value[1] = findAttributeRequestReceived[21];
}

// Find Information Request
void storeFindInformationRequest(BYTE* findInfoRequestReceived)
{
	pAttributeRequested->opcode = findInfoRequestReceived[13];
	pAttributeRequested->startingHandle[0] = findInfoRequestReceived[14];
	pAttributeRequested->startingHandle[1] = findInfoRequestReceived[15];
	pAttributeRequested->endingHandle[0] = findInfoRequestReceived[16];
	pAttributeRequested->endingHandle[1] = findInfoRequestReceived[17];
}

// Read Request
void storeReadRequest(BYTE* readRequestReceived)
{
	pAttributeRequested->opcode = readRequestReceived[13];
	pAttributeRequested->startingHandle[0] = readRequestReceived[14];
	pAttributeRequested->startingHandle[1] = readRequestReceived[15];
}

// Read Blob Request
void storeReadBlobRequest(BYTE* readRequestReceived)
{
	pAttributeRequested->opcode = readRequestReceived[13];
	pAttributeRequested->startingHandle[0] = readRequestReceived[14];
	pAttributeRequested->startingHandle[1] = readRequestReceived[15];
	pAttributeRequested->valueOffset[0] = readRequestReceived[16];
	pAttributeRequested->valueOffset[1] = readRequestReceived[17];
}

// Write Request
void storeWriteRequest(BYTE* writeRequestReceived)
{
	pAttributeRequested->opcode = writeRequestReceived[13];
	pAttributeRequested->startingHandle[0] = writeRequestReceived[14];
	pAttributeRequested->startingHandle[1] = writeRequestReceived[15];
}

BOOL isExchangeMTURequestReceived()
{
	if (attMsgReceived[0] == 0x0B
		&& attMsgReceived[13] == 0x02 // Exchange MTU Request
		)
		return TRUE;
	return FALSE;
}

BOOL isReadByTypeRequestReceived()
{
	if (attMsgReceived[0] == 0x0F
		&& attMsgReceived[13] == 0x08 // Read By Type Request
		)
		return TRUE;
	return FALSE;
}

BOOL isReadByTypeRequestReceivedAndAttributeFound(BYTE uuid0, BYTE uuid1, AttributeData* attributes, int nbAttributes)
{
	if (isReadByTypeRequestReceived())
	{
		if (attMsgReceived[18] == uuid0 && attMsgReceived[19] == uuid1)
		{
			for (int idx = 0; idx < nbAttributes; idx++)
			{
				if (attributes[idx].handle[0] >= attMsgReceived[14]		// startingHandle0
					&& attributes[idx].handle[1] >= attMsgReceived[15]	// startingHandle1
					&& attributes[idx].handle[0] <= attMsgReceived[16]	// endingHandle0
					&& attributes[idx].handle[1] <= attMsgReceived[17])	// endingHandle1
				{
					return TRUE;
					break;
				}
			}
		}	
	}
		
	return FALSE;
}

BOOL isReadByGroupTypeRequestReceived()
{
	if (attMsgReceived[0] == 0x0F
		&& attMsgReceived[13] == 0x10 // Read By Group Type Request
		)
		return TRUE;
	return FALSE;
}

BOOL isFindByTypeValueRequestReceived()
{
	if (attMsgReceived[0] == 0x11
		&& attMsgReceived[13] == 0x06 // Find By Type Value Request
		)
		return TRUE;
	return FALSE;
}

BOOL isFindInformationRequestReceived()
{
	if (attMsgReceived[0] == 0x0D
		&& attMsgReceived[13] == 0x04 // Find Information Request
		)
		return TRUE;
	return FALSE;
}

BOOL isReadRequestReceived()
{
	if (attMsgReceived[0] == 0x0B
		&& attMsgReceived[13] == 0x0A // Read Request
		)
		return TRUE;
	return FALSE;
}

BOOL isReadBlobRequestReceived()
{
	if (attMsgReceived[0] == 0x0D
		&& attMsgReceived[13] == 0x0C // Read Blob Request
		)
		return TRUE;
	return FALSE;
}

BOOL isWriteRequestReceived()
{
	if (attMsgReceived[0] >= 0x0B
		&& attMsgReceived[13] == 0x12 // Write Request
		)
		return TRUE;
	return FALSE;
}

BOOL isPairingRequestReceived()
{
	if (secMsgReceived[0] == 0x0F
		&& secMsgReceived[13] == 0x01 // Pairing Request
		)
		return TRUE;
	return FALSE;
}

BOOL isPairingConfirmReceived()
{
	if (secMsgReceived[0] == 0x19
		&& secMsgReceived[13] == 0x03 // Pairing confirm
		)
		return TRUE;
	return FALSE;
}

BOOL isPairingRandomReceived()
{
	if (secMsgReceived[0] == 0x19
		&& secMsgReceived[13] == 0x04 // Pairing Random
		)
		return TRUE;
	return FALSE;
}

BOOL isSigningInformationReceived()
{
	if (secMsgReceived[0] == 0x19
		&& secMsgReceived[13] == 0x0A // Signing Information
		)
		return TRUE;
	return FALSE;
}

BOOL isConnectionParameterUpdateResponseReceived()
{
	if (sigMsgReceived[0] == 0x0E
		&& sigMsgReceived[13] == 0x13 // Connection Parameter Update Response
		)
		return TRUE;
	return FALSE;
}

void parseAclData()
{
	BYTE* readAcl_outputBuffer = aclData[activeAclData];
	DWORD returned = aclDataSize[activeAclData];

	ResetEvent(hEventAclDataReceived);

	printBuffer2HexString(readAcl_outputBuffer, returned, TRUE, 'a');

	if (returned >= 12 && readAcl_outputBuffer[4] == 0x02) // ACL message
	{
		switch (readAcl_outputBuffer[11])
		{
		case 0x04: // Attribute Protocol
			memcpy_s(attMsgReceived, 1024, readAcl_outputBuffer, returned);
			if (isExchangeMTURequestReceived())
			{
				printf("Received: Exchange MTU Request.\n");
				//storeMTUReceived(readAcl_outputBuffer); We keep our 23 bytes MTU to be compatible with some clients expecting a small MTU from a mouse.
			}
			else if (isReadByTypeRequestReceived())
			{
				printf("Received: Read By Type Request (2 bytes UUID).\n");
				storeReadAttributeRequest(readAcl_outputBuffer);
			}
			else if (isReadByGroupTypeRequestReceived())
			{
				printf("Received: Read By Group Type Request (2 bytes UUID).\n");
				storeReadAttributeRequest(readAcl_outputBuffer);
			}
			else if (isFindByTypeValueRequestReceived())
			{
				printf("Received: Find By Type Value Request.\n");
				storeFindAttributeRequest(readAcl_outputBuffer);
			}
			else if (isFindInformationRequestReceived())
			{
				printf("Received: Find Information Request.\n");
				storeFindInformationRequest(readAcl_outputBuffer);
			}
			else if (isReadRequestReceived())
			{
				printf("Received: Read Request.\n");
				storeReadRequest(readAcl_outputBuffer);
			}
			else if (isReadBlobRequestReceived())
			{
				printf("Received: Read Blob Request.\n");
				storeReadBlobRequest(readAcl_outputBuffer);
			}
			else if (isWriteRequestReceived())
			{
				printf("Received: Write Request.\n");
				storeWriteRequest(readAcl_outputBuffer);
			}
			break;
		case 0x05: // Signaling Protocol
			memcpy_s(sigMsgReceived, 1024, readAcl_outputBuffer, returned);
			if (isConnectionParameterUpdateResponseReceived())
			{
				printf("Received: Connection Parameter Update Response.\n");
			}
			break;
		case 0x06: // Security Manager Protocol
			memcpy_s(secMsgReceived, 1024, readAcl_outputBuffer, returned);
			if (isPairingRequestReceived())
			{
				printf("Received: Pairing Request.\n");
				storeConnectionHandle(readAcl_outputBuffer);
				storePairingRequest(readAcl_outputBuffer);
			}
			else if (isPairingConfirmReceived())
			{
				printf("Received: Pairing Confirm.\n");
			}
			else if (isPairingRandomReceived())
			{
				printf("Received: Pairing Random.\n");
				storePairingRandomReceived(readAcl_outputBuffer);
			}
			else if (isSigningInformationReceived())
			{
				printf("Received: Signing Information.\n");
			}
			break;
		}
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

BOOL isAttributeInRange(AttributeData* attributes, int length)
{
	BOOL found = FALSE;

	for (int idx = 0; idx < length; idx++)
	{
		if (attributes[idx].handle[0] >= pAttributeRequested->startingHandle[0]
			&& attributes[idx].handle[1] >= pAttributeRequested->startingHandle[1]
			&& attributes[idx].handle[0] <= pAttributeRequested->endingHandle[0]
			&& attributes[idx].handle[1] <= pAttributeRequested->endingHandle[1])
		{
			found = TRUE;
			break;
		}
	}

	return found;
}

int sendConnectionParameterUpdateRequest(HANDLE hciControlDeviceCmd, BYTE* cmd_inputBuffer, BYTE* cmd_outputBuffer)
{
	BOOL success;
	DWORD returned;
	int i;

	printf("Sending Connection Parameter Update Request...\n");
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
	cmd_inputBuffer[i++] = 0x05; // Signaling Protocol
	cmd_inputBuffer[i++] = 0x00; //
	cmd_inputBuffer[i++] = 0x12; // Connection Parameter Update Request
	cmd_inputBuffer[i++] = 0x01; // Identifier
	cmd_inputBuffer[i++] = 0x08; // Length
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x06; // Interval Min
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x06; // Interval Max
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0x00; // Slave Latency
	cmd_inputBuffer[i++] = 0x00; // 
	cmd_inputBuffer[i++] = 0xE8; // Timeout Multiplier
	cmd_inputBuffer[i++] = 0x03; //
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		return 1;
	}
	else
	{
		printf("Connection Parameter Update Request sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	return 0;
}

void initData(AttributeData* characteristics, AttributeData* services, AttributeData* informations, BYTE* reportMap, MouseData* mouseData)
{
	characteristics[0].handle[0] = 0x02;
	characteristics[0].handle[1] = 0x00;
	characteristics[0].characteristicProperties = 0x0A; // 0x08 Write | 0x02 Read
	characteristics[0].characteristicValueHandle[0] = 0x03;
	characteristics[0].characteristicValueHandle[1] = 0x00;
	characteristics[0].UUID16bits[0] = 0x00; // Device Name
	characteristics[0].UUID16bits[1] = 0x2A; //
	characteristics[1].handle[0] = 0x04;
	characteristics[1].handle[1] = 0x00;
	characteristics[1].characteristicProperties = 0x02; // 0x02 Read
	characteristics[1].characteristicValueHandle[0] = 0x05;
	characteristics[1].characteristicValueHandle[1] = 0x00;
	characteristics[1].UUID16bits[0] = 0x01; // Appearance
	characteristics[1].UUID16bits[1] = 0x2A; //
	characteristics[2].handle[0] = 0x06;
	characteristics[2].handle[1] = 0x00;
	characteristics[2].characteristicProperties = 0x02; // 0x02 Read
	characteristics[2].characteristicValueHandle[0] = 0x07;
	characteristics[2].characteristicValueHandle[1] = 0x00;
	characteristics[2].UUID16bits[0] = 0x04; // Peripheral Preferred Connection Parameters
	characteristics[2].UUID16bits[1] = 0x2A; //
	characteristics[3].handle[0] = 0x0A;
	characteristics[3].handle[1] = 0x00;
	characteristics[3].characteristicProperties = 0x02; // 0x02 Read
	characteristics[3].characteristicValueHandle[0] = 0x0B;
	characteristics[3].characteristicValueHandle[1] = 0x00;
	characteristics[3].UUID16bits[0] = 0x29; // Manufacturer Name String
	characteristics[3].UUID16bits[1] = 0x2A; //
	characteristics[4].handle[0] = 0x0C;
	characteristics[4].handle[1] = 0x00;
	characteristics[4].characteristicProperties = 0x02; // 0x02 Read
	characteristics[4].characteristicValueHandle[0] = 0x0D;
	characteristics[4].characteristicValueHandle[1] = 0x00;
	characteristics[4].UUID16bits[0] = 0x50; // PnP ID
	characteristics[4].UUID16bits[1] = 0x2A; //
	characteristics[5].handle[0] = 0x0F;
	characteristics[5].handle[1] = 0x00;
	characteristics[5].characteristicProperties = 0x06; // 0x06 = 0x04 Write Without Response | 0x02 Read
	characteristics[5].characteristicValueHandle[0] = 0x10;
	characteristics[5].characteristicValueHandle[1] = 0x00;
	characteristics[5].UUID16bits[0] = 0x4E; // Protocol Mode
	characteristics[5].UUID16bits[1] = 0x2A; //
	characteristics[6].handle[0] = 0x11;
	characteristics[6].handle[1] = 0x00;
	characteristics[6].characteristicProperties = 0x1A; // 0x1A = 0x10 Notify | 0x08 Write | 0x02 Read
	characteristics[6].characteristicValueHandle[0] = 0x12;
	characteristics[6].characteristicValueHandle[1] = 0x00;
	characteristics[6].UUID16bits[0] = 0x4D; // Report
	characteristics[6].UUID16bits[1] = 0x2A; //
	characteristics[7].handle[0] = 0x15;
	characteristics[7].handle[1] = 0x00;
	characteristics[7].characteristicProperties = 0x02; // 0x02 Read
	characteristics[7].characteristicValueHandle[0] = 0x16;
	characteristics[7].characteristicValueHandle[1] = 0x00;
	characteristics[7].UUID16bits[0] = 0x4B; // Report Map
	characteristics[7].UUID16bits[1] = 0x2A; //
	characteristics[8].handle[0] = 0x17;
	characteristics[8].handle[1] = 0x00;
	characteristics[8].characteristicProperties = 0x1A; // 0x1A = 0x10 Notify | 0x08 Write | 0x02 Read
	characteristics[8].characteristicValueHandle[0] = 0x18;
	characteristics[8].characteristicValueHandle[1] = 0x00;
	characteristics[8].UUID16bits[0] = 0x33; // Boot Mouse Input Report
	characteristics[8].UUID16bits[1] = 0x2A; //
	characteristics[9].handle[0] = 0x1A;
	characteristics[9].handle[1] = 0x00;
	characteristics[9].characteristicProperties = 0x02; // 0x02 Read
	characteristics[9].characteristicValueHandle[0] = 0x1B;
	characteristics[9].characteristicValueHandle[1] = 0x00;
	characteristics[9].UUID16bits[0] = 0x4A; // HID Information
	characteristics[9].UUID16bits[1] = 0x2A; //
	characteristics[10].handle[0] = 0x1C;
	characteristics[10].handle[1] = 0x00;
	characteristics[10].characteristicProperties = 0x04; //  0x04 Write Without Response
	characteristics[10].characteristicValueHandle[0] = 0x1D;
	characteristics[10].characteristicValueHandle[1] = 0x00;
	characteristics[10].UUID16bits[0] = 0x4C; // HID Control Point
	characteristics[10].UUID16bits[1] = 0x2A; //

	services[0].handle[0] = 0x01;
	services[0].handle[1] = 0x00;
	services[0].endGroupHandle[0] = 0x07;
	services[0].endGroupHandle[1] = 0x00;
	services[0].UUID16bits[0] = 0x00; // Generic Access Profile
	services[0].UUID16bits[1] = 0x18; //
	services[1].handle[0] = 0x08;
	services[1].handle[1] = 0x00;
	services[1].endGroupHandle[0] = 0x08;
	services[1].endGroupHandle[1] = 0x00;
	services[1].UUID16bits[0] = 0x01; // Generic Attribute Profile
	services[1].UUID16bits[1] = 0x18; //
	services[2].handle[0] = 0x09;
	services[2].handle[1] = 0x00;
	services[2].endGroupHandle[0] = 0x0D;
	services[2].endGroupHandle[1] = 0x00;
	services[2].UUID16bits[0] = 0x0A; // Device Information
	services[2].UUID16bits[1] = 0x18; //
	services[3].handle[0] = 0x0E;
	services[3].handle[1] = 0x00;
	services[3].endGroupHandle[0] = 0xFF;
	services[3].endGroupHandle[1] = 0xFF;
	services[3].UUID16bits[0] = 0x12; // Human Interface Device
	services[3].UUID16bits[1] = 0x18; //

	informations[0].handle[0] = 0x13;
	informations[0].handle[1] = 0x00;
	informations[0].UUID16bits[0] = 0x02; // Client Characteristic Configuration
	informations[0].UUID16bits[1] = 0x29; //
	informations[1].handle[0] = 0x14;
	informations[1].handle[1] = 0x00;
	informations[1].UUID16bits[0] = 0x08; // Report Reference
	informations[1].UUID16bits[1] = 0x29; //

	reportMap[0] = 0x05; // Usage Page (Generic Desktop Ctrls)
	reportMap[1] = 0x01; // 
	reportMap[2] = 0x09; // Usage (Mouse)
	reportMap[3] = 0x02; // 
	reportMap[4] = 0xA1; // Collection (Application)
	reportMap[5] = 0x01; // 
	reportMap[6] = 0x05; //   Usage Page (Generic Desktop Ctrls)
	reportMap[7] = 0x01; //
	reportMap[8] = 0x09; //   Usage (Mouse)
	reportMap[9] = 0x02; //
	reportMap[10] = 0xA1; //   Collection (Logical)
	reportMap[11] = 0x02; //
	reportMap[12] = 0x85; //     Report ID (0x1A)
	reportMap[13] = 0x1A; //
	reportMap[14] = 0x09; //     Usage (Pointer)
	reportMap[15] = 0x01; //
	reportMap[16] = 0xA1; //     Collection (Physical)
	reportMap[17] = 0x00; //
	reportMap[18] = 0x05; //       Usage Page (Button)
	reportMap[19] = 0x09; //
	reportMap[20] = 0x19; //       Usage Minimum (0x01)
	reportMap[21] = 0x01; //
	reportMap[22] = 0x29; //       Usage Maximum (0x05)  	-> 5 buttons
	reportMap[23] = 0x05; //
	reportMap[24] = 0x95; //       Report Count (5)			-> //
	reportMap[25] = 0x05; //
	reportMap[26] = 0x75; //       Report Size (1)			-> 1 bit per button
	reportMap[27] = 0x01; //
	reportMap[28] = 0x15; //       Logical Minimum (0)		-> binary buttons (value=0 or 1)
	reportMap[29] = 0x00; //
	reportMap[30] = 0x25; //       Logical Maximum (1)		-> //
	reportMap[31] = 0x01; //
	reportMap[32] = 0x81; //       Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
	reportMap[33] = 0x02; //
	reportMap[34] = 0x75; //       Report Size (3)			-> 3 bits constant field
	reportMap[35] = 0x03; //
	reportMap[36] = 0x95; //       Report Count (1)
	reportMap[37] = 0x01; //
	reportMap[38] = 0x81; //       Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
	reportMap[39] = 0x01; //
	reportMap[40] = 0x05; //       Usage Page (Generic Desktop Ctrls)
	reportMap[41] = 0x01; //
	reportMap[42] = 0x09; //       Usage (X)
	reportMap[43] = 0x30; //
	reportMap[44] = 0x09; //       Usage (Y)
	reportMap[45] = 0x31; //
	reportMap[46] = 0x95; //       Report Count (2)
	reportMap[47] = 0x02; //
	reportMap[48] = 0x75; //       Report Size (16)
	reportMap[49] = 0x10; //
	reportMap[50] = 0x16; //       Logical Minimum (-32767)
	reportMap[51] = 0x01; //
	reportMap[52] = 0x80; //
	reportMap[53] = 0x26; //       Logical Maximum (32767)
	reportMap[54] = 0xFF; //
	reportMap[55] = 0x7F; //
	reportMap[56] = 0x81; //       Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
	reportMap[57] = 0x06; //
	reportMap[58] = 0xA1; //       Collection (Logical)
	reportMap[59] = 0x02; //
	reportMap[60] = 0x85; //         Report ID (0x1A)
	reportMap[61] = 0x1A; //
	reportMap[62] = 0x09; //         Usage (Wheel)
	reportMap[63] = 0x38; //
	reportMap[64] = 0x35; //         Physical Minimum (0)
	reportMap[65] = 0x00; //
	reportMap[66] = 0x45; //         Physical Maximum (0)
	reportMap[67] = 0x00; //
	reportMap[68] = 0x95; //         Report Count (1)
	reportMap[69] = 0x01; //
	reportMap[70] = 0x75; //         Report Size (16)
	reportMap[71] = 0x10; //
	reportMap[72] = 0x16; //         Logical Minimum (-32767)
	reportMap[73] = 0x01; //
	reportMap[74] = 0x80; //
	reportMap[75] = 0x26; //         Logical Maximum (32767)
	reportMap[76] = 0xFF; //
	reportMap[77] = 0x7F; //
	reportMap[78] = 0x81; //         Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
	reportMap[79] = 0x06; //
	reportMap[80] = 0xC0; //       End Collection
	reportMap[81] = 0xA1; //       Collection (Logical)
	reportMap[82] = 0x02; //
	reportMap[83] = 0x85; //         Report ID (0x1A)
	reportMap[84] = 0x1A; //
	reportMap[85] = 0x05; //         Usage Page (Consumer)
	reportMap[86] = 0x0D; //
	reportMap[87] = 0x95; //         Report Count (1)
	reportMap[88] = 0x01; //
	reportMap[89] = 0x75; //         Report Size (16)
	reportMap[90] = 0x10; //
	reportMap[91] = 0x16; //         Logical Minimum (-32767)
	reportMap[92] = 0x01; //
	reportMap[93] = 0x80; //
	reportMap[94] = 0x26; //         Logical Maximum (32767)
	reportMap[95] = 0xFF; //
	reportMap[96] = 0x7F; //
	reportMap[97] = 0x0A; //         Usage (AC Pan)		-> Horizontal wheel
	reportMap[98] = 0x38; //
	reportMap[99] = 0x02; //
	reportMap[100] = 0x81; //         Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
	reportMap[101] = 0x06; //
	reportMap[102] = 0xC0; //       End Collection
	reportMap[103] = 0xC0; //     End Collection
	reportMap[104] = 0xC0; //   End Collection
	reportMap[105] = 0xC0; // End Collection

	ZeroMemory(mouseData, 88 * sizeof(MouseData));
	mouseData[0].xlsb = 0x01;
	mouseData[0].ylsb = 0x01;
	mouseData[1].xlsb = 0x01;
	mouseData[1].ylsb = 0x01;
	mouseData[2].xlsb = 0x01;
	mouseData[2].ylsb = 0x01;
	mouseData[3].xlsb = 0x01;
	mouseData[3].ylsb = 0x01;
	mouseData[4].xlsb = 0x01;
	mouseData[4].ylsb = 0x01;
	mouseData[5].xlsb = 0x01;
	mouseData[5].ylsb = 0x01;
	mouseData[6].xlsb = 0x01;
	mouseData[6].ylsb = 0x01;
	mouseData[7].xlsb = 0x01;
	mouseData[8].xlsb = 0x01;
	mouseData[8].ylsb = 0x01;
	mouseData[9].xlsb = 0x01;
	mouseData[10].xlsb = 0x01;
	mouseData[11].xlsb = 0x01;
	mouseData[12].xlsb = 0x01;
	mouseData[13].xlsb = 0x01;
	mouseData[13].ylsb = 0xFF; // -1
	mouseData[13].ymsb = 0xFF;
	mouseData[14].xlsb = 0x01;
	mouseData[15].xlsb = 0x01;
	mouseData[15].ylsb = 0xFF;
	mouseData[15].ymsb = 0xFF;
	mouseData[16].xlsb = 0x01;
	mouseData[16].ylsb = 0xFF;
	mouseData[16].ymsb = 0xFF;
	mouseData[17].xlsb = 0x01;
	mouseData[17].ylsb = 0xFF;
	mouseData[17].ymsb = 0xFF;
	mouseData[18].ylsb = 0xFF;
	mouseData[18].ymsb = 0xFF;
	mouseData[19].xlsb = 0x01;
	mouseData[19].ylsb = 0xFF;
	mouseData[19].ymsb = 0xFF;
	mouseData[20].ylsb = 0xFF;
	mouseData[20].ymsb = 0xFF;
	mouseData[21].ylsb = 0xFF;
	mouseData[21].ymsb = 0xFF;
	mouseData[22].ylsb = 0xFF;
	mouseData[22].ymsb = 0xFF;
	mouseData[23].ylsb = 0xFF;
	mouseData[23].ymsb = 0xFF;
	mouseData[24].xlsb = 0xFF;
	mouseData[24].xmsb = 0xFF;
	mouseData[24].ylsb = 0xFF;
	mouseData[24].ymsb = 0xFF;
	mouseData[25].ylsb = 0xFF;
	mouseData[25].ymsb = 0xFF;
	mouseData[26].xlsb = 0xFF;
	mouseData[26].xmsb = 0xFF;
	mouseData[26].ylsb = 0xFF;
	mouseData[26].ymsb = 0xFF;
	mouseData[27].xlsb = 0xFF;
	mouseData[27].xmsb = 0xFF;
	mouseData[27].ylsb = 0xFF;
	mouseData[27].ymsb = 0xFF;
	mouseData[28].xlsb = 0xFF;
	mouseData[28].xmsb = 0xFF;
	mouseData[28].ylsb = 0xFF;
	mouseData[28].ymsb = 0xFF;
	mouseData[29].xlsb = 0xFF;
	mouseData[29].xmsb = 0xFF;
	mouseData[30].xlsb = 0xFF;
	mouseData[30].xmsb = 0xFF;
	mouseData[30].ylsb = 0xFF;
	mouseData[30].ymsb = 0xFF;
	mouseData[31].xlsb = 0xFF;
	mouseData[31].xmsb = 0xFF;
	mouseData[32].xlsb = 0xFF;
	mouseData[32].xmsb = 0xFF;
	mouseData[33].xlsb = 0xFF;
	mouseData[33].xmsb = 0xFF;
	mouseData[34].xlsb = 0xFF;
	mouseData[34].xmsb = 0xFF;
	mouseData[35].xlsb = 0xFF;
	mouseData[35].xmsb = 0xFF;
	mouseData[35].ylsb = 0x01;
	mouseData[36].xlsb = 0xFF;
	mouseData[36].xmsb = 0xFF;
	mouseData[37].xlsb = 0xFF;
	mouseData[37].xmsb = 0xFF;
	mouseData[37].ylsb = 0x01;
	mouseData[38].xlsb = 0xFF;
	mouseData[38].xmsb = 0xFF;
	mouseData[38].ylsb = 0x01;
	mouseData[39].xlsb = 0xFF;
	mouseData[39].xmsb = 0xFF;
	mouseData[39].ylsb = 0x01;
	mouseData[40].xlsb = 0xFF;
	mouseData[40].xmsb = 0xFF;
	mouseData[40].ylsb = 0x01;
	mouseData[41].xlsb = 0xFF;
	mouseData[41].xmsb = 0xFF;
	mouseData[41].ylsb = 0x01;
	mouseData[42].xlsb = 0xFF;
	mouseData[42].xmsb = 0xFF;
	mouseData[42].ylsb = 0x01;
	mouseData[43].xlsb = 0xFF;
	mouseData[43].xmsb = 0xFF;
	mouseData[43].ylsb = 0x01;
	mouseData[44].xlsb = 0xFF;
	mouseData[44].xmsb = 0xFF;
	mouseData[44].ylsb = 0x01;
	mouseData[45].xlsb = 0xFF;
	mouseData[45].xmsb = 0xFF;
	mouseData[45].ylsb = 0x01;
	mouseData[46].xlsb = 0xFF;
	mouseData[46].xmsb = 0xFF;
	mouseData[46].ylsb = 0x01;
	mouseData[47].xlsb = 0xFF;
	mouseData[47].xmsb = 0xFF;
	mouseData[47].ylsb = 0x01;
	mouseData[48].xlsb = 0xFF;
	mouseData[48].xmsb = 0xFF;
	mouseData[48].ylsb = 0x01;
	mouseData[49].xlsb = 0xFF;
	mouseData[49].xmsb = 0xFF;
	mouseData[49].ylsb = 0x01;
	mouseData[50].xlsb = 0xFF;
	mouseData[50].xmsb = 0xFF;
	mouseData[50].ylsb = 0x01;
	mouseData[51].xlsb = 0xFF;
	mouseData[51].xmsb = 0xFF;
	mouseData[52].xlsb = 0xFF;
	mouseData[52].xmsb = 0xFF;
	mouseData[52].ylsb = 0x01;
	mouseData[53].xlsb = 0xFF;
	mouseData[53].xmsb = 0xFF;
	mouseData[54].xlsb = 0xFF;
	mouseData[54].xmsb = 0xFF;
	mouseData[55].xlsb = 0xFF;
	mouseData[55].xmsb = 0xFF;
	mouseData[56].xlsb = 0xFF;
	mouseData[56].xmsb = 0xFF;
	mouseData[57].xlsb = 0xFF;
	mouseData[57].xmsb = 0xFF;
	mouseData[57].ylsb = 0xFF;
	mouseData[57].ymsb = 0xFF;
	mouseData[58].xlsb = 0xFF;
	mouseData[58].xmsb = 0xFF;
	mouseData[59].xlsb = 0xFF;
	mouseData[59].xmsb = 0xFF;
	mouseData[59].ylsb = 0xFF;
	mouseData[59].ymsb = 0xFF;
	mouseData[60].xlsb = 0xFF;
	mouseData[60].xmsb = 0xFF;
	mouseData[60].ylsb = 0xFF;
	mouseData[60].ymsb = 0xFF;
	mouseData[61].xlsb = 0xFF;
	mouseData[61].xmsb = 0xFF;
	mouseData[61].ylsb = 0xFF;
	mouseData[61].ymsb = 0xFF;
	mouseData[62].ylsb = 0xFF;
	mouseData[62].ymsb = 0xFF;
	mouseData[63].xlsb = 0xFF;
	mouseData[63].xmsb = 0xFF;
	mouseData[63].ylsb = 0xFF;
	mouseData[63].ymsb = 0xFF;
	mouseData[64].ylsb = 0xFF;
	mouseData[64].ymsb = 0xFF;
	mouseData[65].ylsb = 0xFF;
	mouseData[65].ymsb = 0xFF;
	mouseData[66].ylsb = 0xFF;
	mouseData[66].ymsb = 0xFF;
	mouseData[67].ylsb = 0xFF;
	mouseData[67].ymsb = 0xFF;
	mouseData[68].xlsb = 0x01;
	mouseData[68].ylsb = 0xFF;
	mouseData[68].ymsb = 0xFF;
	mouseData[69].ylsb = 0xFF;
	mouseData[69].ymsb = 0xFF;
	mouseData[70].xlsb = 0x01;
	mouseData[70].ylsb = 0xFF;
	mouseData[70].ymsb = 0xFF;
	mouseData[71].xlsb = 0x01;
	mouseData[71].ylsb = 0xFF;
	mouseData[71].ymsb = 0xFF;
	mouseData[72].xlsb = 0x01;
	mouseData[72].ylsb = 0xFF;
	mouseData[72].ymsb = 0xFF;
	mouseData[73].xlsb = 0x01;
	mouseData[74].xlsb = 0x01;
	mouseData[74].ylsb = 0xFF;
	mouseData[74].ymsb = 0xFF;
	mouseData[75].xlsb = 0x01;
	mouseData[76].xlsb = 0x01;
	mouseData[77].xlsb = 0x01;
	mouseData[78].xlsb = 0x01;
	mouseData[79].xlsb = 0x01;
	mouseData[79].ylsb = 0x01;
	mouseData[80].xlsb = 0x01;
	mouseData[81].xlsb = 0x01;
	mouseData[81].ylsb = 0x01;
	mouseData[82].xlsb = 0x01;
	mouseData[82].ylsb = 0x01;
	mouseData[83].xlsb = 0x01;
	mouseData[83].ylsb = 0x01;
	mouseData[84].xlsb = 0x01;
	mouseData[84].ylsb = 0x01;
	mouseData[85].xlsb = 0x01;
	mouseData[85].ylsb = 0x01;
	mouseData[86].xlsb = 0x01;
	mouseData[86].ylsb = 0x01;
	mouseData[87].xlsb = 0x01;
	mouseData[87].ylsb = 0x01;
}

int main()
{
	BYTE* cmd_inputBuffer;
	BYTE* cmd_outputBuffer;
	BOOL success;
	DWORD returned;
	HANDLE hciControlDeviceCmd;
	HANDLE hThreadEvent;
	HANDLE hThreadAclData;
	int i;
	int result = EXIT_FAILURE;
	BOOL startedSendingReportMap = FALSE;
	DWORD nbIterationWoAclRequest = 0;
	int mouseDataIdx;

	pAttributeRequested = (AttributeRequested*)malloc(sizeof(AttributeRequested));
	pAttributeRequested->opcode = 0x00;
	aclData[0] = (BYTE*)malloc(1024);
	aclData[1] = (BYTE*)malloc(1024);
	attMsgReceived = (BYTE*)malloc(1024);
	sigMsgReceived = (BYTE*)malloc(1024);
	secMsgReceived = (BYTE*)malloc(1024);
	attMsgReceived[0] = 0x00; // No message stored
	sigMsgReceived[0] = 0x00; // No message stored
	secMsgReceived[0] = 0x00; // No message stored

	AttributeData* characteristics = (AttributeData*)malloc(11 * sizeof(AttributeData));
	AttributeData* services = (AttributeData*)malloc(4 * sizeof(AttributeData));
	AttributeData* informations = (AttributeData*)malloc(2 * sizeof(AttributeData));
	
	int reportMapIdx = 0;
	BYTE* reportMap = (BYTE*)malloc(106);
	
	MouseData* mouseData = (MouseData*)malloc(88 * sizeof(MouseData));

	initData(characteristics, services, informations, reportMap, mouseData);


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

	printf("Sending Reset command...\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x03; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x01; // Command
	cmd_inputBuffer[i++] = 0x03; // OGF 0x03, OCF 0x0003
	cmd_inputBuffer[i++] = 0x0C; // The OGF occupies the upper 6 bits of the Opcode, while the OCF occupies the remaining 10 bits.
	cmd_inputBuffer[i++] = 0x00; // Parameter Total Length
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	ResetEvent(hEventCmdFinished);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		goto exit;
	}
	else
	{
		printf("Reset sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE,'c');
	}

	printf("Waiting for the end of the Reset command...\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Sending Read BD_ADDR Command...\n");
	i = 0;
	cmd_inputBuffer[i++] = 0x03; // Length of the IOCTL message
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x00;
	cmd_inputBuffer[i++] = 0x01; // Command
	cmd_inputBuffer[i++] = 0x09; // OGF 0x04, OCF 0x0009
	cmd_inputBuffer[i++] = 0x10; //
	cmd_inputBuffer[i++] = 0x00; // Parameter Total Length
	printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
	ResetEvent(hEventCmdFinished);
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		goto exit;
	}
	else
	{
		printf("Read BD_ADDR Command sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Sending LE Set Advertising Parameters Command...\n");
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
		goto exit;
	}
	else
	{
		printf("Set Advertising Parameters sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Waiting for the end of the Set Advertising Parameters command...\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Sending LE Set Advertising Data Command...\n");
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
		goto exit;
	}
	else
	{
		printf("Set Advertising Data sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Waiting for the end of the Set Advertising Data command...\n");
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

	printf("Sending LE Set Advertise Enable Command...\n");
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
		goto exit;
	}
	else
	{
		printf("Set Advertise Enable sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Waiting for the end of the Set Advertise Enable command...\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Long wait for the Pairing Request...\n");
	do
	{
		if (WaitForSingleObject(hEventAclDataReceived, 500) == WAIT_OBJECT_0)
		{
			parseAclData();
		}

		if (FALSE == mainLoop_continue)
		{
			goto exit;
		}

	} while (FALSE == isPairingRequestReceived());

	printf("Sending Pairing Response...\n");
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
		goto exit;
	}
	else
	{
		printf("Pairing Response sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
		storePairingResponse(cmd_inputBuffer);
	}

	// Our(slave) "random" value (TODO: we can generate a new random value each time)
	BYTE sRand[] = {
		0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA,
		0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA
	};
	
	// Some information of the Connection Complete Event are mandatory to compute the Pairing Confirm 
	printf("Waiting for the Connection Complete...\n");
	if (WaitForSingleObject(hEventConnCmpltReceived, 1000) != WAIT_OBJECT_0)
	{
		goto exit;
	}

	printf("Waiting for the Pairing Confirm...\n");
	do
	{
		if (WaitForSingleObject(hEventAclDataReceived, 500) == WAIT_OBJECT_0)
		{
			parseAclData();
		}

		if (FALSE == mainLoop_continue)
		{
			goto exit;
		}

	} while (FALSE == isPairingConfirmReceived());

	printf("Computing pairing confirm...\n");
	BYTE confirmValue[16];
	computeConfirmValue(sRand, pairingRequest, pairingResponse, iat, ia, rat, ra, confirmValue);
	printBuffer2HexString(sRand, 16, FALSE, 'r');
	printBuffer2HexString(pairingRequest, 7, FALSE, 'p');
	printBuffer2HexString(pairingResponse, 7, FALSE, 'p');
	printBuffer2HexString(ia, 6, FALSE, 'i');
	printBuffer2HexString(ra, 6, FALSE, 'r');
	printBuffer2HexString(confirmValue, 16, FALSE, 'v');
	printf("Pairing confirm computed.\n");

	printf("Sending Pairing Confirm...\n");
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
		goto exit;
	}
	else
	{
		printf("Pairing Confirm sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}


	printf("Waiting for the Pairing Random...\n");
	do
	{
		if (WaitForSingleObject(hEventAclDataReceived, 500) == WAIT_OBJECT_0)
		{
			parseAclData();
		}

		if (FALSE == mainLoop_continue)
		{
			goto exit;
		}

	} while (FALSE == isPairingRandomReceived());

	printf("Sending Pairing Random...\n");
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
		goto exit;
	}
	else
	{
		printf("Pairing Random sent.\n");
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

	printf("Waiting for the LTK Request...\n");
	if (WaitForSingleObject(hEventLTKRqstReceived, 1000) != WAIT_OBJECT_0)
	{
		goto exit;
	}

	printf("Computing STK...\n");
	BYTE stkValue[16];
	printBuffer2HexString(sRand, 16, FALSE, 's');
	printBuffer2HexString(mRand, 16, FALSE, 'm');
	computeStk(sRand, mRand, stkValue);
	printBuffer2HexString(stkValue, 16, FALSE, 'k');
	printf("STK computed.\n");

	printf("Sending LTK Request Reply command...\n");
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
		goto exit;
	}
	else
	{
		printf("LTK Request Reply sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Waiting for the end of the LTK Request Reply command...\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	printf("Sending Encryption Information...\n");
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
		goto exit;
	}
	else
	{
		printf("Encryption Information sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}


	printf("Sending Master Identification...\n");
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
		goto exit;
	}
	else
	{
		printf("Master Identification sent.\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
	}

	printf("Waiting for the Signing Information...\n");
	do
	{
		if (WaitForSingleObject(hEventAclDataReceived, 500) == WAIT_OBJECT_0)
		{
			parseAclData();
		}

		if (FALSE == mainLoop_continue)
		{
			goto exit;
		}

	} while (FALSE == isSigningInformationReceived());


	printf("Waiting for the optional Exchange MTU Request ...\n");
	i = 0;
	do
	{
		if (WaitForSingleObject(hEventAclDataReceived, 500) == WAIT_OBJECT_0)
		{
			parseAclData();
		}

		if (isExchangeMTURequestReceived())
		{
			printf("Sending Exchange MTU Response...\n");
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
			cmd_inputBuffer[i++] = mtu[0]; // MTU value
			cmd_inputBuffer[i++] = mtu[1]; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Exchange MTU Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
			break;
		}

		if (FALSE == mainLoop_continue)
		{
			goto exit;
		}

	} while (i++ < 2);

	sendConnectionParameterUpdateRequest(hciControlDeviceCmd, cmd_inputBuffer, cmd_outputBuffer);
	printf("Waiting for the Connection Parameter Update Response...\n");
	do
	{
		if (WaitForSingleObject(hEventAclDataReceived, 500) == WAIT_OBJECT_0)
		{
			parseAclData();
		}

		if (FALSE == mainLoop_continue)
		{
			goto exit;
		}

	} while (FALSE == isConnectionParameterUpdateResponseReceived());

	printf("Main loop...\n");
	while (mainLoop_continue)
	{
		if (isReadByTypeRequestReceivedAndAttributeFound(0x03, 0x28, characteristics, 11)) // GATT Characteristic Declaration
		{
			printf("Sending Read By Type Response for GATT Characteristic Declaration...\n");
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

			int startMTU = i;
			for (int charIdx = 0; charIdx < 11; charIdx++)
			{
				if (characteristics[charIdx].handle[0] >= attMsgReceived[14]	// startingHandle0
					&& characteristics[charIdx].handle[1] >= attMsgReceived[15]	// startingHandle1
					&& characteristics[charIdx].handle[0] <= attMsgReceived[16]	// endingHandle0
					&& characteristics[charIdx].handle[1] <= attMsgReceived[17]	// endingHandle1
					&& i - startMTU < 23 - 7) // Do we still have enough place for a new attribute value ?
				{
					cmd_inputBuffer[i++] = characteristics[charIdx].handle[0];
					cmd_inputBuffer[i++] = characteristics[charIdx].handle[1];
					cmd_inputBuffer[i++] = characteristics[charIdx].characteristicProperties;
					cmd_inputBuffer[i++] = characteristics[charIdx].characteristicValueHandle[0];
					cmd_inputBuffer[i++] = characteristics[charIdx].characteristicValueHandle[1];
					cmd_inputBuffer[i++] = characteristics[charIdx].UUID16bits[0];
					cmd_inputBuffer[i++] = characteristics[charIdx].UUID16bits[1];
				}
			}

			cmd_inputBuffer[9] = i-13; // Length of the L2CAP message
			cmd_inputBuffer[7] = i-9; // Length of the ACL message
			cmd_inputBuffer[0] = i-5; // Length of the IOCTL message

			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Read By Type Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x08	// Read By Type Request
			&& pAttributeRequested->uuid[0] == 0x50	// PnP ID
			&& pAttributeRequested->uuid[1] == 0x2A)  //
		{
			printf("Sending Read By Type Response for PnP ID...\n");
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
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x09; // Read By Type Response
			cmd_inputBuffer[i++] = 0x09; // Size of Attribute Data
			cmd_inputBuffer[i++] = 0x0D; // Handle
			cmd_inputBuffer[i++] = 0x00; // 
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
				goto exit;
			}
			else
			{
				printf("Read By Type Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x08	// Read By Type Request
			&& pAttributeRequested->uuid[0] == 0x04	// Peripheral Preferred Connection Parameters
			&& pAttributeRequested->uuid[1] == 0x2A)  //
		{
			printf("Sending Read By Type Response for Peripheral Preferred Connection Parameters...\n");
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
			cmd_inputBuffer[i++] = 0x09; // Read By Type Response
			cmd_inputBuffer[i++] = 0x0A; // Size of Attribute Data
			cmd_inputBuffer[i++] = 0x07; // Handle
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x06; // Interval_Min 0x0006
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x06; // Interval_Max 0x0006
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x00; // Latency 0x003C
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0xE8; // Timeout 0x012C
			cmd_inputBuffer[i++] = 0x03; //
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Read By Type Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x10	// Read By Group Type Request
			&& pAttributeRequested->uuid[0] == 0x00	// GATT Primary Service Declaration
			&& pAttributeRequested->uuid[1] == 0x28   //
			&& isAttributeInRange(services, 4))
		{
			printf("Sending Read By Group Type Response for GATT Primary Service Declaration...\n");
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

			int startMTU = i;
			for (int srvIdx = 0; srvIdx < 4; srvIdx++)
			{
				if (services[srvIdx].handle[0] >= pAttributeRequested->startingHandle[0]
					&& services[srvIdx].handle[1] >= pAttributeRequested->startingHandle[1]
					&& services[srvIdx].handle[0] <= pAttributeRequested->endingHandle[0]
					&& services[srvIdx].handle[1] <= pAttributeRequested->endingHandle[1]
					&& i - startMTU < 23 - 6) // Do we still have enough place for a new attribute value ?
				{
					cmd_inputBuffer[i++] = services[srvIdx].handle[0];
					cmd_inputBuffer[i++] = services[srvIdx].handle[1];
					cmd_inputBuffer[i++] = services[srvIdx].endGroupHandle[0];
					cmd_inputBuffer[i++] = services[srvIdx].endGroupHandle[1];
					cmd_inputBuffer[i++] = services[srvIdx].UUID16bits[0];
					cmd_inputBuffer[i++] = services[srvIdx].UUID16bits[1];
				}
			}

			cmd_inputBuffer[9] = i - 13; // Length of the L2CAP message
			cmd_inputBuffer[7] = i - 9; // Length of the ACL message
			cmd_inputBuffer[0] = i - 5; // Length of the IOCTL message

			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Read By Group Type Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x06	// Find By Type Value Request
			&& pAttributeRequested->uuid[0] == 0x00	// GATT Primary Service Declaration
			&& pAttributeRequested->uuid[1] == 0x28	//
			&& pAttributeRequested->value[0] == 0x01	// Generic Attribute Profile
			&& pAttributeRequested->value[1] == 0x18	//
			&& pAttributeRequested->startingHandle[0] <= 0x08
			&& pAttributeRequested->startingHandle[1] == 0x00
			&& pAttributeRequested->endingHandle[0] >= 0x08
			&& pAttributeRequested->endingHandle[1] >= 0x00)
		{
			printf("Sending Find By Type Value Response for type GATT Primary Service Declaration and value Generic Attribute Profile...\n");
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
				goto exit;
			}
			else
			{
				printf("Find By Type Value Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x04 	// Find Information Request
			&& isAttributeInRange(informations, 2))
		{
			printf("Sending Find Information Response for Report...\n");
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

			for (int infoIdx = 0; infoIdx < 2; infoIdx++)
			{
				if (informations[infoIdx].handle[0] >= pAttributeRequested->startingHandle[0]
					&& informations[infoIdx].handle[1] >= pAttributeRequested->startingHandle[1]
					&& informations[infoIdx].handle[0] <= pAttributeRequested->endingHandle[0]
					&& informations[infoIdx].handle[1] <= pAttributeRequested->endingHandle[1])
				{
					cmd_inputBuffer[i++] = informations[infoIdx].handle[0];
					cmd_inputBuffer[i++] = informations[infoIdx].handle[1];
					cmd_inputBuffer[i++] = informations[infoIdx].UUID16bits[0];
					cmd_inputBuffer[i++] = informations[infoIdx].UUID16bits[1];
				}
			}

			cmd_inputBuffer[9] = i - 13; // Length of the L2CAP message
			cmd_inputBuffer[7] = i - 9; // Length of the ACL message
			cmd_inputBuffer[0] = i - 5; // Length of the IOCTL message

			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Find Information Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if(pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x03
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Generic Access Profile: Device Name...\n");
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
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x0D
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Device Information: PnP ID...\n");
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
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x10
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Human Interface Device: Protocol Mode...\n");
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
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x12
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Human Interface Device: Report...\n");
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
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x13
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Human Interface Device: Report: Client Characteristic Configuration...\n");
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
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x14
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Human Interface Device: Report: Report Reference...\n");
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
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x16
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Human Interface Device: Report Map...\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x1F; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x1B; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x17; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0B; // Read Response
			for (reportMapIdx = 0; reportMapIdx < 22; reportMapIdx++) // Why 22 instead of the MTU 23 ?
			{
				cmd_inputBuffer[i++] = reportMap[reportMapIdx];
			}
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
				startedSendingReportMap = TRUE;
			}
		}
		else if (pAttributeRequested->opcode == 0x0C // Read Blob Request
			&& pAttributeRequested->startingHandle[0] == 0x16
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Blob Response for Human Interface Device: Report Map...\n");
			i = 0;
			cmd_inputBuffer[i++] = 0x1F; // Length of the IOCTL message
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x00;
			cmd_inputBuffer[i++] = 0x02; // ACL data
			cmd_inputBuffer[i++] = connectionHandle[0];
			cmd_inputBuffer[i++] = connectionHandle[1];
			cmd_inputBuffer[i++] = 0x1B; // Length of the ACL message
			cmd_inputBuffer[i++] = 0x00; // 
			cmd_inputBuffer[i++] = 0x17; // Length of the L2CAP message
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x04; // Attribute Protocol
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x0D; // Read Blob Response
			
			for (reportMapIdx = pAttributeRequested->valueOffset[0]; 
				reportMapIdx < 22 + pAttributeRequested->valueOffset[0] && reportMapIdx < 106; // Why 22 instead of the MTU 23 ?
				reportMapIdx++) 
			{
				cmd_inputBuffer[i++] = reportMap[reportMapIdx];
			}

			cmd_inputBuffer[9] = i - 13; // Length of the L2CAP message
			cmd_inputBuffer[7] = i - 9; // Length of the ACL message
			cmd_inputBuffer[0] = i - 5; // Length of the IOCTL message

			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Read Blob Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x0A // Read Request
			&& pAttributeRequested->startingHandle[0] == 0x1B
			&& pAttributeRequested->startingHandle[1] == 0x00)
		{
			printf("Sending Read Response for Human Interface Device: HID Information...\n");
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
				goto exit;
			}
			else
			{
				printf("Read Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode == 0x12) // Write Request
		{
			// We always acknowledge Write Requests
			printf("Sending Write Response...\n");
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
				goto exit;
			}
			else
			{
				printf("Write Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}
		else if (pAttributeRequested->opcode != 0x00)
		{
			// Unknown attribute
			printf("Sending Error Response...\n");
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
			cmd_inputBuffer[i++] = pAttributeRequested->opcode; // Request Opcode in Error
			cmd_inputBuffer[i++] = pAttributeRequested->startingHandle[0]; // Attribute Handle In Error
			cmd_inputBuffer[i++] = pAttributeRequested->startingHandle[1]; //
			cmd_inputBuffer[i++] = 0x0A; // Error Code = Attribute Not Found
			printBuffer2HexString(cmd_inputBuffer, i, FALSE, 'c');
			success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
			if (!success)
			{
				printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
				goto exit;
			}
			else
			{
				printf("Error Response sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			}
		}

		if (startedSendingReportMap && nbIterationWoAclRequest == 1)
		{
			break;
			// Next step: send notifications
		}
		
		printf("Waiting for the new ACL data...\n");
		if (WaitForSingleObject(hEventAclDataReceived, 2000) == WAIT_OBJECT_0)
		{
			parseAclData();
			nbIterationWoAclRequest = 0;
		}
		else
		{
			ResetEvent(hEventAclDataReceived);
			pAttributeRequested->opcode = 0x00;
			nbIterationWoAclRequest++;
		}
	}

	printf("Start sending Handle Value Notification...\n");
	mouseDataIdx = 0;
	while (mainLoop_continue)
	{
		// We don't expect to receive any ACL data, but this serves us to temporize our notifications
		//printf("Waiting for the new ACL data...\n");
		if (WaitForSingleObject(hEventAclDataReceived, 100) == WAIT_OBJECT_0)
		{
			parseAclData();
		}
		else
		{
			//printf("Sending Handle Value Notification...\n");
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
			cmd_inputBuffer[i++] = 0x1B; // Handle Value Notification
			cmd_inputBuffer[i++] = 0x12; // Attribute Handle
			cmd_inputBuffer[i++] = 0x00; //
			cmd_inputBuffer[i++] = 0x00; // 5 Buttons 0b00011111
			cmd_inputBuffer[i++] = mouseData[mouseDataIdx].xlsb; // lsb X
			cmd_inputBuffer[i++] = mouseData[mouseDataIdx].xmsb; // msb X
			cmd_inputBuffer[i++] = mouseData[mouseDataIdx].ylsb; // lsb Y
			cmd_inputBuffer[i++] = mouseData[mouseDataIdx].ymsb; // msb Y
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
				//printf("Handle Value Notification sent.\n");
				printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
				mouseDataIdx++;
				if (mouseDataIdx > 87)
				{
					mouseDataIdx = 0;
				}
			}
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
	result = EXIT_SUCCESS;
exit:
	readLoop_continue = FALSE;
	printf("Stop then start the Bluetooth of the phone to exit this program.\n");

	// Wait for the end of the "read events" and "read ACL data" threads.
	WaitForSingleObject(hThreadEvent, INFINITE);
	WaitForSingleObject(hThreadAclData, INFINITE);
	CloseHandle(hThreadEvent);
	CloseHandle(hThreadAclData);
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
	free(pAttributeRequested);
	free(aclData[0]);
	free(aclData[1]);
	free(attMsgReceived);
	free(sigMsgReceived);
	free(secMsgReceived);
	free(characteristics);
	free(services);
	free(informations);
	free(reportMap);
	free(mouseData);

    return result;
}

