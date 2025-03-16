// wp81mouseJiggler.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

static HANDLE hEventCmdFinished = NULL;
static HANDLE hLogFile;
static BOOL readLoop_continue;
static BYTE connectionHandle[2];
static HANDLE hEventPairingRqstReceived = NULL;
static HANDLE hEventConnCmpltReceived = NULL;
static HANDLE hEventPairingConfirmReceived = NULL;
static HANDLE hEventPairingRndReceived = NULL;
static HANDLE hEventLTKRqstReceived = NULL;
static HANDLE hEventXchgMTURqstReceived = NULL;
static BYTE pairingRequest[7]; // MSB first
static BYTE pairingResponse[7]; // MSB first
static BYTE iat; // Initiating device address type
static BYTE ia[6]; // Initiating device address (MSB first)
static BYTE rat = 0x00; // Responding device address type = public
static BYTE ra[6]; // Responding device address 
static BYTE mRand[16]; // Pairing random send by the initiator/master device (MSB first)
static BYTE mtu[2] = {23,0}; // MTU common to initiating and responding device (default minimum = 23 bytes)

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

DWORD WINAPI readAclData(void* notUsed)
{
	HANDLE hciControlDeviceAcl;
	DWORD returned;
	BYTE* readAcl_inputBuffer;
	BYTE* readAcl_outputBuffer;
	BOOL success;
	BYTE resultSuccess[4] = { 0x00, 0x00, 0x00, 0x00 };
	BYTE cidLocalHidControl[2] = { 0x40, 0x00 };
	BYTE cidLocalHidInterrupt[2] = { 0x41, 0x00 };

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

	readAcl_outputBuffer = (BYTE*)malloc(1030);

	printf("Start listening to ACL Data...\n");
	while (readLoop_continue)
	{
		success = DeviceIoControl(hciControlDeviceAcl, IOCTL_CONTROL_READ_HCI, readAcl_inputBuffer, 4, readAcl_outputBuffer, 1030, &returned, NULL);
		if (success)
		{
			printBuffer2HexString(readAcl_outputBuffer, returned, TRUE, 'a');
			if (returned == 20
				&& readAcl_outputBuffer[4] == 0x02 // ACL message
				&& readAcl_outputBuffer[11] == 0x06 // Security Manager Protocol
				&& readAcl_outputBuffer[13] == 0x01 // Pairing request
				)
			{
				printf("Received pairing request\n");
				storeConnectionHandle(readAcl_outputBuffer);
				storePairingRequest(readAcl_outputBuffer);
				SetEvent(hEventPairingRqstReceived);
			}
			else if (returned == 30
				&& readAcl_outputBuffer[4] == 0x02 // ACL message
				&& readAcl_outputBuffer[11] == 0x06 // Security Manager Protocol
				&& readAcl_outputBuffer[13] == 0x03 // Pairing confirm
				)
			{
				printf("Received pairing confirm\n");
				SetEvent(hEventPairingConfirmReceived);
			}
			else if (returned == 30
				&& readAcl_outputBuffer[4] == 0x02 // ACL message
				&& readAcl_outputBuffer[11] == 0x06 // Security Manager Protocol
				&& readAcl_outputBuffer[13] == 0x04 // Pairing random
				)
			{
				printf("Received pairing random\n");
				storePairingRandomReceived(readAcl_outputBuffer);
				SetEvent(hEventPairingRndReceived);
			}
			else if (returned == 16
				&& readAcl_outputBuffer[4] == 0x02 // ACL message
				&& readAcl_outputBuffer[11] == 0x04 // Attribute Protocol
				&& readAcl_outputBuffer[13] == 0x02 // Exchange MTU Request
				)
			{
				printf("Received exchange MTU request\n");
				storeMTUReceived(readAcl_outputBuffer);
				SetEvent(hEventXchgMTURqstReceived);
			}
		}
		else
		{
			printf("Failed to send IOCTL_CONTROL_READ_HCI! 0x%X\n", GetLastError());
		}
	}

	free(readAcl_inputBuffer);
	free(readAcl_outputBuffer);
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
	int i;
	int n;

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

	// Start "read events" thread
	hThreadEvent = CreateThread(NULL, 0, readEvents, NULL, 0, NULL);

	// Start "read ACL data" thread
	hThreadAclData = CreateThread(NULL, 0, readAclData, NULL, 0, NULL);

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
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Reset sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE,'c');
		ResetEvent(hEventCmdFinished);
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
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Set Advertising Parameters sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
		ResetEvent(hEventCmdFinished);
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
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Set Advertising Data sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
		ResetEvent(hEventCmdFinished);
	}

	printf("Wait for the end of the Set Advertising Data command\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	// Prepare to wait for the pairing request
	hEventPairingRqstReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_PAIRING_REQUEST"
	);

	// Prepare to wait for the connection complete event
	hEventConnCmpltReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_CONNECTION_COMPLETE"
	);

	// Prepare to wait for the exchange MTU request
	hEventXchgMTURqstReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_EXCHANGE_MTU_REQUEST"
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
	success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
	if (!success)
	{
		printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
	}
	else
	{
		printf("Set Advertise Enable sent\n");
		printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
		ResetEvent(hEventCmdFinished);
	}

	printf("Wait for the end of the Set Advertise Enable command\n");
	WaitForSingleObject(hEventCmdFinished, 1000);

	// Prepare to wait for the pairing confirm
	hEventPairingConfirmReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_PAIRING_CONFIRM"
	);

	printf("Wait for the Pairing Request\n");
	if (WaitForSingleObject(hEventPairingRqstReceived, 20000) == WAIT_OBJECT_0)
	{
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
	}

	// Prepare to wait for the pairing random
	hEventPairingRndReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_PAIRING_RANDOM"
	);

	// Our(slave) "random" value (TODO: we can generate a new random value each time)
	BYTE sRand[] = {
		0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA,
		0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA, 0xCA
	};

	printf("Wait for the Connection Complete\n");
	WaitForSingleObject(hEventConnCmpltReceived, 2000);

	printf("Wait for the Pairing Confirm\n");
	if (WaitForSingleObject(hEventPairingConfirmReceived, 2000) == WAIT_OBJECT_0)
	{
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
	}

	// Prepare to wait for the LTK request
	hEventLTKRqstReceived = CreateEventW(
		NULL,
		TRUE,	// manually reset
		FALSE,	// initial state: nonsignaled
		L"WP81_WAIT_FOR_LTK_REQUEST"
	);

	printf("Wait for the Pairing Random\n");
	if (WaitForSingleObject(hEventPairingRndReceived, 2000) == WAIT_OBJECT_0)
	{
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
	if (WaitForSingleObject(hEventLTKRqstReceived, 2000) == WAIT_OBJECT_0)
	{
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
		success = DeviceIoControl(hciControlDeviceCmd, IOCTL_CONTROL_WRITE_HCI, cmd_inputBuffer, i, cmd_outputBuffer, 4, &returned, NULL);
		if (!success)
		{
			printf("Failed to send DeviceIoControl! 0x%08X\n", GetLastError());
		}
		else
		{
			printf("Long Term Key Request Reply sent\n");
			printBuffer2HexString(cmd_outputBuffer, returned, TRUE, 'c');
			ResetEvent(hEventCmdFinished);
		}
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

	Sleep(100);

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

	printf("Wait for the Exchange MTU Request\n");
	if (WaitForSingleObject(hEventXchgMTURqstReceived, 1000) == WAIT_OBJECT_0)
	{
		printf("Send Exchange MTU Response\n");
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

	printf("Wait before exit\n");
	Sleep(10000);

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
	CloseHandle(hThreadEvent);
	CloseHandle(hThreadAclData);

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

    return 0;
}

