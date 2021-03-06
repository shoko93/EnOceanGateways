#include <stdio.h>
#include <stdlib.h>
#include <stddef.h> //offsefof
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <termio.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h> //PATH_MAX

#include "dpride.h"
#include "ptable.h"
#include "queue.h"
#include "serial.h"
#include "esp3.h"
#include "logger.h"
#include "json.h"
#include "utils.h"
#include "../eolib/models.h"
#include "../eolib/secure.h"

static const char copyright[] = "\n(c) 2017 Device Drivers, Ltd. \n";
static const char version[] = "\n@ dpride Version 1.28 \n";

//#define ERP2_DEBUG (1)
//#define SECURE_DEBUG (1)
#define QUEUE_DEBUG (0)
//#define FILTER_DEBUG (1)
//#define CMD_DEBUG (1)
//#define CT_DEBUG (1)
//#define SIG_DEBUG (1)
//#define MSG_DEBUG (1)
//#define MODEL_DEBUG (1)
////#define RAW_INPUT (1) // RAW_INPUT Display for ERP2 DEBUG

//
#define msleep(a) usleep((a) * 1000)

#define MAINBUFSIZ (1024)
#define DATABUFSIZ (528)
#define HEADER_SIZE (5)
#define CRC8D_SIZE (1)

#define RESPONSE_TIMEOUT (90)

//
//
//

#if defined(STAILQ_HEAD)
#undef STAILQ_HEAD

#define STAILQ_HEAD(name, type)                                 \
        struct name {                                                           \
        struct type *stqh_first;        /* first element */                     \
        struct type **stqh_last;        /* addr of last next element */         \
        int num_control;       \
        pthread_mutex_t lock; \
        }
#endif

#define INCREMENT(a) ((a) = (((a)+1) & 0x7FFFFFFF))

#define QueueSetLength(Buf, Len) \
	((QUEUE_ENTRY *)((Buf) - offsetof(QUEUE_ENTRY, Data)))->Length = (Len)

#define QueueGetLength(Buf) \
	(((QUEUE_ENTRY *)((Buf) - offsetof(QUEUE_ENTRY, Data)))->Length)

STAILQ_HEAD(QueueHead, QEntry);
typedef struct QueueHead QUEUE_HEAD;

struct QEntry {
	STAILQ_ENTRY(QEntry) Entries;
	INT Number;
	INT Length;
	BYTE Data[DATABUFSIZ];
	//....;
};
typedef struct QEntry QUEUE_ENTRY;

QUEUE_HEAD DataQueue;     // Received Data
QUEUE_HEAD ResponseQueue; // CO Command Responce
QUEUE_HEAD ExtraQueue;    // Event, Smack, etc currently not used
QUEUE_HEAD FreeQueue;     // Free buffer
QUEUE_HEAD JsonQueue;     // Json service

static const INT FreeQueueCount = 8;
static const INT QueueTryTimes = 10;
//static const INT QueueTryTimes = 100;
static const INT QueueTryWait = 2; //msec

static UINT QFCount = 0; // Enq FreeQueue
static UINT DFCount = 0; // Deq FreeQueue
static UINT QDCount = 0; // Enq DataQueue
static UINT DDCount = 0; // Deq DataQueue
static UINT QRCount = 0; // Enq RespQueue
static UINT DRCount = 0; // Deq RespQueue
static UINT QECount = 0; // Enq ExtrQueue
static UINT DECount = 0; // Deq ExtrQueue
static UINT RDCount = 0; // QueueData
static UINT MJCount = 0; // MainJob count
static UINT PPCount = 0; // PushPacket count

static const UINT _QDebug = QUEUE_DEBUG;
#define _QD if (_QDebug)  
#define _QD2 if (_QDebug > 1) 

//
//
//
BOOL stop_read;
BOOL stop_action;
BOOL stop_job;
BOOL read_ready;

typedef struct _THREAD_BRIDGE {
        pthread_t ThRead;
        pthread_t ThAction;
}
THREAD_BRIDGE;

////
// Command -- interface to Web API, other module, or command line
//

typedef enum {
        CMD_NOOP = 0,
        CMD_SHUTDOWN=1,
        CMD_REBOOT=2,
        CMD_FILTER_ADD=3,
        CMD_FILTER_CLEAR=4,
        CMD_CHANGE_MODE=5, //Monitor, Register, Operation
        CMD_CHANGE_OPTION=6, //Silent, Verbose, Debug,
}
JOB_COMMAND;

typedef struct _CMD_PARAM
{
        int Num;
        char Data[DATABUFSIZ];
}
CMD_PARAM;

void SetFd(int fd);
int GetFd(void);
static int _Fd;
void SetFd(int fd) { _Fd = fd; }
int GetFd(void) { return _Fd; }

void SetThdata(THREAD_BRIDGE Tb);
THREAD_BRIDGE *GetThdata(void);
static THREAD_BRIDGE _Tb;
void SetThdata(THREAD_BRIDGE Tb) { _Tb = Tb; }
THREAD_BRIDGE *GetThdata(void) { return &_Tb; }

//
static EO_CONTROL EoControl;
static long EoFilterList[EO_FILTER_SIZE];
static char EoLogMonitorMessage[DATABUFSIZ];

typedef struct _CDM_BUFFER
{
#define CDM_BUFFER_SIZE (528)
	INT LastIndex;
	BYTE Id[4];
	BYTE Status;
	INT Length;
	INT CurrentLength; 
	BYTE *Buffer;
	BYTE *DecBuffer;
}
CDM_BUFFER;

#define CDM_TABLE_SIZE (8)
static CDM_BUFFER CdmBuffer[CDM_TABLE_SIZE];

JOB_COMMAND GetCommand(CMD_PARAM *Param);

//
static const char *CmdName[] = {
	/*0*/ "CMD_NOOP",
	/*1*/ "CMD_SHUTDOWN",
	/*2*/ "CMD_REBOOT",
	/*3*/ "CMD_FILTER_ADD",
	/*4*/ "CMD_FILTER_CLEAR",
	/*5*/ "CMD_CHANGE_MODE", //Monitor, Register, Operation
	/*6*/ "CMD_CHANGE_OPTION", //Silent, Verbose, Debug,
	/*7*/ NULL
};

static inline char *CommandName(int index)
{
	return (char *) CmdName[index & 0x7];
}

JOB_COMMAND GetCommand(CMD_PARAM *Param)
{
	EO_CONTROL *p = &EoControl;
	int mode = 0;
	char param[DATABUFSIZ];
	const char *OptionName[] = {
		"Auto",
		"Break",
		"Clear",
		"Debug",
		"Execute",
		"File",
		"Go",
		"Help",
		"Info",
		"Join",
		"Kill",
		"List",
		"Monitor",
		"Nutral",
		"Operation",
		"Print",
		"Quit",
		"Register",
		"Silent",
		"Test",
		"Unseen",
		"Verbose",
		"Wrong",
		"X\'mas",
		"Yeld",
		"Zap",
		NULL, NULL, NULL, NULL, NULL
	};

	if (p->CommandPath == NULL) {
		p->CommandPath = MakePath(p->BridgeDirectory, p->CommandFile); 
	}
	if (p->Debug > 0) {
		printf("D:**%s: path=%s\n", __FUNCTION__, p->CommandPath);
	}
	if (!ReadCmd(p->CommandPath, &mode, param)) {
		if (p->Debug > 0) {
			printf("D:**%s: ReadCmd error mode=%d, then NOOP return\n", __FUNCTION__, mode);
		}
		return CMD_NOOP;
	}
	if (Param != NULL) {
		Param->Num = mode;
		if (Param->Data) {
			strcpy(Param->Data, param);
		}
	}
	if (p->Debug > 0) {
		printf("D:**%s: cmd:%d:%s opt:%s\n", __FUNCTION__,
			mode, CommandName(mode), isdigit(*param) ? param : OptionName[(*param - 'A') & 0x1F]);
	}
	return (JOB_COMMAND) mode;
}

void PushPacket(BYTE *Buffer);
void LogMessageRegister(char *buf);
void StartUp(void);
void SendCommand(BYTE *cmdBuffer);
bool MainJob(BYTE *Buffer);

//
//
//
INT Enqueue(QUEUE_HEAD *Queue, BYTE *Buffer)
{
	QUEUE_ENTRY *qEntry;
	const size_t offset = offsetof(QUEUE_ENTRY, Data);

	_QD printf("**Enqueue:%p\n", Buffer);

	qEntry = (QUEUE_ENTRY *)(Buffer - offset);

	pthread_mutex_lock(&Queue->lock);
	qEntry->Number = INCREMENT(Queue->num_control);

	STAILQ_INSERT_TAIL(Queue, qEntry, Entries);
	pthread_mutex_unlock(&Queue->lock);

	_QD2 printf("**Dequeue list(%d):%p\n", qEntry->Number, Buffer);

	return Queue->num_control;
}

BYTE *Dequeue(QUEUE_HEAD *Queue)
{
	QUEUE_ENTRY *entry;
	BYTE *buffer;

	//_QD printf("**Dequeue:\n");

	if (STAILQ_EMPTY(Queue)) {
		//printf("**Dequeue Empty=%s!\n", 
		//	Queue == &DataQueue ? "Data" : "Free");
		return NULL;
	}
	pthread_mutex_lock(&Queue->lock);
	entry = STAILQ_FIRST(Queue);
	buffer = entry->Data;
	STAILQ_REMOVE(Queue, entry, QEntry, Entries);
	pthread_mutex_unlock(&Queue->lock);

	_QD2 printf("**Dequeue list(%d):%p\n", entry->Number, buffer);

	return buffer;
}

#if 0 
#define	STAILQ_INSERT_TAIL(head, elm, field) do {	\
	(elm)->field.stqe_next = NULL;					\
	*(head)->stqh_last = (elm);					    \
	(head)->stqh_last = &(elm)->field.stqe_next;	\
} while (/*CONSTCOND*/0)

#define	STAILQ_REMOVE(head, elm, type, field) do {	\
	if ((head)->stqh_first == (elm)) {				\
		STAILQ_REMOVE_HEAD((head), field);	

head=Queue;
field=Entries;

#define	STAILQ_FOREACH(var, head, field)			\
	for ((var) = ((head)->stqh_first);				\
		(var);										\
		(var) = ((var)->field.stqe_next))
#endif

INT QueueCount(QUEUE_HEAD *Queue)
{
	QUEUE_ENTRY *entry;
	INT count = 0;
	STAILQ_FOREACH(entry, Queue, Entries) {
		count++;
	}
	return count;
}

VOID StartJobs(CMD_PARAM *Param)
{
	printf("**StartJobs\n");
}

VOID StopJobs(VOID)
{
	printf("**StopJobs\n");
}

VOID Shutdown(CMD_PARAM *Param)
{
	printf("**Shutdown\n");
}

//
VOID QueueData(QUEUE_HEAD *Queue, BYTE *DataBuffer, int Length)
{
#ifdef SECURE_DEBUG
	printf("+Q"); PacketDump(DataBuffer);
#endif
	_QD printf("**QueueData:%p %d %s\n", DataBuffer, QDCount, 
		Queue == &DataQueue ? "Data" :
		Queue == &ResponseQueue ? "Response" :
		Queue == &ExtraQueue ? "Extra" : "Other");

	QueueSetLength(DataBuffer, Length);
	Enqueue(Queue, DataBuffer);

	_QD QDCount++; //Enq Data Queue
}

VOID FreeQueueInit(VOID)
{
	INT i;
	struct QEntry *freeEntry;

	for(i = 0; i < FreeQueueCount; i++) {
		freeEntry = (struct QEntry *) calloc(sizeof(struct QEntry), 1);
		if (freeEntry == NULL) {
			fprintf(stderr, "FreeQueueInit: calloc error=%d\n", i);
			return;
		}
		Enqueue(&FreeQueue, freeEntry->Data);

		_QD QFCount++; // Enq FreeQueue
	}
}

VOID QueueStatus(VOID)
{
	printf("\nQF=%d DF=%d QD=%d DD=%d QR=%d DR=%d QE=%d DE=%d RD=%d MJ=%d\n",
		QFCount,
		DFCount,
		QDCount,
		DDCount,
		QRCount,
		DRCount,
		QECount,
		DECount,
		RDCount,
		MJCount);
}

VOID *ReadThread(VOID *arg)
{
	INT fd = GetFd();
	ESP_STATUS rType;
	BYTE   *dataBuffer;
	USHORT  dataLength = 0;
	BYTE   optionLength = 0;
	BYTE   packetType = 0;
	INT    totalLength;
	INT count = 0;

	_QD2 printf("**Start ReadThread()\n");

	while(!stop_read) {
		_QD printf("**ReadThread: %d FreeQ=%d\n", RDCount++, QueueCount(&FreeQueue));

		do {
			dataBuffer = Dequeue(&FreeQueue);
			if (dataBuffer == NULL) {
				if (QueueTryTimes >= count) {
					fprintf(stderr, "ReadThread: FreeQueue empty\n");
					QueueStatus();
					return (void*) NULL;
				}
				count++;
				msleep(QueueTryWait);
			}
		}
		while(dataBuffer == NULL);
		_QD DFCount++; // Deq FreeQueue

		read_ready = TRUE;
		rType = GetPacket(fd, dataBuffer, (USHORT) DATABUFSIZ);
		if (stop_job) {
			_QD2 printf("**ReadThread breaked by stop_job-1\n");
			break;
		}
		else if (rType == OK) {
			dataLength = (dataBuffer[0] << 8) + dataBuffer[1];
			optionLength = dataBuffer[2];
			packetType = dataBuffer[3];
			totalLength = HEADER_SIZE + dataLength + optionLength + CRC8D_SIZE;
#if RAW_INPUT
			if (1) {
				BYTE dataType = dataBuffer[5];
				printf("*_:");
				PacketDump(dataBuffer);
				printf("D:dLen=%d oLen=%d tot=%d typ=%02X daT=%02X\n",
					dataLength, optionLength, totalLength, packetType, dataType);
			}
#endif
		}
		else {
			fprintf(stderr, "ReadThread: invalid rType==%02X\n\n", rType);
		}

		if (stop_job) {
			_QD printf("**ReadThread breaked by stop_job-2\n");
			break;
		}
		_QD printf("**ReadTh: process=%d\n", packetType);

		switch (packetType) {
		case RADIO_ERP1: //1  Radio telegram
		case RADIO_ERP2: //0x0A ERP2 protocol radio telegram
			_QD QDCount++;
			QueueData(&DataQueue, dataBuffer, totalLength);
			break;
		case RESPONSE: //2 Response to any packet
			_QD QRCount++;
			QueueData(&ResponseQueue, dataBuffer, totalLength);
			break;
		case RADIO_SUB_TEL: //3 Radio subtelegram
		case EVENT: //4 Event message
		case COMMON_COMMAND: //5 Common command
		case SMART_ACK_COMMAND: //6 Smart Ack command
		case REMOTE_MAN_COMMAND: //7 Remote management command
		case RADIO_MESSAGE: //9 Radio message
		case CONFIG_COMMAND: //0x0B ESP3 configuration
		default:
			_QD QECount++;
			QueueData(&ExtraQueue, dataBuffer, totalLength);
			fprintf(stderr, "ReadThread: Unknown packet=%d\n", packetType);
			break;
		}
	}

	//printf("ReadThread end=%d stop_read=%d\n", stop_job, stop_read);
	return (void*) NULL;
}

//
VOID *ActionThread(void *arg)
{
	BYTE *data;

	_QD printf("**ActionThread:\n");

	while(!stop_action && !stop_job) {
		data = Dequeue(&DataQueue);
		if (data == NULL) {
			msleep(1);
		}
		else {
			_QD DDCount++; // Deq DataQUeue

			_QD MJCount++; // MainJob Count
			MainJob(data);

			Enqueue(&FreeQueue, data);
			_QD QFCount++; // Enq FreeQueue
		}
		// Check for ExtraQueue
		data = Dequeue(&ExtraQueue);
		if (data != NULL) {
			// Currently nothing to do
			_QD DECount++;
			Enqueue(&FreeQueue, data);
			_QD QFCount++; // Enq FreeQueue
		}
	}
    return OK;
}

BOOL InitSerial(OUT int *pFd)
{
	static char *ESP_PORTS[] = {
		EO_ESP_PORT_USB,
		EO_ESP_PORT_S0,
		EO_ESP_PORT_AMA0,
		NULL
	};
	char *pp;
	int i;
	EO_CONTROL *p = &EoControl;
	int fd;
	struct termios tio;

	if (p->ESPPort[0] == '\0') {
		// default, check for available port
		pp = ESP_PORTS[0];
		for(i = 0; pp != NULL && *pp != '\0'; i++) {
			if ((fd = open(pp, O_RDWR)) >= 0) {
				close(fd);
				p->ESPPort = pp;
				//printf("##%s: found=%s\n", __func__, pp);
				break;
			}
			pp = ESP_PORTS[i+1];
		}
	}

	if (p->ESPPort && p->ESPPort[0] == '\0') {
		fprintf(stderr, "Serial: PORT access admission needed.\n");
		return 1;
	}
	else if ((fd = open(p->ESPPort, O_RDWR)) < 0) {
		fprintf(stderr, "Serial: open error:%s\n", p->ESPPort);
		return 1 ;
	}
	bzero((void *) &tio, sizeof(tio));
	//tio.c_cflag = B57600 | CRTSCTS | CS8 | CLOCAL | CREAD;
	tio.c_cflag = B57600 | CS8 | CLOCAL | CREAD;
	tio.c_cc[VTIME] = 0; // no timer
	tio.c_cc[VMIN] = 1; // at least 1 byte
	//tcsetattr(fd, TCSANOW, &tio);
	cfsetispeed( &tio, B57600 );
	cfsetospeed( &tio, B57600 );

	cfmakeraw(&tio);
	tcsetattr( fd, TCSANOW, &tio );
	ioctl(fd, TCSETS, &tio);
	*pFd = fd;

	printf("ESP port: %s\n", p->ESPPort);
	
	return 0;
}

//
// support ESP3 functions
// Common response
//
ESP_STATUS GetResponse(OUT BYTE *Buffer)
{
	INT startMSec;
	INT timeout;
	INT length;
	BYTE *data;
	ESP_STATUS responseMessage;

	startMSec = SystemMSec();
	do {
		data = Dequeue(&ResponseQueue);
		if (data != NULL) {
			break;
		}
		timeout = SystemMSec() - startMSec;
		if (timeout > RESPONSE_TIMEOUT) {
			fprintf(stderr, "GetResponse: Timeout=%d\n", timeout);
			return TIMEOUT;
		}
		msleep(1);
	}
	while(1);
	_QD DRCount++;

	length = QueueGetLength(data);
	memcpy(Buffer, data, length);
	Enqueue(&FreeQueue, data);
	_QD QFCount++; // Enq FreeQueue

	switch(Buffer[5]) {
	case 0:
	case 1:
	case 2:
	case 3:
		responseMessage = Buffer[5];
		break;
	default:
		responseMessage = INVALID_STATUS;
		break;
	}

	//PacketDump(Buffer);
	//printf("**GetResponse=%d\n", responseMessage);
	return responseMessage;
}

//
//
void SendCommand(BYTE *cmdBuffer)
{
	int length = cmdBuffer[2] + 7;
	//printf("**SendCommand fd=%d len=%d\n", GetFd(), length);

	write(GetFd(), cmdBuffer, length);
}

//
//
//
void USleep(int Usec)
{
	const int mega = (1000 * 1000);
	struct timespec t;
	t.tv_sec = 0;

	int sec = Usec / mega;

	if (sec > 0) {
		t.tv_sec = sec;
	}
	t.tv_nsec = (Usec % mega) * 1000 * 2; ////DEBUG////
	nanosleep(&t, NULL);
}

//
//
char *MakePath(char *Dir, char *File)
{
	char path[PATH_MAX];
	char *pathOut;

	if (File[0] == '/') {
		/* Assume absolute path */
		return(strdup(File));
	}
	strcpy(path, Dir);
	if (path[strlen(path) - 1] != '/') {
		strcat(path, "/");
	}
	strcat(path, File);
	pathOut = strdup(path);
	if (!pathOut) {
		Error("strdup() error");
	}
	return pathOut;
}

//
//
int EoReadControl()
{
	EO_CONTROL *p = &EoControl;
	int modelCount;
	int csvCount;

	if (p->ControlPath == NULL) {
		p->ControlPath = MakePath(p->BridgeDirectory, p->ControlFile); 
	}
	if (p->ModelPath == NULL) {
		p->ModelPath = MakePath(p->BridgeDirectory, p->ModelFile); 
	}
	if (p->PublickeyPath == NULL) {
		p->PublickeyPath = MakePath(p->BridgeDirectory, p->PublickeyFile); 
	}
	modelCount = ReadModel(p->ModelPath);
	csvCount = ReadCsv(p->ControlPath);
	p->ControlCount = csvCount;

	if (p->Debug > 1) {
		printf("**EoReadControl: modelCount=%d csvCount=%d\n", modelCount, csvCount);
	}
	(void) CacheProfiles();

	return csvCount;
}

void EoClearControl()
{
	EO_CONTROL *p = &EoControl;

 	if (p->ControlPath == NULL) {
		p->ControlPath = MakePath(p->BridgeDirectory, p->ControlFile); 
	}
	if (p->ModelPath == NULL) {
		p->ModelPath = MakePath(p->BridgeDirectory, p->ModelFile); 
	}
	if (p->PublickeyPath == NULL) {
		p->PublickeyPath = MakePath(p->BridgeDirectory, p->PublickeyFile); 
	}

	if (truncate(p->ControlPath, 0)) {
		fprintf(stderr, "EoClearControl: %s: truncate error=%s\n", __FUNCTION__,
		p->ControlPath);
	}
	if (truncate(p->ModelPath, 0)) {
		fprintf(stderr, "EoClearControl: %s: truncate error=%s\n", __FUNCTION__,
		p->ModelPath);
	}
	p->ControlCount = 0;
	CmCleanUp();
	DeletePublickey(p->PublickeyPath);
}


bool EoApplyFilter()
{
	INT i;
	UINT id;
	BYTE pid[4];
	EO_CONTROL *p = &EoControl;

	CO_WriteFilterDelAll(); //Filter clear

	for(i = 0; i < p->ControlCount; i++) {
		id = GetId(i);
		if (id != 0) {
			pid[0] = ((BYTE *)&id)[3];
			pid[1] = ((BYTE *)&id)[2];
			pid[2] = ((BYTE *)&id)[1];
			pid[3] = ((BYTE *)&id)[0];
#ifdef FILTER_DEBUG			
			printf("**%s: %d: id:%02X%02X%02X%02X\n",
				__FUNCTION__, i, pid[0], pid[1], pid[2], pid[3]);
#endif
			CO_WriteFilterAdd(pid);
		}
		else {
			break;
		}
	}
	//FilterEnable();
	CO_WriteFilterEnable(TRUE);
	return true;
}

//
//
//
void EoParameter(int ac, char**av, EO_CONTROL *p)
{
	int mFlags = 1; //default
	int rFlags = 0;
	int oFlags = 0;
	int cFlags = 0;
	int vFlags = 0;
	int lFlags = 0; //websocket logflags
	int llFlags = 0; //local logfile logflags
	int pFlags = 0; //packet debug
	int xFlags = 0; //ERP2 on ESP3 flag
	int opt;
	int timeout = 0;
	int jsonPort = DEFAULT_JSON_PORT;
	char *controlFile = EO_CONTROL_FILE;
	char *commandFile = EO_COMMAND_FILE;
	char *brokerFile = BROKER_FILE;
	char *publickeyFile = PUBLICKEY_FILE;
	char *bridgeDirectory = EO_DIRECTORY;
	char *eepFile = EO_EEP_FILE;
	char *serialPort = "\0";
	char *modelFile = EO_MODEL_FILE;

	while ((opt = getopt(ac, av, "AcDjlLmopPqrvxb:d:e:f:g:J:k:s:t:z:")) != EOF) {
		switch (opt) {
		case 'A': //PrintProfileAll
			p->AFlags++;
			break;
		case 'D': //Debug
			p->Debug++;
			break;
		case 'm': //Monitor mode
			mFlags++;
			rFlags = oFlags = 0;
			break;
		case 'r': //Register mode
			rFlags++;
			mFlags = oFlags = 0;
			break;
		case 'o': //Operation mode
			oFlags++;
			mFlags = rFlags = 0;
			break;
		case 'c': //clear before register
			cFlags++;
			break;
		case 'v': //Verbose
			vFlags++;
			break;
		case 'x': //eXtended ESP
			xFlags++;
			break;
		case 'l': //WebSocket logger
			lFlags++;
			break;
		case 'L': //Local logfile logger
			llFlags++;
			break;
		case 'p': //packet debug
			pFlags++;
			break;
		case 'P': //supress packet debug
			pFlags = 0;
			break;
		case 'q': //quiet mode
			p->QuietMode++;
			break;
		case 'j': //JSON Service
			p->JsonServer++;
			break;
		case 'J': //JSON port
			p->JsonServer++;
			jsonPort = atoi(optarg);
			break;

		case 'f': //Control file name
			controlFile = optarg;
			break;
		case 'z': //command file name
			commandFile = optarg;
			break;
		case 'b': //broker file name
			brokerFile = optarg;
			break;
		case 'k': //public Key file name
			publickeyFile = optarg;
			break;
		case 'd': //bridge Directory name
			bridgeDirectory = optarg;
			break;
		case 'e': //eepfile name
			eepFile = optarg;
			break;
		case 'g': //model file name
			modelFile = optarg;
			break;

		case 't': //timeout secs for register
			timeout = atoi(optarg);
			break;
		case 's': //ESP serial port
			serialPort = optarg;
			break;
		default: /* '?' */
			fprintf(stderr,
				"Usage: %s [-m|-r|-o][-c][-v]\n"
				"  [-d Directory][-f Controlfile][-e EEPfile][-b BrokerFile]\n"
				"  [-s SeriaPort][-z CommandFile][-t timeout seconds]\n\n"
				"  Operation mode:\n"
				"    -m    Monitor mode\n"
				"    -r    Register mode\n"
				"    -o    Operation mode\n"
				"    -c    Clear settings before register\n"
				"  Output log options:\n"
				"    -l    Output websocket log for logger client\n"
				"    -L    Output local logfile log\n\n"
				"    -j    Provide JSON service (Experimental)\n"
				"    -J    JSON TCP socket service port (default:8000)\n"
				"  Runtime options:"
				"    -d directory   Bridge file directrory\n"
				"    -f file        Control file\n"
				"    -e eepfile     EEP file\n"
				"    -b brokerfile  Broker file\n"
				"    -s device      ESP3 serial port device name\n"
				"    -z commfile    Command file\n"
				"    -g modelfile   Generic Model file\n"
				"    -k publickeyfile Public key file\n"
				"    -t secs        Timeout seconds for register\n"
				"  Options for development:\n"
				"    -A    Print All (EEP) profile\n"
				"    -v    View working status (verbose message level)\n"
				"    -D    Add debug level\n"
				"    -p    Display packet debug\n"
				"    -P    Don't display packet debug (default)\n"
				"    -q    Quiet mode, don't display message without debug\n"
				,av[0]);

			CleanUp(0);
			exit(EXIT_FAILURE);
		}
	}
	p->Mode = oFlags ? Operation : rFlags ? Register : Monitor;
	p->CFlags = cFlags;
	p->VFlags = vFlags;
	p->Logger = lFlags;
	p->LocalLog = llFlags;
	p->Timeout = timeout;
	p->XFlags = xFlags;
	p->ControlFile = strdup(controlFile);
	p->CommandFile = strdup(commandFile);
	p->BrokerFile = strdup(brokerFile);
	p->PublickeyFile = strdup(publickeyFile);
	p->EEPFile = strdup(eepFile);
	p->BridgeDirectory = strdup(bridgeDirectory);
	p->ESPPort = serialPort;
	p->ModelFile = strdup(modelFile);
	if (lFlags) {
		// assume built in gateway use with web browser
		// websocket messages are enough to see status
		p->QuietMode++;
	}
	if (p->JsonServer) {
		p->JsonPort = jsonPort;
	}
	PacketDebug(pFlags);
	SecNoticeLevel(vFlags);
	ESP_Debug(p->Debug);
}

int MakeSCutFields(char *line, DATAFIELD *pd, int count)
{
	int i = 0;

	for(; i < count; i++, pd++) {
		char *pointName; //newShotCutName
		if (pd->DataName == NULL) {
			continue;
		}
		else if (pd->ShortCut == NULL || *pd->ShortCut == '\0') {
			continue;
		}
		else if (!strcmp(pd->DataName, "LRN Bit")
		    || !strcmp(pd->ShortCut, "LRNB")
		    || !strcmp(pd->DataName, "Learn Button")) {
			continue; // Skip Learn bit
		}
		pointName = GetNewName(pd->ShortCut);
		if (pointName == NULL) {
			Error("GetNewName error");
			pointName = "ERR";
		}
		strcat(line, ",");
		strcat(line, pointName);
		free(pointName);
		pointName = NULL;
	}
	return i;
}

int MakeSCutFieldsWithCurrent(char *line, DATAFIELD *pd, int count)
{
	int i;
	int scIndex;
	DATAFIELD *basePd = pd;
	char *currentSCuts[SC_SIZE];
	char *pointName; //newShotCutName

	for(i = 0; i < count; i++) {
		currentSCuts[i] = NULL;
	}
	scIndex = 0;
	for(i = 0; i < count; i++, pd++) {
		if (pd->DataName == NULL) {
			continue;
		}
		else if (pd->ShortCut == NULL || *pd->ShortCut == '\0') {
			continue;
		}
		else if (!strcmp(pd->DataName, "LRN Bit")
			 || !strcmp(pd->ShortCut, "LRNB")
			 || !strcmp(pd->DataName, "Learn Button")) {
			continue; // Skip Learn bit
		}
		
		pointName = pd->ShortCut;
#ifdef CT_DEBUG
		printf("With[%d]:<%s>\n", i, pointName); //***1
#endif
		pointName = GetNewNameWithCurrent(pointName, currentSCuts);
		if (pointName == NULL) {
			Error("GetNewName error");
			pointName = "ERR";
		}
#ifdef CT_DEBUG
		printf("With NewName[%d]<%s>\n", scIndex, pointName);
#endif
		currentSCuts[scIndex++] = pointName;
#ifdef CT_DEBUG
		printf("With[%d:%d]<%s><%s>\n", i, scIndex, pointName, currentSCuts[scIndex]); //***
#endif
		strcat(line, ",");
		strcat(line, pointName);
#ifdef CT_DEBUG
		printf("With[%d]!<%s>\n", i, pointName); //***2
#endif
	}
	pd = basePd;
	for(i = 0; i < count; i++, pd++) {
		if (currentSCuts[i] != NULL) {
			//if (currentSCuts[i] != pd->ShortCut) {
			free(currentSCuts[i]);
			//}
		}
		else break;
	}
	return scIndex;
}
	
void EoSetEep(EO_CONTROL *P, byte *Id, byte *Data, uint Rorg)
{
	uint func, type, man = 0;
	EEP_TABLE *eepTable;
	EEP_TABLE *pe;
	DATAFIELD *pd;
	FILE *f;
	struct stat sb;
	int rtn, scCount;
	BOOL isUTE = FALSE;
	time_t timep;
	struct tm *time_inf;
	char eep[12];
	char sEep[12];
	char idBuffer[12];
	char buf[DATABUFSIZ];
	char timeBuf[64];
	char *leadingBuffer;
	char *trailingBuffer;

	sprintf(idBuffer, "%02X%02X%02X%02X", Id[0], Id[1], Id[2], Id[3]);
	if (P->VFlags) {
		printf("EoSetEep:<%s>%s\n", idBuffer, Id[5] ? ":SEC" : "");
	}

	switch (Rorg) {
	case 0xF6: // RPS
		func = 0x02;
		type = P->ERP1gw ? 0x01 : 0x04;
		man = 0xb00;
		break;

	case 0xD5: // 1BS
		func = 0x00;
		type = 0x01;
		man = 0xb00;
		break;

	case 0xA5: // 4BS
		DataToEep(Data, &func, &type, &man);
		break;

	case 0xD2: //VLD
		func = 0x03;
		type = 0x20;
		man = 0xb00;
		break;

	case 0xD4: // UTE:
		Rorg = Data[6];
		func = Data[5];
		type = Data[4];
		man = Data[3] & 0x07;
		isUTE = TRUE;
		break;

	default:
		func = 0x00;
		type = 0x00;
		break;
	}
	if (!isUTE && man == 0) {
		fprintf(stderr, "EoSetEep: %s no man ID is set\n", idBuffer);
		return;
	}

	// Format EEP to string with secure mark //
	sprintf(eep, "%02X-%02X-%02X", Rorg, func, type);
	sprintf(sEep, "%s%s", Id[5] ? "!" : "", eep);
	
	eepTable = GetEep(eep);
	if (eepTable == NULL) {
		fprintf(stderr, "EoSetEep: %s EEP is not found=%s\n",
			idBuffer, eep);
		return;
	}
	if (P->ControlPath == NULL) {
		P->ControlPath = MakePath(P->BridgeDirectory, P->ControlFile); 
	}
	rtn = stat(P->BridgeDirectory, &sb);
	if (rtn < 0){
		mkdir(P->BridgeDirectory, 0777);
	}
	rtn = stat(P->BridgeDirectory, &sb);
	if (!S_ISDIR(sb.st_mode) || rtn < 0) {
		fprintf(stderr, "EoSetEep: Directory error=%s\n", P->BridgeDirectory);
		return;
	}

	// SetNewEep //
	timep = time(NULL);
	time_inf = localtime(&timep);
	strftime(timeBuf, sizeof(timeBuf), "%x %X", time_inf);
	sprintf(buf, "%s,%s,%s,%s", timeBuf, idBuffer, sEep, eepTable->Title);

	leadingBuffer = index(buf, ',') + 1; // buffer starts without time
	trailingBuffer = buf + strlen(buf);

	pe = eepTable;
	pd = pe->Dtable;
#if defined(CMD_DEBUG) || defined(CT_DEBUG)
	printf("*** PrintEep ***\n");
	PrintEep(eep);
#endif
	//scCount = MakeSCutFields(trailingBuffer, pd, pe->Size);
	scCount = MakeSCutFieldsWithCurrent(trailingBuffer, pd, pe->Size);
	if (scCount == 0) {
		fprintf(stderr, "EoSetEep: No shortcuts here at %s, %s.\n", idBuffer, eep);
		return;
	}

	f = fopen(P->ControlPath, "a+");
	if (f == NULL) {
		fprintf(stderr, "EoSetEep: cannot open control file=%s\n",
			P->ControlPath);
		return;
	}
	fwrite(leadingBuffer, strlen(leadingBuffer), 1, f);
	fwrite("\r\n", 2, 1, f);
	fflush(f);
	fclose(f);		

	if (P->VFlags) {
		printf("EoSetEep: Done <%s %s>\n", idBuffer, eep);
	}
	LogMessageRegister(buf);
}

void EoSetCm(EO_CONTROL *P, BYTE *Id, BYTE *Data, INT Length)
{
	FILE *f;
	struct stat sb;
	int rtn, i, scCount;
	time_t timep;
	struct tm *time_inf;

	CM_TABLE *pmc; // Pointer of Model Cache
	DATAFIELD *pd;
	char idBuffer[12];
	char buf[DATABUFSIZ];
	char timeBuf[64];
	char *pModel;
	char *leadingBuffer;
	char *trailingBuffer;
	
	extern CM_TABLE *CmGetModel(BYTE *Buf, INT Size);
	
	sprintf(idBuffer, "%02X%02X%02X%02X", Id[0], Id[1], Id[2], Id[3]);
	if (P->VFlags) {
		printf("EoSetCm:<%s>\n", idBuffer);
	}

	pmc = CmGetModel(Data, Length);
	if (pmc == NULL) {
		fprintf(stderr, "EoSetCm: %s invalid data for CM\n", idBuffer);
		return;
	}

	if (P->Debug > 1) {
		printf("EoSetCm: CmStr=%s, Length=%d\n", pmc->CmStr, Length);
#if MODEL_DEBUG
		if (pmc->Dtable != NULL) {
			pd = &pmc->Dtable[0];
			for(i = 0; i < pmc->Count; i++) {
				printf(" $$$ %d: DataName=%s(%p)\n", i, pd->DataName, pd->DataName);
				printf(" $$$ %d: ShortCut=%s(%p)\n", i, pd->ShortCut, pd->ShortCut);
				pd++;
			}
		}
		else {
			printf("EoSetCm: pmc->Dtable == NULL\n");
		}
#endif
	}

	if (P->ControlPath == NULL) {
		P->ControlPath = MakePath(P->BridgeDirectory, P->ControlFile); 
	}
	if (P->ModelPath == NULL) {
		P->ModelPath = MakePath(P->BridgeDirectory, P->ModelFile); 
	}
	rtn = stat(P->BridgeDirectory, &sb);
	if (rtn < 0){
		mkdir(P->BridgeDirectory, 0777);
	}
	rtn = stat(P->BridgeDirectory, &sb);
	if (!S_ISDIR(sb.st_mode) || rtn < 0) {
		fprintf(stderr, "EoSetCm: Directory error=%s\n", P->BridgeDirectory);
		return;
	}

	// SetNewCm //
	timep = time(NULL);
    time_inf = localtime(&timep);
	strftime(timeBuf, sizeof(timeBuf), "%x %X", time_inf);
	sprintf(buf, "%s,%s,%s,%s", timeBuf, idBuffer, pmc->CmStr, pmc->Title);

	leadingBuffer =  index(buf, ',') + 1; // buffer starts without time
	trailingBuffer = buf + strlen(buf);
	pd = pmc->Dtable;

    scCount = MakeSCutFieldsWithCurrent(trailingBuffer, pd, pmc->Count);
	if (scCount == 0) {
		fprintf(stderr, "EoSetCm:No shortcuts here at %s, %s.\n", idBuffer, pmc->CmStr);
		return;
	}
#if MODEL_DEBUG
	printf("$$CM: scCount=%d title<%s>\n", scCount, pmc->Title);
#endif
	f = fopen(P->ControlPath, "a+");
	if (f == NULL) {
		fprintf(stderr, "EoSetCm: cannot open control file=%s\n",
			P->ControlPath);
		return;
	}
	fwrite(leadingBuffer, strlen(leadingBuffer), 1, f);
	fwrite("\r\n", 2, 1, f);
	fflush(f);
	fclose(f);
	
	if (P->Debug > 1) {
		printf("$$Write CM-CSV\n");
		if (pmc->Dtable != NULL) {
			pd = &pmc->Dtable[0];
			for(i = 0; i < pmc->Count; i++) {
				printf(" $$$$ %d: DataName=%s(%p)\n", i, pd->DataName, pd->DataName);
				printf(" $$$$ %d: ShortCut=%s(%p)\n", i, pd->ShortCut, pd->ShortCut);
				pd++;
			}
		}
	}

    // Convert and save key-record
	f = fopen(P->ModelPath, "a+");
	if (f == NULL) {
		Error2("fopen error", P->ModelPath);
		return;
	}
	pModel = CmBinToText(Data, Length);
	if (pModel != NULL ) {
		fwrite(pModel, strlen(pModel), 1, f);
		fflush(f);
		if (P->Debug > 1) {
			printf("$CM:profile: <%s>\n", pModel);
		}
	}
	fclose(f);
	free(pModel);
	if (P->VFlags || P->Debug > 1) {
		printf("EoSetCm: Write Profile Done <%s %s>\n", idBuffer, pmc->CmStr);
	}
	LogMessageRegister(buf);
}

void LogMessageRegister(char *buf)
{
	EO_CONTROL *p = &EoControl;
	if (p->Logger) {
		BOOL loggedOK;
		//Warn("call MonitorMessage()");
		loggedOK = MonitorMessage(buf);
		if (p->VFlags) {
			printf("MsgRegister: MonitorMessage=%s\n",
			       loggedOK ? "OK" : "FAILED");
		}
	}
	if (p->LocalLog > 0) {
		EoLogRaw(buf);
	}
	if (!p->QuietMode) {
		printf("R:%s\n", buf);
	}
}

void LogMessageStart(uint Id, char *Eep, char *Prefix)
{
	EO_CONTROL *p = &EoControl;
	time_t      timep;
	struct tm   *time_inf;
	char        buf[64];
  
	timep = time(NULL);
	time_inf = localtime(&timep);
	strftime(buf, sizeof(buf), "%x %X", time_inf);

	if (p->JsonServer && Eep != NULL) {
		JsonTimeStamp(buf);
	}
	if (p->Logger > 0 || p->LocalLog > 0 || !p->QuietMode) {
		if (Eep != NULL) {
			sprintf(EoLogMonitorMessage, "%s,%08X,%s%s,", buf, Id, Prefix, Eep);
		}
		else {
			sprintf(EoLogMonitorMessage, "%s,%08X,", buf, Id);
		}
	}
}

void LogMessageAdd(char *Point, double Data, char *Unit)
{
	EO_CONTROL *p = &EoControl;
	char buf[128];
	if (p->JsonServer) {
		JsonAddData(Point, Data, Unit);
	}
	if (p->Logger > 0 || p->LocalLog > 0 || !p->QuietMode) {
		sprintf(buf, "%s=%.2lf%s ", Point, Data, Unit);
		strcat(EoLogMonitorMessage, buf);
	}
}

void LogMessageAddInt(char *Point, int Data)
{
	EO_CONTROL *p = &EoControl;
	char buf[128];
	if (p->JsonServer) {
		JsonAddInt(Point, Data);
	}
	if (p->Logger > 0 || p->LocalLog > 0 || !p->QuietMode) {
		sprintf(buf, "%s=%d ", Point, Data);
		strcat(EoLogMonitorMessage, buf);
	}
}

void LogMessageAddDbm(byte Data)
{
	EO_CONTROL *p = &EoControl;
	char buf[128];
	if (p->JsonServer) {
		JsonAddDbm(Data);
	}
	if (p->Logger > 0 || p->LocalLog > 0 || !p->QuietMode) {
		sprintf(buf, "-%d ", Data);
		strcat(EoLogMonitorMessage, buf);
	}
}

void LogMessageMonitor(char *Eep, char *Message)
{
	EO_CONTROL *p = &EoControl;
	char buf[DATABUFSIZ];
	if (p->Logger > 0 || p->LocalLog > 0 || !p->QuietMode) {
		sprintf(buf, "%s,%s", Eep ? Eep : NULL, Message);
		strcat(EoLogMonitorMessage, buf);
	}
}

void LogMessageOutput()
{
	EO_CONTROL *p = &EoControl;
	BOOL loggedOK;

	if (p->Mode == Operation) {
		if (p->Logger > 0) {
			//Warn("call MonitorMessage()");
			loggedOK = MonitorMessage(EoLogMonitorMessage);
			if (p->VFlags) {
				printf("MsgOut: MonitorMessage=%s\n",
					loggedOK ? "OK" : "FAILED");
			}
		}
		if (p->LocalLog > 0) {
			EoLogRaw(EoLogMonitorMessage);
		}
		if (!p->QuietMode) {
			printf("O:%s\n", EoLogMonitorMessage);
		}
	}
}

//
//
void PrintTelegram(EO_PACKET_TYPE PacketType, byte *Id, byte ROrg, byte *Data, int Dlen, int Olen)
{
	int i;
	int count, sum;
	UINT func, type, man;
	BOOL teachIn = FALSE;
	char buf[MAINBUFSIZ];
	char eepbuf[128];
	time_t      timep;
	struct tm   *time_inf;
	char timebuf[64];

	LogMessageStart(ByteToId(Id), NULL, NULL);
	eepbuf[0] = '\0';

	timep = time(NULL);
	time_inf = localtime(&timep);
	strftime(timebuf, sizeof(timebuf), "%x %X", time_inf);
	printf("M:%s %02X%02X%02X%02X (%d %d) ",
       timebuf, Id[0], Id[1], Id[2], Id[3], Dlen, Olen);

	switch(ROrg) {
	case 0xF6: //RPS
		teachIn = TRUE;
		sprintf(buf, "RPS:%02X -%d", Data[0], Data[Dlen + 4]);
		break;
		
	case 0xD5: //1BS
		teachIn = (Data[0] & 0x08) == 0;
		sprintf(buf, "1BS:%02X -%d", Data[0], Data[Dlen + 4]);
		break;

	case 0xA5: //4BS
		teachIn = (Data[3] & 0x08) == 0;
		sprintf(buf, "4BS:%02X %02X %02X %02X -%d",
		       Data[0], Data[1], Data[2], Data[3], Data[Dlen + 4]);
		break;

	case 0xD2: //VLD
		sum = sprintf(buf, "VLD:%02X ", Data[0]);
		for(i = 1; i <= (Dlen - 7); i++) {
			count = sprintf(buf + sum, "%02X ", Data[i]);
			sum += count;
		}
		sprintf(buf + sum, "-%d", Data[Dlen + 4]);
		break;

	case 0xD4: // UTE:
		teachIn = 1;
		sprintf(buf, "UTE:%02X %02X %02X %02X %02X %02X %02X -%d",
			Data[0], Data[1], Data[2], Data[3], Data[4], Data[5], Data[6], Data[Dlen + 4]);
		break;

	case 0xB0: // CM_TI:
	//case 0x05: // CM_TI on ERP2:
		teachIn = TRUE;
		CmPrintTI((BYTE *)buf, (BYTE *)eepbuf, (BYTE *)Data, Dlen - 6);
		break;

	case 0xB1: // CM_TR:
	//case 0x06: // CM_TR on ERP2:
		CmPrintTR((BYTE *)buf, (BYTE *)Data, Dlen - 6);
		break;

	case 0xB2: // CM_CD:
	//case 0x07: // CM_CD on ERP2:
		CmPrintCD((BYTE *)buf, (BYTE *)Data, Dlen - 6);
		break;

	case 0xB3: // CM_SD:
		CmPrintSD((BYTE *)buf, (BYTE *)Data, Dlen - 6);
		break;

	default:
		sprintf(buf, "Unknown R-ORG:%02X, %02X %02X %02X %02X %02X %02X %02X -%d",
			ROrg, 
			Data[0], Data[1], Data[2], Data[3], Data[4], Data[5], Data[6], Data[Dlen + 4]);
		break;
	}
	
	// EEP and teach-in
	if ((ROrg == 0xA5 || ROrg == 0xD5) && teachIn) {
		DataToEep(Data, &func, &type, &man);
		sprintf(eepbuf, "%02X-%02X-%02X %03x", ROrg, func, type, man);
	}
	else if (ROrg == 0xD4) { //UTE
		teachIn = TRUE;
		func = Data[5];
		type = Data[4];
		man = Data[3] & 0x07;
		sprintf(eepbuf, "%02X-%02X-%02X %03x", ROrg, func, type, man);
	}
	else if (ROrg == 0xF6) { //RPS
		teachIn = TRUE;
		func = 0x02;
		type = EoControl.ERP1gw ? 0x01 : 0x04;
		man = 0xb00;
		sprintf(eepbuf, "%02X-%02X-%02X %03x", ROrg, func, type, man);
	}
	else if (ROrg == 0xD2 && Dlen == 7 && Olen == 7
		&& Data[0] == 0x80 && Data[Dlen - 2] /*status*/ == 0x80) {
		// D2-03-20, special beacon
		teachIn = TRUE;
		strcpy(eepbuf, "D2-03-20");
	}
	
	if (teachIn) {
		printf("%s [%s]\n", buf, eepbuf);
	}
	else {
		printf("%s\n", buf);
	}

	LogMessageMonitor(eepbuf, buf);
	LogMessageOutput();
}

//
// Keep this function here for logging messages.
//
void WriteBridge(char *FileName, double ConvertedData, char *Unit)
{
	EO_CONTROL *p = &EoControl;
	FILE *f;
	char *bridgePath = MakePath(p->BridgeDirectory, FileName);
	BOOL isIntData = Unit == NULL || *Unit == '\0';
	INT data = (INT) ConvertedData;

	//printf("##%s: data=%d, isIntData=%d\n", FileName, data, isIntData); 
	
	f = fopen(bridgePath, "w");
	if (f == NULL) {
		Error2("fopen error", bridgePath);
		return;
	}
	if (isIntData) {
		LogMessageAddInt(FileName, data);
		fprintf(f, "%d\r\n", data);
	}
	else {
		LogMessageAdd(FileName, ConvertedData, Unit);
		fprintf(f, "%.2lf\r\n", ConvertedData);
	}
	fflush(f);
	fclose(f);
	////free(bridgePath);
}

//
//
//
void CleanUp(int Signum)
{
    EO_CONTROL *p = &EoControl;

	if (p->PidPath) {
		unlink(p->PidPath);
	}

	_QD QueueStatus();

	exit(0);
}

void SignalAction(int signo, void (*func)(int))
{
        struct sigaction act, oact;

        act.sa_handler = func;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;

        if (sigaction(signo, &act, &oact) < 0) {
                fprintf(stderr, "error at sigaction\n");
        }
}

void SignalCatch(int Signum)
{
	SignalAction(SIGUSR2, SIG_IGN);
}

//
// Brokers
//
typedef struct _brokers
{
	char *Name;
	char *PidPath;
	pid_t pid;

} BROKERS;

BROKERS BrokerTable[MAX_BROKER];

RETURN_CODE InitBrokers()
{
	EO_CONTROL *p = &EoControl;
	BROKERS *pb = &BrokerTable[0];
	char *pt;
	int i;
	FILE *f;
	char *rtn;
	char buf[DATABUFSIZ];

	if (p->BrokerPath != NULL) {
		return OK;
	}
	p->BrokerPath = MakePath(p->BridgeDirectory, p->BrokerFile);
	f = fopen(p->BrokerPath, "r");
	if (f == NULL) {
		printf("%s: no broker file=%s\n", __FUNCTION__, p->BrokerPath);
		return NO_FILE;
	}

	memset(&BrokerTable[0], 0, sizeof(BROKERS) * MAX_BROKER);

	for(i = 0; i < MAX_BROKER; i++) {
		rtn = fgets(buf, DATABUFSIZ, f);
		if (rtn == NULL) {
			break;
		}
		pt = &buf[0];
		while(isprint(*pt)) {
			pt++;
		}
		*pt = '\0'; // clear the last CR
#if 0
		pt = &buf[strlen(buf)] - 1;
		if (*pt == '\n') {
			*pt = '\0'; // clear the last CR
		}
#endif
		
#if SIG_DEBUG
		printf("%s %d: [%s]\n", __FUNCTION__, i, &buf[0]);

		printf("%s %d: %02X %02X %02X %02X %02X %02X %02X %02X\n", __FUNCTION__, i,
		       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
#endif
		pb->Name = strdup(buf);
		strcat(buf, ".pid");
		pb->PidPath = MakePath(p->BridgeDirectory, buf);
#if SIG_DEBUG
		printf("%s %d: [%s]\n", __FUNCTION__, i, pb->PidPath);
#endif
		pb++;
	}
	fclose(f);

	return (i > 0 ? OK : NO_FILE);
}

void NotifyBrokers(long num)
{
	enum { PIDLEN = 32 };
	BROKERS *pb = &BrokerTable[0];
	int i;
	FILE *f;
	char *rtn;
	char buf[PIDLEN];

	for(i = 0; i < MAX_BROKER; i++, pb++) {
#if SIG_DEBUG
		printf("$%s:%d name=%s path=%s\n",
		       __FUNCTION__, i, pb->Name, pb->PidPath);
#endif		
		if (pb->Name == NULL || pb->Name[0] == '\0') {
#if SIG_DEBUG
			printf("$%s: break\n", __FUNCTION__);
#endif		
			break;
		}
		f = fopen(pb->PidPath, "r");
		if (f == NULL) {
#if SIG_DEBUG
			Warn("no pid file");
#endif
			continue;
		}
		rtn = fgets(buf, PIDLEN, f);
		if (rtn == NULL) {
#if SIG_DEBUG
			Warn("pid file read error");
#endif
			fclose(f);
			continue;
		}
		pb->pid = (pid_t) strtol(buf, NULL, 10);
		fclose(f);

		// Notify to each broker
#if SIG_DEBUG
                printf("$%s:%d pid=%d num=%ld\n",
			__FUNCTION__, i, pb->pid, num);
#endif
                if (sigqueue(pb->pid, SIG_BROKERS, (sigval_t) ((void *) num)) < 0){
#if SIG_DEBUG		
                        Warn("sigqueue error");
#endif			
                }
#if SIG_DEBUG		
                printf("$%s:%d pid=%d END post\n",
                       __FUNCTION__, i, pb->pid);
#endif
	}
}

BOOL IsSecureCDM(BYTE *Buffer)
{
	//pb = &CdmBuffer[seq | (secureMark << 2)];
	INT i;
	//INT statSeq = 0 | (1 << 2);
	CDM_BUFFER *pb = &CdmBuffer[0 | (1 << 2)];
	for(i = 0; i < 4; i++, pb++) {
		if (*(UINT *) &pb->Id[0] == *(UINT *) Buffer) {
			// matched
			return TRUE;
		}
	}
	return FALSE;
}

void PushPacket(BYTE *Buffer)
{
	EO_CONTROL *p = &EoControl;
	CDM_BUFFER *pb;
	INT totalDataLength = 0;
	INT optionLength;
	INT length;
	INT thisDataLength;
	INT seq;
	INT index;
	INT secureMark;
	INT i;
	BYTE rOrg;
	BYTE *data;
	NODE_TABLE *nt = NULL;
	const INT erp1addrOffset = -5;
	const INT cdmMark = 1;    //CDM rORG, 0x40 or 0x33
	const INT seqIdx = 1;     //SEQ|IDX
	const INT cdmHeader = 2;  //LEN
	const INT rOrgByte = 1;   //real rORG
	const INT trailBytes = 5;  //SrcAddress(4) + status(1))

	length = (INT) (Buffer[0] << 8 | Buffer[1]);
	optionLength = (INT) Buffer[2];

	rOrg= Buffer[5];
	secureMark = rOrg == 0x33;
	data = &Buffer[HEADER_SIZE + cdmMark + seqIdx]; // Header(5) + R-ORG(1) + seq/index
	seq = Buffer[6] >> 6;
	index = Buffer[6] & 0x3F;

	thisDataLength = length - cdmMark - seqIdx - trailBytes + ((index == 0) & secureMark);

	if(p->Debug > 2) {
		printf("PushPacket Dump:\n");
		PacketDump(Buffer);
	}
#ifdef SECURE_DEBUG
	printf("PP Push: secureMark=%d\n", secureMark);
	printf("PP thisDatalen=%d len=%d ((index == 0) & secureMark)=%d\n",
		thisDataLength, length, ((index == 0) & secureMark));
#endif	
	// This method does not completely avoid collisions, but may reduce them somewhat.
	pb = &CdmBuffer[seq | (secureMark << 2)];

	if (index == 0) { // new CDM comming
		data += cdmHeader; // CDM Hdr(2)
		totalDataLength = (INT) (Buffer[7] << 8 | Buffer[8]);
		// ex. 14 -= 3 ==>11
		thisDataLength -= (cdmHeader + rOrgByte);

		pb->Length = totalDataLength;
		pb->CurrentLength = 0;

		pb->Id[0] = Buffer[HEADER_SIZE + length + erp1addrOffset];
		pb->Id[1] = Buffer[HEADER_SIZE + length + erp1addrOffset + 1];
		pb->Id[2] = Buffer[HEADER_SIZE + length + erp1addrOffset + 2];
		pb->Id[3] = Buffer[HEADER_SIZE + length + erp1addrOffset + 3];
		for(i = 0; i < HEADER_SIZE; i++) {
			pb->Buffer[i] = 0;
		}
		memcpy(&pb->Buffer[HEADER_SIZE], data, (rOrgByte & !secureMark) + thisDataLength);
#if SECURE_DEBUG
		printf("PP index==0: thisLen=%d copyLen=%d\n",
			thisDataLength, (rOrgByte & !secureMark) + thisDataLength);
		printf("PP-0: ");
		DataDump(&pb->Buffer[HEADER_SIZE], (rOrgByte & !secureMark) + thisDataLength);
#endif
	}
	else { // index > 0
		// Check address compare.
		//if (p->Debug > 2) {
		//	printf("IDCHK:pb->Id=%02X%02X%02X%02X *(UINT*)&pb->Id[]=%08X *(UINT*)&Buffer[]=%08X\n",
		//	       pb->Id[0], pb->Id[1], pb->Id[2], pb->Id[3],
		//	       *(UINT *) &pb->Id[0], *(UINT *) &Buffer[HEADER_SIZE + length  + erp1addrOffset]);
		//}
		if (*(UINT *) &pb->Id[0] == *(UINT *) &Buffer[HEADER_SIZE + length + erp1addrOffset]) {
			// copy data
			memcpy(&pb->Buffer[HEADER_SIZE + pb->CurrentLength + (rOrgByte & !secureMark)], data, thisDataLength);
		}
		else {
			fprintf(stderr, "%s: Registered ID mismatched %08X;%08X\n", __func__,
				*(UINT *) &pb->Id[0], *(UINT *) &Buffer[HEADER_SIZE + length + erp1addrOffset]);
			return;
		}
#if SECURE_DEBUG
		printf("PP index==%d: curLen=%d copyLen(thisLen)=%d\n",
			index, pb->CurrentLength, thisDataLength);
		printf("PP-%d: ", index);
		DataDump(&pb->Buffer[HEADER_SIZE + pb->CurrentLength + (rOrgByte & !secureMark)], thisDataLength);
#endif
	}

	if (p->Debug > 0) {
		printf("PP %d-%d: rorg=%02X len=%d cur=%d tot=%d this=%d\n",
			seq, index, rOrg, length, pb->CurrentLength, pb->Length, thisDataLength);
	}

	pb->CurrentLength += thisDataLength;
	if (pb->CurrentLength >= pb->Length) {
		// Have to Push packet
		BYTE   *dataBuffer;
		INT    dataLength;
		INT    totalLength;
		INT    count = 0;

		_QD printf("**PuchPacket: %d FreeQ=%d\n", PPCount++, QueueCount(&FreeQueue));
		do {
			dataBuffer = Dequeue(&FreeQueue);
			if (dataBuffer == NULL) {
				if (QueueTryTimes >= count) {
					fprintf(stderr, "PuchPacket: FreeQueue empty at PushPacket(MainJob)\n");
					QueueStatus();
					return;
				}
				count++;
				msleep(QueueTryWait);
			}
		}
		while(dataBuffer == NULL);

		//When Secure-Telegram, adjust encapslated R-Org length  
		//pb->Length -= secureMark;
		//The last packet, Make Header
		dataLength = pb->Length + (rOrgByte & !secureMark) + trailBytes;
#if SECURE_DEBUG
		printf("PP Last:");
		DataDump(&pb->Buffer[HEADER_SIZE], pb->Length);
#endif
		if (p->Debug > 0) {
			//Make Trailer
			printf("PP %d-%d: thisLen=%d Queue=%d,%d opt=%d\n", seq, index, thisDataLength,
			       pb->CurrentLength, pb->Length, optionLength);
		}

		if (secureMark) { // SEC_CDM (rOrg == 0x33)
			SECURE_REGISTER *ps;
			ULONG secId = ByteToId(&pb->Id[0]);
			SEC_HANDLE sec;
			INT status;
			INT decLength;
#ifdef SECURE_DEBUG
			BYTE nextRlc[4];
#endif
			/* Need to make new Header to DecBuffer !!*/
			pb->DecBuffer[0] = dataLength >> 8;
			pb->DecBuffer[1] = dataLength & 0xFF;
			pb->DecBuffer[2] = optionLength;
			pb->DecBuffer[3] = 1; // Type: RADIO_ERP1
			pb->DecBuffer[4] = 0; // CRC

			if (p->Debug > 3) {
				printf("PP: Copy &pb->DecBuffer[%d] << &Buffer[%d], len=%d\n",
					HEADER_SIZE + pb->Length - trailBytes, length, trailBytes + optionLength);
			}
			memcpy(&pb->DecBuffer[HEADER_SIZE + pb->Length],
				&Buffer[HEADER_SIZE + length - trailBytes], trailBytes + optionLength);

#ifdef SECURE_DEBUG
			printf("PP-org:"); PacketDump(&pb->DecBuffer[0]);
#endif
			ps = GetSecureRegister(secId);
			if (ps == NULL) {
				// not SecureRegistered, use NODE_TABLE
				PUBLICKEY *pt;
				BYTE rlc[MAX_RLC_SIZE];
				
				nt = GetTableId(secId);
				if (nt != NULL && nt->Secure != NULL) {
					sec = nt->Secure;
					SecGetRlc(sec, rlc);

					////////////////////////
					/* read RLC from file */
					pt = GetPublickey(secId);
					//ReadRlc(pt); //-->delete!
					status = SecCheck(sec, pt->Rlc);

					if (p->Debug > 1) {
						printf("SecCheck=%d, old=%02X%02X%02X%02X new=%02X%02X%02X%02X\n",
						       status, rlc[0], rlc[1], rlc[2], rlc[3],
						       pt->Rlc[0], pt->Rlc[1], pt->Rlc[2], pt->Rlc[3]);
					}
				}
				else {
					if (p->Debug > 1) {
						if (nt != NULL) {
							fprintf(stderr, "PushPacket: EEP=%p Secure=%p\n", nt->Eep, nt->Secure);
						}
						else {
							fprintf(stderr, "PushPacket: Secure GetTableId is NULL\n");
						}
					}
					return;
				}
			}
			else {
#ifdef SECURE_DEBUG
				PrintKey(ps);
				printf("SEC: %08lX: HeadSz=%d Len=%d OLen=%d Tot=%d\n",
				       secId, HEADER_SIZE, pb->Length, optionLength,
				       HEADER_SIZE + pb->Length + optionLength);
#endif
				sec = SecCreate(ps->Rlc, ps->Key, ps->RlcLength);
				if (sec == NULL) {
					Error("SecCreate error");
					return;
				}
				ps->Sec = sec;
			}

#ifdef SECURE_DEBUG
			printf("ORG(%d):", pb->Length - 1);
			for(i = 0; i < pb->Length - 1; i++) {
					printf(" %02X", pb->Buffer[i + HEADER_SIZE]);
			}
			printf("\n");
#endif

			pb->Length -= 8; // Actual decrypt length (== declength?)

			// Just in case, when some troubles happen...
			if (pb->Length < 2 || pb->Length > 64) {
				printf("####PP:Invalid Length=%d\n", pb->Length);
				pb->Length = 4;
				msleep(500);
			}
			SecCheck(sec, &pb->Buffer[HEADER_SIZE + pb->Length]);
			decLength = SecDecrypt(sec, &pb->Buffer[HEADER_SIZE], pb->Length, &pb->DecBuffer[HEADER_SIZE]);

			length = pb->DecBuffer[0] << 8 | pb->DecBuffer[1];
			optionLength = pb->DecBuffer[2];
#ifdef SECURE_DEBUG
			do {
				INT totalLen = HEADER_SIZE + length + optionLength;
				if (totalLen > 64) {
					totalLen = 64;
				}
				printf("DEC(%d,%d):", HEADER_SIZE + length + optionLength, totalLen);
				for(i = 0; i < totalLen; i++) {
					printf(" %02X", pb->DecBuffer[i]);
				}
				printf("\n");
			}
			while(0);
#endif
			if (p->Debug > 0) {
				printf("SEC: decLength=%d pb->Length=%d length=%d optionLength=%d\n",
					decLength, pb->Length, length, optionLength);
			}

			// Currently we don't care RLC and CMAC
			status = SecUpdate(sec);
			if (p->Debug > 0 && status <= 0) {
				printf("SecUpdate error=%d\n", status);
			}
#ifdef SECURE_DEBUG
			(void) SecGetRlc(sec, nextRlc);
			printf("SEC: ++nextRlc:%02X %02X %02X %02X\n", nextRlc[0], nextRlc[1], nextRlc[2], nextRlc[3]);
#endif
			totalLength = HEADER_SIZE + length + trailBytes + optionLength;
			memcpy(dataBuffer, pb->DecBuffer, totalLength);
		}
		else /* !secureMark */ {
		// CDM (rOrg == 0x40)

			pb->Buffer[0] = dataLength >> 8;
			pb->Buffer[1] = dataLength & 0xFF;
			pb->Buffer[2] = optionLength;
			pb->Buffer[3] = 1; // Type: RADIO_ERP1
			pb->Buffer[4] = 0; // CRC

			memcpy(&pb->Buffer[HEADER_SIZE + pb->Length + (rOrgByte & !secureMark)],
				&Buffer[HEADER_SIZE + length - trailBytes], trailBytes + optionLength);

			totalLength = HEADER_SIZE + pb->Length + trailBytes + optionLength;
			memcpy(dataBuffer, pb->Buffer, totalLength);
		}
		QueueData(&DataQueue, dataBuffer, totalLength);
	}
}

//
bool MainJob(BYTE *Buffer)
{
	EO_CONTROL *p = &EoControl;
	EO_PACKET_TYPE packetType;
	INT dataLength;
	INT optionLength;
	INT idCount;
	BYTE id[6];
	BYTE *data;
	BYTE rOrg;
	BYTE status = 0;
	BOOL teachIn = FALSE;
	BOOL newIdComming = FALSE;
	BOOL D2_Special = FALSE;
	BOOL isSecure = FALSE;
	NODE_TABLE *nt = NULL;
	SECURE_REGISTER *ps;
	extern void PrintItems(); //// DEBUG control.c
	extern void PrintProfileAll();
	extern int GetTableIndex(UINT Id);
	extern char *GetTableEep(uint Target);

	if (p->Debug > 3) {
		printf("MainJob:\n");
	}
	PacketAnalyze(Buffer); //Convert ERP2 to ERP1 when Register mode

	dataLength = (int) (Buffer[0] << 8 | Buffer[1]);
	optionLength = (int) Buffer[2];
	packetType = (EO_PACKET_TYPE) Buffer[3];
	rOrg = Buffer[HEADER_SIZE];
	idCount = -1;
	
	if (p->Debug > 1) {
		printf("MJ-AfterConvert:\n"); PacketDump(Buffer);
		if (p->Debug > 4) {
			printf("MJ:dLen=%d oLen=%d tot=%d typ=%02X org=%02X\n",
				dataLength, optionLength, (dataLength + optionLength),
				packetType, rOrg);
		}
	}
	
	data = &Buffer[HEADER_SIZE + 1]; // data points next to rOrg
	id[0] = data[dataLength - 6];    //rOrg(1) + AddrLen(4) + Stat(1)=6  
	id[1] = data[dataLength - 5];
	id[2] = data[dataLength - 4];
	id[3] = data[dataLength - 3];
	id[4] = id[5] = '\0';            // clear these for special security options
	status = data[dataLength - 2];   // Last data is status byte

	nt = GetTableId(ByteToId(id));
	newIdComming = nt == NULL;

	switch(rOrg) {
	case 0xF6: // RPS
		teachIn = TRUE; // RPS telegram. always teach In
		break;
	case 0xD5: // 1BS
        teachIn = (data[0] & 0x08) == 0;
		break;
	case 0xA5: // 4BS
        teachIn = (data[3] & 0x08) == 0;
		break;
	case 0xD2:  // VLD
		if (dataLength == 7 && optionLength == 7
		    && data[0] == 0x80 && status == 0x80) {
			// D2-03-20
			if (p->Debug > 0)
				printf("Special:VLD teachIn=D2-03-20\n");
			teachIn = TRUE;
			D2_Special = TRUE;
		}
		break;
	case 0xD4: // UTE
	case 0x35: // SEC_TI
        teachIn = TRUE;
		if (p->Debug > 1) {
			DataDump(data - 1, dataLength - 5);
		}
		break;

	case 0x30: // SEC
	case 0x31: // SECR
	case 0x32: // SECD
		if (p->Debug > 1) {
			DataDump(data - 1, dataLength - 5);
		}

		if (rOrg == 0x31 || p->Mode == Operation && rOrg == 0x30) {
            SECURE_REGISTER *ps;
            ULONG secId = ByteToId(&id[0]);
            SEC_HANDLE sec;
			INT validLength;
            INT decLength;
			INT cipherLength;
			INT rlcLength = 0;
      	    INT i;
			PUBLICKEY *pt;
			BYTE rlc[MAX_RLC_SIZE];
			BYTE decBuffer[DATABUFSIZ - 8 - HEADER_SIZE - 1];
			const int cmacLength = 4; 
#ifdef SECURE_DEBUG
			BYTE nextRlc[4];
			//printf("+P"); PacketDump(&pb->Buffer[0]);
			printf("call GetSecureRegister:%08lX(%02X)\n", secId, rOrg);
#endif
			ps = GetSecureRegister(secId);
			if (ps == NULL) {
				// not SecureRegistered, use NODE_TABLE
				nt = GetTableId(secId);
				if (nt != NULL && nt->Secure != NULL) {
					sec = nt->Secure;
					rlcLength = SecGetRlc(sec, rlc);

					/* read RLC from file */
					pt = GetPublickey(secId);
					//ReadRlc(pt); //-->delete!
					status = SecCheck(sec, pt->Rlc);

					if (p->Debug > 1) {
						printf("SECD:Reg SecCheck=%d rlclen=%d old=%02X%02X%02X%02X new=%02X%02X%02X%02X\n",
							status, rlcLength, rlc[0], rlc[1], rlc[2], rlc[3],
							pt->Rlc[0], pt->Rlc[1], pt->Rlc[2], pt->Rlc[3]);
					}
				}
				else {
					if (p->Debug > 1) {
						if (nt != NULL) {
							fprintf(stderr, "MJ: EEP=%p Secure=%p\n", nt->Eep, nt->Secure);
						}
						else {
							fprintf(stderr, "MJ: Secure GetTableId is NULL\n");
						}
					}
					break;
				}
			}
	        else {
#ifdef SECURE_DEBUG
				PrintKey(ps);
				printf("SECD: %08lX: HeadSz=%d Len=%d rlcLen=%d OLen=%d Tot=%d\n",
					secId, HEADER_SIZE, dataLength, ps->RlcLength, optionLength,
					dataLength + optionLength);
#endif
				sec = ps->Sec;
				if (sec== NULL) {
					fprintf(stderr, "MJ: ps but sec is NULL, %08lX:%d\n", secId, ps->Status);
					break;
				}
				rlcLength = ps->RlcLength;

				//nt = GetTableId(secId);
				//if (nt == NULL) {
				//	fprintf(stderr, "MJ: ps but nt is NULL, %08lX:%d\n", secId, ps->Status);
				//	break;
				//}
				//else {
#ifdef SECURE_DEBUG
				//	printf("SECD: existing ID comming\n");
#endif
				//	nt->Secure = sec;
				//}
			}
#ifdef SECURE_DEBUG
			printf("ORG:");
			for(i = 0; i < dataLength - 6; i++) {
				printf(" %02X", data[i]); //Print without Header
			}
			printf("\n");
#endif
			validLength = dataLength - cmacLength - rlcLength; // vLen = Truncate RLC and CMAC
			cipherLength = validLength - HEADER_SIZE - 1; // vLen - HDR(5) - rORG(1)
#ifdef SECURE_DEBUG
			do {
				int i;
				BYTE *px = &Buffer[0];

				printf("SecCheck: valid=%d cipherLen=%d: ", validLength, cipherLength);
				for(i = 0; i < validLength; i++) {
					printf("%02X ", px[i]);
				}
				//px = &Buffer[validLength];
				printf("\nRLC+CMAC: ");
				for(i = validLength; i < dataLength; i++) {
					printf("%02X ", px[i]);
				}
				printf("\n");
			}
			while(0);
#endif
			do {
				INT newLength;
				INT dataStart = 0;
				BYTE newOrg = 0;

				//This stops the explosion in case of mis-decryption.//
				if (cipherLength < 2 || cipherLength > 64) {
					printf("####MJ:Invalid cipherLength=%d\n", cipherLength);
					cipherLength = 2;
					msleep(500);
				}
				//// - - - ////

				(void) SecCheck(sec, &Buffer[validLength]);
				decLength = SecDecrypt(sec, &Buffer[HEADER_SIZE + 1],
					cipherLength, &decBuffer[0]);

				if (rOrg == 0x30 && nt != NULL && nt->Eep[0] != '\0') {
					dataStart = 1;
					i = 0;
					if (nt->Eep[i] == '!') { // Skip Secure mark
						i++;
					}
					newOrg = ((0xA + (nt->Eep[i] - 'A')) << 4) | (nt->Eep[i + 1] - '0');
					Buffer[HEADER_SIZE] = newOrg;
				}
				for(i = 0; i < cipherLength; i++) {
					Buffer[HEADER_SIZE + dataStart + i] = decBuffer[i];
				}
				// update new length = old length -1, and replace data

				newLength = dataLength - 1 + dataStart;
				if (dataStart == 0) {
					Buffer[0] = newLength >> 8;
					Buffer[1] = newLength & 0xFF;
					for(i = HEADER_SIZE + cipherLength; i < newLength; i++){
						Buffer[i] = Buffer[i + 1];
					}
					for(i = newLength; i < newLength + optionLength; i++){
						Buffer[i] = Buffer[i + 1];
					}
				}
#ifdef SECURE_DEBUG
				printf("DEC: newOrg=%02X cipLen=%d decLen=%d datSt=%d newLen=%d dLen=%d oLen=%d\n",
					newOrg, cipherLength, decLength, dataStart, 
					newLength, dataLength, optionLength);
#endif
			}
			while(0);

			rOrg = Buffer[HEADER_SIZE];

			// Decrypt completed //
			// Re-Inspect teachIn or Not //
			switch(rOrg) {
			case 0xF6: // RPS
				teachIn = TRUE; // RPS telegram. always Teach-In
				break;
			case 0xD5: // 1BS
				teachIn = (data[0] & 0x08) == 0;
				break;
			case 0xA5: // 4BS
				teachIn = (data[3] & 0x08) == 0;
				break;
			case 0xD2:  // VLD
				if (dataLength == 7 && optionLength == 7
					&& data[0] == 0x80 && status == 0x80) {
					// D2-03-20
					printf("**Sec VLD teachIn=D2-03-20\n");
					teachIn = TRUE;
					D2_Special = TRUE;
				}
				break;
			case 0xD4: // UTE
				teachIn = TRUE;
				break;
			}
			// Mark this is secure.
			isSecure = TRUE;
#ifdef SECURE_DEBUG
			printf("DEC:");
			for(i = 0; i < dataLength - HEADER_SIZE; i++) {
					printf(" %02X", Buffer[i + HEADER_SIZE]); //Print without Header
			}
			printf("\n");
#endif
			if (p->Debug > 1) {
				//#ifdef SECURE_DEBUG
				printf("SECDupd: rOrg=%02X %s declength=%d dataLength=%d\n",
					rOrg, teachIn ? "T" : "", decLength, dataLength);
			}
			// Currently we don't care RLC and CMAC
			status = SecUpdate(sec);
			if (p->Debug > 0 && status <= 0) {
				printf("SecUpdate error=%d\n", status);
			}
#ifdef SECURE_DEBUG
			(void) SecGetRlc(sec, nextRlc);
			printf("++nextRlc:%02X %02X %02X %02X\n", nextRlc[0], nextRlc[1], nextRlc[2], nextRlc[3]);
#endif
		} // if (rOrg == 0x31) // SECR
#ifdef SECURE_DEBUG
		printf("+D"); PacketDump(&Buffer[0]);
#endif
		break;

	case 0x40: // CDM
		PushPacket(Buffer);
		if (p->Debug > 1) {
			printf("PushedData(rORG=0x40):");
			DataDump(data, dataLength - 6);
		}
		break;

	case 0x33: // SEC_CDM
		if (nt != NULL && nt->Secure != NULL) {
			if (p->Debug > 3) {
				printf("SEC_CDM: PushPacket! registerd ID\n");
			}
			PushPacket(Buffer);
		}
		else {
			ps = GetSecureRegister(ByteToId(id));
			if (ps == NULL) {
				if (p->Debug > 2) {
					printf("SEC_CDM: New SEC_CDM /wo SEC_TI/Register\n");
				}
				PushPacket(Buffer);
			}
			else if (ps->Status == FIRST_COME) {
				if (p->Debug > 3) {
					printf("SEC_CDM: Ststus FIRST_COME\n");
				}
			}
			else if (ps->Status == REGISTERED) {
				if (p->Debug > 2) {
					printf("SEC_CDM: PushPacket REGISTERED\n");
				}
				PushPacket(Buffer);
			}
			else {
				if (p->Debug > 0) {
					printf("SEC_CDM: no-push ps->Status=%d\n", ps->Status);
				}
			}
		}
		if (p->Debug > 1) {
			printf("PushedData(rORG=0x33):");
			DataDump(data, dataLength - 6);
		}
		break;

	case 0xB0: // CM_TI:
	//case 0x05: // CM_TI on ERP2:
		teachIn = TRUE; // CM Teach-In
	case 0xB1: // CM_TR:
	//case 0x06: // CM_TR on ERP2:
	case 0xB2: // CM_CD:
	//case 0x07: // CM_CD on ERP2:
	case 0xB3: // CM_SD:
	case 0xD0: // SMACK_SIGNAL:
		if (p->Debug > 2) {
			printf("MJ-Data:%02X: ", rOrg);
			DataDump(data, dataLength - 6);
		}
		break;
		
	default:
		fprintf(stderr, "MJ: %s: Unknown rOrg=%02X pType=%02X (%d %d)\n",
			__FUNCTION__, rOrg, packetType, dataLength, optionLength);
		do {
			char bu[32];
			sprintf(bu, "invalid R-ORG=0x%02X Type=%02X", rOrg, packetType);
			EoLog((char *)id, "Unknown", bu);
		}
		while(0);
		//return false;
		break;
	} // End switch(rOrg)

	if (packetType != RadioErp1) {
		fprintf(stderr, "MJ: %s: Unknown type = %02X (%d %d)\n",
			__FUNCTION__, packetType, dataLength, optionLength);
		//return false;
	}

	if (p->VFlags) {
		printf("MJ:mode:%s id:%02X%02X%02X%02X rOrg:%02X %s\n", //here!
			p->Mode == Register ? "Register" : p->Mode == Operation ? "Operation" : "Monitor",
			id[0] , id[1] , id[2] , id[3], rOrg, teachIn ? "T" : "");
	}

	if (teachIn && p->Mode == Register) {
		SECURE_REGISTER *ps;
		UINT secId = ByteToId(id);
		
#if CMD_DEBUG
		printf("!C!teachIn && p->Mode == Register\n");
		printf("!C! newIdComming=%d\n", newIdComming);
#endif
		switch(rOrg) {
		case 0xF6: //RPS
		case 0xD5: //1BS
		case 0xD2: //VLD
		case 0xA5: //4BS
		case 0xD4: //UTE
			if (newIdComming) {
				PUBLICKEY *pt;
#if SECURE_DEBUG
				printf("!S! isSecure=%d IsSecureCDM=%d\n", isSecure, IsSecureCDM(id));
#endif
				if (isSecure |= IsSecureCDM(id)) {
					id[5] = '!';
				}
				EoSetEep(p, id, data, rOrg);
				idCount = EoReadControl();

				if (isSecure) {
					ReloadPublickey(p->PublickeyPath);
					ps = GetSecureRegister(secId);
					if (ps == NULL) {
						if (p->Debug > 2) {
							Warn("No secure telegram");
						}
					}
					else if (ps != NULL && ps->Status == REGISTERED) {
						nt = GetTableId(secId);
						if (nt == NULL) {
							Error("GetTableId-2");
							break;
						}
						nt->Secure = ps->Sec; // SEC_HANDLE
						pt = AddPublickey(p, secId, ps);
						if (pt == NULL) {
							Error("AddPublickey");
							return FALSE;
						}
						else {
							if (p->Debug > 1) {
								printf("TI: RLC path:%s\n", pt->RlcPath);
							}
						}
						ClearSecureRegister(secId);
					}
					else {
						fprintf(stderr, "MJ: TI: Unkown error status=%d", ps->Status);
					}
				}
			}
#if CMD_DEBUG
			else printf("!C!No newID-1\n");
			printf("!C!ID Count=%d\n", idCount);
#endif
			break;
			
		case 0xB0: // CM_TI:
		//case 0x05: // CM_TI on ERP2:
			// Note that CM_TI doesn't allow secure telegram.
			if (newIdComming) {
				EoSetCm(p, id, data, dataLength - 6 /* 6=rOrg+SrcAddr+Status */);
				idCount = EoReadControl();
			}
#if CMD_DEBUG
			else {
				printf("!C!No newID-2\n");
			}
			printf("!C!ID Count=%d\n", idCount);
#endif
			break;

		case 0x35: // SEC_TI
			secId = ByteToId(id);
			SEC_HANDLE sec;

			//if (!newIdComming) {
			//	// already registered, TODO: NEED check to update the Key and RLC
			//	break;
			//}
			ps = GetSecureRegister(secId);
			if (ps == NULL) {
				ps = NewSecureRegister();
				if (ps == NULL) {
					Error("NewSecureRegister no space");
					return FALSE;
				}
				ps->Id = secId;
				ps->Status = FIRST_COME;
				ps->Info = data[0];
				ps->Slf = data[1];
				ps->RlcLength = RlcLength(ps->Slf);
				memcpy(ps->Rlc, &data[2], ps->RlcLength);
				memcpy(ps->Key, &data[2 + ps->RlcLength], 8);

				//26=(1:rOrg + 2:SecHdr + 16:Key + 2:MinRlc + 4:SrcAddr + 1:Stat)
				if (dataLength >= 26) {
#ifdef SECURE_DEBUG
					printf("SECURE1: Long Secure TeachIn(>26)=%d\n", dataLength);
#endif
					memcpy(ps->Key, &data[2 + ps->RlcLength], 16);
					ps->Status = REGISTERED;
					sec = SecCreate(ps->Rlc, ps->Key, ps->RlcLength);
					if (sec == NULL) {
						Error("SecCreate error");
						break;
					}
					ps->Sec = sec;
				}
#ifdef SECURE_DEBUG
				printf("SECURE1: data[0]=%02X Slf=%02X RlcLen=%d\n",
					data[0], ps->Slf, ps->RlcLength);
				PrintKey(ps);
#endif
			}
			else if (ps->Status == FIRST_COME && (data[0] & 0xF0) == 0x40) {
				// This is the second TeachIn
				memcpy(&ps->Key[8], &data[1], 8);
				ps->Status = REGISTERED;
#ifdef SECURE_DEBUG
				printf("SECURE2: data[0]=%02X Slf=%02X RlcLen=%d\n",
					data[0], ps->Slf, ps->RlcLength);
				PrintKey(ps);
#endif
				sec = SecCreate(ps->Rlc, ps->Key, ps->RlcLength);
				if (sec == NULL) {
					Error("SecCreate error");
					break;
				}
				ps->Sec = sec;

#ifdef SECURE_DEBUG
				do {
					INT i;
					BYTE rlc[4];
					(void) SecGetRlc(ps->Sec, rlc);
					printf("=+Rlc:");
					for(i = 0; i < ps->RlcLength; i++) {
						printf("%02X", rlc[0]);
					}
					printf("\n");
				}
				while(0);
#endif
			}
#ifdef SECURE_DEBUG
			else {
				printf("SECUREX: Status=%d data[0]=%02X data[1]=%02X\n", ps->Status, data[0], data[1]);
			}
#endif
			break;
			
		default:
			break;
		}
		if (p->Debug > 1) {
			printf("TI&Reg: IDCount=%d newID=%d\n",
			       idCount, newIdComming);
		}
	}
	else if (p->Mode == Monitor) {
#if CMD_DEBUG
		printf("!C!p->Mode == Monitor\n");
#endif
		PrintTelegram(packetType, id, rOrg, data, dataLength, optionLength);
	}
	//else if (p->Mode == Operation || teachIn && (rOrg == 0xD2 || rOrg == 0xF6)) {
	else if (p->Mode == Operation) {
		int nodeIndex = -1;
		uint uid = ByteToId(id);

#if CMD_DEBUG
		printf("!C!p->Mode == Operation\n");
#endif
		if (p->JsonServer) {
			JsonCreate(JsonData, uid, GetTableEep(uid), rOrg, (UINT) isSecure);
		}

		switch(rOrg) {
		case 0xF6: //RPS:
			LogMessageStart(uid, GetTableEep(uid), isSecure ? "!" : "");
			if (CheckTableId(ByteToId(id))) {
				WriteRpsBridgeFile(ByteToId(id), data);
				nodeIndex = GetTableIndex(ByteToId(id));
				if (nodeIndex >= 0) {
					NotifyBrokers((long) nodeIndex);
				}
			}
			LogMessageAddDbm(data[dataLength + 4]);
			LogMessageOutput();
			if (p->JsonServer) {
				JsonRelease(&JsonQueue);
			}
			break;

		case 0xD5: //1BS:
			LogMessageStart(uid, GetTableEep(uid), isSecure ? "!" : "");
			if (!teachIn && CheckTableId(ByteToId(id))) {
				Write1bsBridgeFile(ByteToId(id), data);
				nodeIndex = GetTableIndex(ByteToId(id));
				if (nodeIndex >= 0) {
					NotifyBrokers((long) nodeIndex);
				}
			}
			LogMessageAddDbm(data[dataLength + 4]);
			LogMessageOutput();
			if (p->JsonServer) {
				JsonRelease(&JsonQueue);
			}
			break;
			
		case 0xD2: //VLD:
			LogMessageStart(uid, GetTableEep(uid), isSecure ? "!" : "");
			if (D2_Special || !teachIn && CheckTableId(ByteToId(id))) {
				WriteVldBridgeFile(ByteToId(id), data);
				nodeIndex = GetTableIndex(ByteToId(id));
				if (nodeIndex >= 0) {
					if (!teachIn)
						NotifyBrokers((long) nodeIndex);
				}
			}
			LogMessageAddDbm(data[dataLength + 4]);
			LogMessageOutput();
			if (p->JsonServer) {
				JsonRelease(&JsonQueue);
			}
			break;

		case 0xA5: //4BS:
			LogMessageStart(uid, GetTableEep(uid), isSecure ? "!" : "");
			if (!teachIn && CheckTableId(ByteToId(id))) {
				Write4bsBridgeFile(ByteToId(id), data);
				nodeIndex = GetTableIndex(ByteToId(id));
				if (nodeIndex >= 0) {
					if (!teachIn)
						NotifyBrokers((long) nodeIndex);
				}
			}
			LogMessageAddDbm(data[dataLength + 4]);
			LogMessageOutput();
			if (p->JsonServer) {
				JsonRelease(&JsonQueue);
			}
			break;

		case 0xB2: //CM_CD:
		//case 0x07: //CM_CD on ERP2:
			LogMessageStart(uid, GetTableEep(uid), isSecure ? "!" : "");
			if (CheckTableId(ByteToId(id))) {
				WriteCdBridgeFile(ByteToId(id), data);
				nodeIndex = GetTableIndex(ByteToId(id));
				if (nodeIndex >= 0) {
					if (!teachIn)
						NotifyBrokers((long) nodeIndex);
				}
			}
			LogMessageAddDbm(data[dataLength + 4]);
			LogMessageOutput();
			if (p->JsonServer) {
				JsonRelease(&JsonQueue);
			}
			break;

		case 0xB3: //CM_SD:
			LogMessageStart(uid, GetTableEep(uid), isSecure ? "!" : "");
			if (CheckTableId(ByteToId(id))) {
				WriteSdBridgeFile(ByteToId(id), data);
				nodeIndex = GetTableIndex(ByteToId(id));
				if (nodeIndex >= 0) {
					if (!teachIn)
						NotifyBrokers((long) nodeIndex);
				}
			}
			LogMessageAddDbm(data[dataLength + 4]);
			LogMessageOutput();
			if (p->JsonServer) {
				JsonRelease(&JsonQueue);
			}
			break;

		case 0xD4: //UTE
		//case 0x05: //CM_TI on ERP2:
		//case 0xB0: //CM_TI:
		//case 0xB1: //CM_TR:
		//case 0xB6: //CM_TR on ERP2:
		//case 0x40: //CDM
		//case 0x33: //SEC_CDM
		//case 0x35: //SEC_TI			
		default:
			break;
		}
	}

	if (p->Debug > 2 && 
		((teachIn && p->Mode == Register) || p->Mode == Operation)) {
		PrintItems();
	}
	return TRUE;
}

//
//
//
int main(int ac, char **av)
{
	pid_t myPid = getpid();
	CMD_PARAM param;
	JOB_COMMAND cmd;
	int fd;
	int rtn, i;
	FILE *f;
	pthread_t thRead;
	pthread_t thAction;
	THREAD_BRIDGE *thdata;
	bool working;
	bool initStatus;
	BYTE versionString[64];
	EO_CONTROL *p = &EoControl;
	ERP_MODE modeRegister;
	ERP_MODE modeOperation;
	ERP_MODE modeMonitor;
	ERP_MODE modeSet;
        struct stat sb;
	extern void PrintProfileAll();

	if (p->Debug > 1) {
		printf("**main() pid=%d\n", myPid);
	}
	printf("%s%s", version, copyright);

	// Signal handling
	SignalAction(SIGPIPE, SIG_IGN); // need for socket_io
	SignalAction(SIGINT, CleanUp);
	SignalAction(SIGTERM, CleanUp);
	SignalAction(SIGUSR2, SIG_IGN);
	////////////////////////////////
	// Stating parateters, profiles, and files

	memset(EoFilterList, 0, sizeof(long) * EO_FILTER_SIZE);

	p->Mode = Monitor; // Default mode
#if CMD_DEBUG
	printf("!C!%s: change mode to Monitor\n", __FUNCTION__);
#endif
	EoParameter(ac, av, p);

        rtn = stat(p->BridgeDirectory, &sb);
	if (rtn < 0){
		mkdir(p->BridgeDirectory, 0777);
	}
	rtn = stat(p->BridgeDirectory, &sb);
	if (!S_ISDIR(sb.st_mode) || rtn < 0) {
		fprintf(stderr, "EoSetEep: Directory error=%s\n", p->BridgeDirectory);
		exit(0);
	}

	p->PidPath = MakePath(p->BridgeDirectory, PID_FILE);

	f = fopen(p->PidPath, "w");
	if (f == NULL) {
		fprintf(stderr, "main: cannot create pid file=%s\n",
			p->PidPath);
		exit(0);
	}
	fprintf(f, "%d\n", myPid);
	fclose(f);
	////free(p->PidPath);
	if (InitBrokers() != OK) {
		fprintf(stderr, "main: No broker notify mode\n");
	}

	if (p->LocalLog) {
		(void) EoLogInit("dpr", ".log");
	}

	if (p->Logger) {
		MonitorStart();
	}

	switch(p->XFlags) {
	case 1:
		modeRegister = ERP1;
		modeOperation = ERP1;
		modeMonitor = ERP1;
		break;
	case 2:
		modeRegister = ERP2;
		modeOperation = ERP2;
		modeMonitor = ERP1;
		break;
	case 3:
		modeRegister = ERP2;
		modeOperation = ERP2;
		modeMonitor = ERP2;
		break;
	case 0:
	default:
		modeRegister = ERP2;
		modeOperation = ERP1;
		modeMonitor = ERP1;
		break;
	}

	switch(p->Mode) {
	case Monitor:
		p->FilterOp = Ignore;
		modeSet = modeMonitor;
		if (p->VFlags)
			fprintf(stderr, "main: Start monitor mode.\n");
		break;

	case Register:
		p->FilterOp = p->CFlags ? Clear : Ignore;
		modeSet = modeRegister;
		if (p->VFlags)
			fprintf(stderr, "main: Start register mode.\n");
		break;

	case Operation:
		p->FilterOp = Read;
		modeSet = modeOperation;
		if (p->VFlags)
			fprintf(stderr, "main: Start operation mode.\n");
		break;
	}

	if (!InitEep(p->EEPFile)) {
		fprintf(stderr, "main: InitEep error=%s\n", p->EEPFile);
		CleanUp(0);
		exit(1);
	}

	InitSecureRegister();
	(void) SecInit(); // needed before EoReadControl() && ReloadPublickey
	
	if (p->FilterOp == Clear) {
		EoClearControl();
	}
	else {

#if SECURE_DEBUG
		printf("!S! main: mode=%s\n", p->Mode == Register ? "Register"
			: p->Mode == Operation ? "Operation" : "Monitor");
#endif
		p->ControlCount = EoReadControl();
		ReloadPublickey(p->PublickeyPath);	
	}
	if (p->AFlags) {
		PrintEepAll();
		PrintProfileAll();
	}

	////////////////////////////////
	// Threads, queues, and serial pot

	STAILQ_INIT(&DataQueue);
	STAILQ_INIT(&ResponseQueue);
	STAILQ_INIT(&ExtraQueue);
	STAILQ_INIT(&FreeQueue);
	STAILQ_INIT(&JsonQueue);
	FreeQueueInit();	
	pthread_mutex_init(&DataQueue.lock, NULL);
	pthread_mutex_init(&ResponseQueue.lock, NULL);
	pthread_mutex_init(&ExtraQueue.lock, NULL);
	pthread_mutex_init(&FreeQueue.lock, NULL);
	pthread_mutex_init(&JsonQueue.lock, NULL);

	if (InitSerial(&fd)) {
		fprintf(stderr, "main: InitSerial error\n");
		CleanUp(0);
		exit(1);
	}

	thdata = calloc(sizeof(THREAD_BRIDGE), 1);
	if (thdata == NULL) {
		printf("calloc error\n");
		CleanUp(0);
		exit(1);
	}
	SetFd(fd);

	rtn = pthread_create(&thAction, NULL, ActionThread, (void *) thdata);
	if (rtn != 0) {
		fprintf(stderr, "main: pthread_create() ACTION failed for %d.",  rtn);
		CleanUp(0);
		exit(EXIT_FAILURE);
	}

	rtn = pthread_create(&thRead, NULL, ReadThread, (void *) thdata);
	if (rtn != 0) {
		fprintf(stderr, "main: pthread_create() READ failed for %d.",  rtn);
		CleanUp(0);
		exit(EXIT_FAILURE);
	}

	thdata->ThAction = thAction;
	thdata->ThRead = thRead;
	SetThdata(*thdata);

	if (p->Debug > 1)
        printf("**main() started fd=%d(%s) mode=%d(%s) Clear=%d\n",
	       fd, p->ESPPort, p->Mode, CommandName(p->Mode), p->CFlags);

	while(!read_ready) {
		if (p->Debug > 1)
			printf("**main() Wait read_ready\n");
		msleep(100);
	}
	if (p->Debug > 1)
		printf("**main() read_ready\n");

	p->ERP1gw = 0;	
	rtn = 0;
	do {
		if (p->Debug > 1) {
			printf("**main() WriteReset\n");
		}
		initStatus = CO_WriteReset();
		if (initStatus == OK) {
			msleep(200);

			initStatus = CO_WriteMode(modeSet); // ERP1:0 or ERP2:1
			if (p->Debug > 1) {
				printf("**main() WriteMode=ESP%d\n", modeSet ? 2 : 1);
			}
		}
		if (initStatus == RET_NOT_SUPPORTED) {
			printf("**main() Oops! this GW should be ERP1, and mark it.\n");
			p->ERP1gw = 1;
			break;
		}
		else if (initStatus != OK) {
			fprintf(stderr, "**main() Reset/WriteMode status=%d\n", initStatus);
			msleep(300);
			rtn++;
			if (rtn > 5) {
				fprintf(stderr, "**main() Error! Reset/WriteMode failed=%d\n", rtn);
				CleanUp(0);
				exit(EXIT_FAILURE);
			}
		}
	}
	while(initStatus != OK);

	if (p->VFlags) {
		CO_ReadVersion(versionString);
	}

	if (p->JsonServer) {
		JsonSetup(p->JsonPort, &JsonQueue);
	}
	if (p->Mode == Operation && p->ControlCount > 0) {
		if (p->VFlags) {
			printf("EoApplyFilter()\n");
		}
		EoApplyFilter();
		if (p->JsonServer) {
			(VOID) JsonStart();
		}
	}

	for(i = 0; i < CDM_TABLE_SIZE; i++) {
		CdmBuffer[i].Buffer = malloc(CDM_BUFFER_SIZE);
		CdmBuffer[i].DecBuffer = malloc(CDM_BUFFER_SIZE);
		if (CdmBuffer[i].Buffer == NULL || CdmBuffer[i].DecBuffer == NULL) {
			Error("malloc CdmBuffer(s)");
			CleanUp(0);
			exit(EXIT_FAILURE);
		}
	}

	///////////////////
	// Interface loop
	working = TRUE;
	while(working) {
		int newMode;

		printf("Wait...\n");
		SignalAction(SIGUSR2, SignalCatch);
		// wait signal forever
		while(sleep(60*60*24*365) == 0)
			;
		cmd = GetCommand(&param);
#ifdef CMD_DEBUG
		printf("!CDEBUG!: main loop()=%d\n", cmd);
#endif
		switch(cmd) {
		case CMD_NOOP:
			if (p->Debug > 0)
				printf("cmd:NOOP\n");
			sleep(1);
			break;

		case CMD_FILTER_ADD: //Manual-add, not implemented
			/* Now! need debug */
			//BackupControl();
			//FilterAdd() and
			//CO_WriteFilterAdd(param.Data);
			//CO_WriteFilterEnable
			break;

		case CMD_FILTER_CLEAR:
			/* Now! need debug */
			//BackupControl();
			EoClearControl();
			//CO_WriteReset();
			CO_WriteFilterDelAll();
			CO_WriteFilterEnable(OFF);
			break;

		case CMD_CHANGE_MODE:
			newMode = param.Data[0];
			if (p->Debug > 0) {
				printf("CHANGE_MODE: newMode=%c\n", newMode);
			}

			switch(newMode) {
			case 'M':
				modeSet = modeMonitor;
				break;
			case 'O':
				modeSet = modeOperation;
				break;
			case 'R':
			default:
				modeSet = modeRegister;
				break;
			}

			initStatus = CO_WriteMode(modeSet); // ERP1:0 or ERP2:1
			if (p->Debug > 0) {
				printf("cmd=newMode: WriteMode=ESP%d\n", modeSet ? 2 : 1);
			}

			if (newMode == 'M' /*Monitor*/) {
				JsonStop();
				/* Now! need debug */
				if (p->Debug > 0)
					printf("cmd=newMode:Monitor\n");
				p->Mode = Monitor;
				CO_WriteFilterDelAll();
				CO_WriteFilterEnable(OFF);
			}
			else if (newMode == 'R' /*Register*/) {
				JsonStop();
				if (p->Debug > 0)
					printf("cmd=newMode:Register\n");
				p->Mode = Register;
				CO_WriteFilterEnable(OFF);
				/* Now! Enable Teach IN */
			}
			else if (newMode == 'C' /*Clear and Register*/) {
				JsonStop();
				if (p->Debug > 0)
					printf("cmd=newMode:Clear and Register\n");
				EoClearControl();
				CO_WriteFilterDelAll();
				ClearTableId();
				p->ControlCount = 0;

				p->Mode = Register;
				CO_WriteFilterEnable(OFF);
				//CO_WriteReset(); // no need
			}
			else if (newMode == 'O' /* Operation */) {
#if SECURE_DEBUG
				printf("!S! main: CMD_CHANGE_MODE, mode=O\n");
#endif
				p->Mode = Operation; 
				p->FilterOp = Read;
				p->ControlCount = EoReadControl();
				if (p->Debug > 0) {
					printf("cmd=newMode:Operation ControlCount=%d\n",
						p->ControlCount);
				}
				if (p->ControlCount > 0) {
					if (!EoApplyFilter()) {
						fprintf(stderr, "main: EoApplyFilter error\n");
					}
					if (p->JsonServer) {
						(VOID) JsonStart();
					}
				}
			}
			break;

		case CMD_CHANGE_OPTION:
			newMode = param.Data[0];
			if (p->Debug > 0) {
				printf("newOpt=%c\n", newMode);
			}

			if (newMode == 'S' /*Silent*/) {
				p->VFlags = FALSE;
				p->Debug  = 0;
			}
			else if (newMode == 'V' /*Verbose*/) {
				p->VFlags  = TRUE;
			}
			else if (newMode == 'D' /*Debug*/) {
				p->Debug++;
			}
			break;

		case CMD_SHUTDOWN:
		case CMD_REBOOT:
			Shutdown(&param);
			break;

		default:
			printf("**Unknown command=%c\n", cmd);
			break;
		}
		if (p->Debug > 0) {
			printf("END cmd=%d\n", cmd);
		}
		sleep(1);
	}
	return 0;
}
