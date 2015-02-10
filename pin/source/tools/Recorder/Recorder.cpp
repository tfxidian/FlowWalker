#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include "pin.H"
#include "SystemCall.h"
#define TRACEMEM
//#define TRACEREG
//#define  LOGTIME

//#ifdef TRACEREG
#include <set>
//#endif

#define BLOCK_SZ 0x20000000
#define TAILBLANK_SZ 1024

KNOB<string> KnobOutputPath (KNOB_MODE_WRITEONCE, "pintool", "op", "", "specify directory path for output");
KNOB<string> KnobFunctionFile (KNOB_MODE_WRITEONCE, "pintool", "ff", "", "specify function list file name, full path included");
KNOB<string> KnobTraceFile (KNOB_MODE_WRITEONCE, "pintool", "tf", "", "specify file name for trace");
KNOB<BOOL>   KnobPrintMem(KNOB_MODE_WRITEONCE, "pintool", "m", "1", "print memory addr");
KNOB<UINT>   KnobMaxLogSize(KNOB_MODE_WRITEONCE, "pintool", "logsz", "1024", "maximum logfile size");
KNOB<UINT>   KnobInstTid(KNOB_MODE_APPEND, "pintool", "tid", "", "thread id of the threads to be instrumented");

static ofstream fOutput1;
static ifstream ffunction;
WINDOWS::LPBYTE proc;
WINDOWS::LPBYTE bbllist;
WINDOWS::DWORD offset = 0;
unsigned __int64 offset_p = 0;
unsigned __int64 offset_list = 0;
WINDOWS::HANDLE hFile;
WINDOWS::HANDLE hFileMapping;
WINDOWS::HANDLE hFile_list;
WINDOWS::HANDLE hFileMapping_list;
UINT32 SegNum = 1;

UINT32 IndexBBL = 0;
PIN_LOCK lock;
THREADID CurTid = 0xffffffff;
bool isRenameFunc = false;
string TagetImageName;

#ifdef TRACEREG
set<THREADID> ExistedThreads;
#endif

//UINT32 User32ID = 0;
unsigned __int64 CountBBL = 0;
#ifdef LOGTIME
static WINDOWS::SYSTEMTIME StartTime, CurTime;
ofstream fTime;
#endif

set<UINT> InstTid;
bool InstSpec=false;

set<ADDRINT> EntryAddr;
//结构：函数信息
struct Function
{
	string Name;
	ADDRINT Start;
	ADDRINT End;
	bool isChunk;

	Function()
	{
		Name.clear();
		Start = 0;
		End = 0;
	}
	Function(string name, ADDRINT startaddr, ADDRINT endaddr, bool chunk):Name(name),Start(startaddr),End(endaddr),isChunk(chunk)
	{
	}
};

//IDA函数导出列表
vector<Function> funclist;

//结构:线程信息
struct ThreadData
{
	UINT32 BBLindex;			//当前所属基本块
	ADDRINT StackBaseTable;		//栈基础地址（STB）
	vector<ADDRINT> MemAddr;	//内存地址
#ifdef TRACEREG
	vector<ADDRINT> RegMod;		//寄存器地址
#endif
	/*构造函数*/
	ThreadData():BBLindex(0),StackBaseTable(0){}
	/*构造函数,并输出目标线程的当前寄存器值*/
	ThreadData(THREADID tid, UINT32 bbl, CONTEXT* ctxt):BBLindex(bbl)
	{
#ifdef TRACEREG
		PIN_GetLock(&lock, tid+1);
		string ctxtstr("N ");
		ctxtstr+=decstr(tid);
		ctxtstr+=" EAX:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EAX)).substr(2));
		ctxtstr+=" EBX:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EBX)).substr(2));
		ctxtstr+=" ECX:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_ECX)).substr(2));
		ctxtstr+=" EDX:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EDX)).substr(2));
		ctxtstr+=" ESP:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_ESP)).substr(2));
		ctxtstr+=" EBP:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EBP)).substr(2));
		ctxtstr+=" ESI:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_ESI)).substr(2));
		ctxtstr+=" EDI:";
		ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EDI)).substr(2));
		ctxtstr+="\n";
		UINT32 len=ctxtstr.length();
		memcpy(proc+offset,ctxtstr.data(),len);
		offset+=len;
		PIN_ReleaseLock(&lock);
#endif
	}
	/*清空结构函数，同时输出记录*/
	void FlushRecord(THREADID tid)
	{
		if (!BBLindex)
			return;

		/*准备输出内存跟踪记录*/
#ifdef TRACEMEM
		string temp("B");
		temp+=hexstr(BBLindex).substr(2);
		UINT MemNum=MemAddr.size();
//		for (vector<ADDRINT>::iterator i=MemAddr.begin(); i!=MemAddr.end(); i++)
		for (UINT ti=0; ti!=MemNum; ++ti)
		{
			temp+=" ";
			ADDRINT tMem=MemAddr[ti];
			//对特殊地址(值)进行转义
			if (tMem>0xfffffffb)
			{
				//-栈相对地址
				if (tMem==0xfffffffe)
					temp+="-";
				//+栈相对地址
				else if (tMem==0xfffffffd)
					temp+="+";
				//栈基础地址
				else if (tMem==0xffffffff)
					temp+="S";
				//跳过当前输出
				else
				{
					++ti;
					temp+=hexstr(MemAddr[ti]).substr(2);
					continue;
				}
				++ti;
			}
			//地址输出
			temp+=hexstr(MemAddr[ti]).substr(2);
		}
		temp+="\n";
		PIN_GetLock(&lock,tid+1);
		++CountBBL;

		//将记录时间写入日志
#ifdef LOGTIME
		if (CountBBL%100000==0)
		{
			WINDOWS::GetLocalTime(&CurTime);
			UINT32 mseconds = (CurTime.wMinute-StartTime.wMinute)*60000+(CurTime.wSecond-StartTime.wSecond)*1000+(CurTime.wMilliseconds-StartTime.wMilliseconds);
			fTime << mseconds << ' ';
		}
#endif
		//将线程ID和内存记录写入日志
		if (tid != CurTid)
		{
			CurTid = tid;
			_itoa(tid,(char*)proc+offset,10);
			offset+=(tid<10?2:(tid<100?3:4));
			*(proc+offset-1)=' ';
		}
		UINT32 len=temp.length();
		memcpy(proc+offset,temp.data(),len);
		offset+=len;
		MemAddr.clear();
#endif

		/*准备输出寄存器跟踪记录*/
#ifdef TRACEREG
		string temp1("B");
		temp1+=hexstr(BBLindex).substr(2);
		for (vector<ADDRINT>::iterator i=RegMod.begin(); i!=RegMod.end(); i++)
		{
			temp1+=" ";
			temp1+=hexstr(*i).substr(2);
		}
		temp1+="\n";

		PIN_GetLock(&lock,tid+1);

		++CountBBL;

		//将记录时间写入日志
#ifdef LOGTIME
		if (CountBBL%100000==0)
		{
			WINDOWS::GetLocalTime(&CurTime);
			UINT32 mseconds = (CurTime.wMinute-StartTime.wMinute)*60000+(CurTime.wSecond-StartTime.wSecond)*1000+(CurTime.wMilliseconds-StartTime.wMilliseconds);
			fTime << mseconds << ' ';
		}
#endif
		//将线程ID和寄存器记录写入日志
		if (tid != CurTid)
		{
			CurTid = tid;
			_itoa(tid,(char*)proc+offset,10);
			offset+=(tid<10?2:(tid<100?3:4));
			*(proc+offset-1)=' ';
		}
		UINT32 len1=temp1.length();
		memcpy(proc+offset,temp1.data(),len1);
		offset+=len1;
		RegMod.clear();
#endif

		/*若当前内存映射文件大小越界，则重新创建内存映射文件*/
		if (offset > BLOCK_SZ - TAILBLANK_SZ)
		{
			WINDOWS::UnmapViewOfFile(proc);

			offset_p += BLOCK_SZ;
			offset = 0;
			if (offset_p%(((unsigned __int64)(KnobMaxLogSize.Value()))*0x100000)!=0)
				proc = (WINDOWS::LPBYTE)WINDOWS::MapViewOfFile(hFileMapping, FILE_MAP_WRITE,(WINDOWS::DWORD)(offset_p>>32),(WINDOWS::DWORD)(offset_p&0xFFFFFFFF),BLOCK_SZ);
			else
			{
				WINDOWS::CloseHandle(hFileMapping);
				++SegNum;
				unsigned __int64 FileMappingSz = ((unsigned __int64)(KnobMaxLogSize.Value()))*0x100000*SegNum;
				hFileMapping = WINDOWS::CreateFileMapping(hFile,NULL,PAGE_READWRITE,(UINT32)(FileMappingSz>>32),(UINT32)(FileMappingSz&0xffffffff),NULL);
				if (hFileMapping == NULL)
				{
					fOutput1 << "FileMapping Failed!";
					fOutput1.close();
					exit(0);
				}
				proc = (WINDOWS::LPBYTE)WINDOWS::MapViewOfFile(hFileMapping, FILE_MAP_WRITE,(WINDOWS::DWORD)(offset_p>>32),(WINDOWS::DWORD)(offset_p&0xFFFFFFFF),BLOCK_SZ);
				if (proc == NULL)
				{
					fOutput1 << "FileMapping Failed 2!";
					fOutput1.close();
					exit(0);
				}
			}
		}

		PIN_ReleaseLock (&lock);
		BBLindex = 0;
	}
}*CurThread;

map<UINT,ThreadData> threads;


static UINT Usage (void)
{
	cerr << "This PINTOOL is used to record the running process." << endl;
	cerr << KNOB_BASE::StringKnobSummary() << endl;
	return -1;
}

/*退出函数*/
VOID Fini (INT32 code, VOID *v)
{
	fOutput1.close();
	
	string temp = decstr(CountBBL)+"\n";
	int tlen = temp.length();
	memcpy(bbllist+offset_list, temp.c_str(), tlen);
	offset_list+=tlen;

	//关闭内存映射文件bbllist
	WINDOWS::UnmapViewOfFile(bbllist);
	WINDOWS::CloseHandle(hFileMapping_list);
	WINDOWS::LONG HighOffset = (WINDOWS::LONG)(offset_list>>32);
	WINDOWS::SetFilePointer(hFile_list,(UINT32)(offset_list&0xFFFFFFFF),&HighOffset,FILE_BEGIN);
	WINDOWS::SetEndOfFile(hFile_list);
	WINDOWS::CloseHandle(hFile_list);

	//关闭内存映射文件proc
	WINDOWS::UnmapViewOfFile(proc);
	WINDOWS::CloseHandle(hFileMapping);
	offset_p += offset;
	WINDOWS::LONG HighOffset2 = (WINDOWS::LONG)(offset_p>>32);
	WINDOWS::SetFilePointer(hFile,(UINT32)(offset_p&0xFFFFFFFF),&HighOffset2,FILE_BEGIN);
	WINDOWS::SetEndOfFile(hFile);
	WINDOWS::CloseHandle(hFile);
}

//VOID OnDetach (VOID *v)
//{
//	Fini(0,0);
//}

/*Image插装函数*/
VOID ImageLoad (IMG img, VOID *v)
{
	//输出至image日志
	string imgname(IMG_Name(img));
	fOutput1 << decstr(IMG_Id(img), 3) << " " << hex << IMG_LowAddress (img) << " " << IMG_HighAddress (img)
	         << " " << imgname << endl;

	/*插装负责进程结束消息的函数，辅助结束目标进程*/
	/*
	RTN QuitMsgRtn = RTN_FindByName(img, "PostQuitMessage");
	if (RTN_Valid(QuitMsgRtn))
	{
		RTN_Open(QuitMsgRtn);
//		RTN_InsertCall(QuitMsgRtn, IPOINT_BEFORE, (AFUNPTR)PIN_Detach, IARG_END);
		RTN_InsertCall(QuitMsgRtn, IPOINT_BEFORE, (AFUNPTR)PIN_ExitApplication, IARG_UINT32, 0, IARG_END);
		RTN_Close(QuitMsgRtn);
	}
	*/

	/*保存所有函数入口地址*/
	if(isRenameFunc && TagetImageName == imgname)
	{
		//根据日志提供的函数信息，由ImageBase重置函数地址，并更新函数入口地址
		ADDRINT ImageBase = IMG_LowAddress(img);
		for (vector<Function>::iterator fi=funclist.begin(); fi!=funclist.end(); ++fi)
		{
			fi->Start += ImageBase;
			fi->End += ImageBase;
			//若不是函数Chunk，则默认为函数入口。否则不记录为函数入口
			if(fi->isChunk == false)
				EntryAddr.insert(fi->Start);
		}
	}
	else
	{
		//根据PIN提供的函数信息，更新函数入口地址
		for( SEC sec= IMG_SecHead(img); SEC_Valid(sec); sec = SEC_Next(sec) )
		{
			for( RTN rtn= SEC_RtnHead(sec); RTN_Valid(rtn); rtn = RTN_Next(rtn) )
			{
				EntryAddr.insert(RTN_Address(rtn));
			}
		}
	}
}

/*判断tid线程是否需要插装*/
ADDRINT PIN_FAST_ANALYSIS_CALL JudgeThread(THREADID tid)
{
	return (InstTid.count(tid));
}

#ifdef TRACEMEM
/*进入基本块函数（内存跟踪），重置原线程信息结构*/
VOID PIN_FAST_ANALYSIS_CALL EnterBBL (UINT32 index, THREADID threadid)
{
	ThreadData* CurThread = &threads[threadid];
	//输出并清空线程信息
	CurThread->FlushRecord(threadid);
	//存储当前BBL序号
	CurThread->BBLindex = index;
}
#endif
#ifdef TRACEREG
/*进入基本块函数（寄存器跟踪），重置原线程信息结构*/
VOID PIN_FAST_ANALYSIS_CALL EnterBBL (UINT32 index, THREADID threadid, CONTEXT* ctxt)
{
	if (ExistedThreads.count(threadid)==0)
	{
		ThreadData NewThread(threadid,index,ctxt);
		threads[threadid]=NewThread;
		ExistedThreads.insert(threadid);
	}
	else
	{
		ThreadData* CurThread = &threads[threadid];
		CurThread->FlushRecord(threadid);
		CurThread->BBLindex = index;
	}
}
#endif

#ifdef TRACEMEM
/*将内存地址存入线程信息结构*/
VOID PIN_FAST_ANALYSIS_CALL PrintMemAddr (ADDRINT addr, THREADID threadid)
{
	threads[threadid].MemAddr.push_back(addr);
}
/*将栈地址存入线程信息结构*/
VOID PIN_FAST_ANALYSIS_CALL PrintStackAddr (ADDRINT addr, THREADID threadid)
{
	ThreadData* CurThread = &threads[threadid];
	ADDRINT CurThreadStackBase = CurThread->StackBaseTable;
	if (CurThreadStackBase>=addr)
	{
		//相对栈基础地址（STB）-偏移，形式化标记0xfffffffe
		CurThread->MemAddr.push_back(0xfffffffe);
		addr=CurThreadStackBase-addr;
	}
	else
	{
		//相对栈基础地址（STB）+偏移，形式化标记0xfffffffd
		CurThread->MemAddr.push_back(0xfffffffd);
		addr-=CurThreadStackBase;
	}
	CurThread->MemAddr.push_back(addr);
}
/*存储栈基础地址StackBaseTable（STB）*/
VOID PIN_FAST_ANALYSIS_CALL PrintStackAddrHead (ADDRINT addr, THREADID threadid)
{
	ThreadData* CurThread = &threads[threadid];
	CurThread->StackBaseTable = addr;

	//STB的形式化标记0xffffffff
	CurThread->MemAddr.push_back(0xffffffff);
	CurThread->MemAddr.push_back(addr);
}

/*将内存地址存入线程信息结构（针对Lea指令）*/
VOID PIN_FAST_ANALYSIS_CALL PrintLeaAddr (ADDRINT addr, THREADID threadid)
{
	threads[threadid].MemAddr.push_back(0xfffffffc);
	threads[threadid].MemAddr.push_back(addr);
}

/*CMOV指令开关标记*/
VOID PIN_FAST_ANALYSIS_CALL CMOVswitch(bool taken, THREADID threadid)
{
	threads[threadid].MemAddr.push_back(taken?1:0);
}
#endif
#ifdef TRACEREG
VOID PIN_FAST_ANALYSIS_CALL PushRegModValue (ADDRINT regv, THREADID threadid)
{
	threads[threadid].RegMod.push_back(regv);
}
#endif

/*Trace插装函数*/
VOID Trace (TRACE trace, VOID *v)
{
	if (!bActive)
		return;

	//获得当前Trace隶属的函数的函数名
	RTN rtn = TRACE_Rtn (trace);
	string RtnName = "";
	ADDRINT TraceEntry=TRACE_Address(trace);

	if (RTN_Valid (rtn) )
	{
		//若读入了函数日志,则首先从函数日志中寻找函数,否则从PIN中获得函数名
		if(isRenameFunc)
		{
			bool isLocaled = false;
			for(vector<Function>::iterator fi=funclist.begin(); fi!=funclist.end(); ++fi)
			{
				if(TraceEntry >= fi->Start && TraceEntry < fi->End)
				{
					RtnName = fi->Name;
					isLocaled = true;
					break;
				}
				if(!isLocaled)
				{
					RtnName = PIN_UndecorateSymbolName(RTN_Name (rtn),UNDECORATION_NAME_ONLY);
				}
			}
		}
		else
			RtnName = PIN_UndecorateSymbolName(RTN_Name (rtn),UNDECORATION_NAME_ONLY);
	}
	//有的代码存在于dll未导出的内部函数中，无法获得函数名
	else
		RtnName = "InvalidRTN";

	//判断Trace地址是否是函数入口地址
	bool IsEntry=false;
	if (EntryAddr.count(TraceEntry))
	{
		IsEntry=true;
		/*note：是否需要erase*/
		EntryAddr.erase(TraceEntry);
	}

	UINT32 ImgID = IMG_Id (IMG_FindByAddress (TRACE_Address (trace) ) );

	// 遍历Trace中的所有BBL
	for (BBL bbl = TRACE_BblHead (trace); BBL_Valid (bbl); bbl = BBL_Next (bbl) )
	{
		//给定当前顺序分配的一个索引号，并记录到日志文件中
		string temp = "BBL "+hexstr(++IndexBBL).substr(2)+"\n"+decstr(ImgID)+" "+RtnName;
		if (IsEntry)
		{
			temp+="_entry";
			IsEntry=false;
		}
		temp+="\n";
		int tlen = temp.length();
		memcpy(bbllist+offset_list, temp.data(), tlen);
		offset_list+=tlen;

		// 对该BBL插装，用于在后续执行该BBL序列时打印调用记录
#ifdef TRACEMEM
		if (InstSpec)
		{
			BBL_InsertIfCall (bbl, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
			BBL_InsertThenCall (bbl, IPOINT_BEFORE, (AFUNPTR) EnterBBL, IARG_FAST_ANALYSIS_CALL,
				IARG_UINT32, IndexBBL, 
				IARG_THREAD_ID, 
				IARG_END);
		}
		else
			BBL_InsertCall (bbl, IPOINT_BEFORE, (AFUNPTR) EnterBBL, IARG_FAST_ANALYSIS_CALL,
				IARG_UINT32, IndexBBL, IARG_THREAD_ID, IARG_END);
		
#endif
#ifdef TRACEREG
		BBL_InsertCall (bbl, IPOINT_BEFORE, (AFUNPTR) EnterBBL, IARG_FAST_ANALYSIS_CALL,
						IARG_UINT32, IndexBBL, /*IARG_UINT32, ImgID,*/ IARG_THREAD_ID,
						IARG_CONST_CONTEXT, IARG_END);
#endif

#ifdef TRACEMEM
		int memcount = 0;
		BOOL ss = TRUE;
#endif
		//遍历BBL中所有指令
		for (INS ins = BBL_InsHead (bbl); INS_Valid (ins); ins = INS_Next (ins) )
		{
			//记录指令地址与反汇编代码
			string temp = hexstr(INS_Address (ins),8)+" "+INS_Disassemble (ins)+"\n";
			int tlen = temp.length();
			memcpy(bbllist+offset_list, temp.data(), tlen);
			offset_list+=tlen;

#ifdef TRACEMEM
			if (KnobPrintMem.Value())
			{
				OPCODE opcode=INS_Opcode(ins);

				//CMOV指令插装
				if (opcode>=XED_ICLASS_CMOVB && opcode<=XED_ICLASS_CMOVZ)
				{
					//存储CMOV开关标志
					if (InstSpec)
					{
						//判断是否属于需要插装的线程，若属于，则进行插装
						INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
						INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) CMOVswitch, IARG_FAST_ANALYSIS_CALL, IARG_EXECUTING, IARG_THREAD_ID, IARG_END);
					}
					else
						//直接插装
						INS_InsertCall (ins, IPOINT_BEFORE, (AFUNPTR) CMOVswitch, IARG_FAST_ANALYSIS_CALL, IARG_EXECUTING, IARG_THREAD_ID, IARG_END);
				}

				//栈读取插装
				if (INS_IsStackRead(ins))
				{
					string temp = "r"+decstr(++memcount)+"\n";
					int tlen = memcount<10?3:(memcount<100?4:5);
					memcpy(bbllist+offset_list, temp.data(), tlen);
					offset_list+=tlen;

					//若是首次栈操作，存储栈基础地址STB
					if (ss)
					{
						ss = FALSE;
						if (InstSpec)
						{
							INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
							INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddrHead, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYREAD_EA, IARG_THREAD_ID, IARG_END);
						}
						else
							INS_InsertPredicatedCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddrHead, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYREAD_EA, IARG_THREAD_ID, IARG_END);						
					}
					//否则，存储栈相对地址
					else
					{
						if (InstSpec)
						{
							INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
							INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddr, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYREAD_EA, IARG_THREAD_ID, IARG_END);
						}
						else
							INS_InsertPredicatedCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddr, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYREAD_EA, IARG_THREAD_ID, IARG_END);
					}
				}

				//内存读取插装
				else if (INS_IsMemoryRead (ins) )
				{
					string temp = "R"+decstr(++memcount)+"\n";
					int tlen = memcount<10?3:(memcount<100?4:5);
					memcpy(bbllist+offset_list, temp.data(), tlen);
					offset_list+=tlen;

					//存储内存地址
					if (InstSpec)
					{
						INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
						INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintMemAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_MEMORYREAD_EA, IARG_THREAD_ID,/* IARG_BOOL, TRUE,*/ IARG_END);
					}
					else
						INS_InsertPredicatedCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintMemAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_MEMORYREAD_EA, IARG_THREAD_ID,/* IARG_BOOL, TRUE,*/ IARG_END);
				}

				//内存读取插装：指令包括2次内存读取操作
				if (INS_HasMemoryRead2 (ins) )
				{
					string temp = "R"+decstr(++memcount)+"\n";
					int tlen = memcount<10?3:(memcount<100?4:5);
					memcpy(bbllist+offset_list, temp.data(), tlen);
					offset_list+=tlen;

					//存储第二次读取操作的内存地址
					if (InstSpec)
					{
						INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
						INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintMemAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_MEMORYREAD2_EA, IARG_THREAD_ID,/* IARG_BOOL, TRUE,*/ IARG_END);
					}
					else
						INS_InsertPredicatedCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintMemAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_MEMORYREAD2_EA, IARG_THREAD_ID,/* IARG_BOOL, TRUE,*/ IARG_END);
				}

				//栈写入插装
				if (INS_IsStackWrite(ins))
				{
					string temp = "w"+decstr(++memcount)+"\n";
					int tlen = memcount<10?3:(memcount<100?4:5);
					memcpy(bbllist+offset_list, temp.data(), tlen);
					offset_list+=tlen;

					//若是首次栈操作，存储栈基础地址STB
					if (ss)
					{
						ss = FALSE;
						if (InstSpec)
						{
							INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
							INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddrHead, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYWRITE_EA, IARG_THREAD_ID, IARG_END);
						}
						else
							INS_InsertPredicatedCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddrHead, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYWRITE_EA, IARG_THREAD_ID, IARG_END);
					}
					//否则，存储栈相对地址
					else
					{
						if (InstSpec)
						{
							INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
							INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddr, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYWRITE_EA, IARG_THREAD_ID, IARG_END);
						}
						else
							INS_InsertPredicatedCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintStackAddr, IARG_FAST_ANALYSIS_CALL,
								IARG_MEMORYWRITE_EA, IARG_THREAD_ID, IARG_END);
					}
				}
				//内存写入插装
				else if (INS_IsMemoryWrite (ins) )
				{
					string temp = "W"+decstr(++memcount)+"\n";
					int tlen = memcount<10?3:(memcount<100?4:5);
					memcpy(bbllist+offset_list, temp.data(), tlen);
					offset_list+=tlen;

					//存储内存地址
					if (InstSpec)
					{
						INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
						INS_InsertThenCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintMemAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_MEMORYWRITE_EA, IARG_THREAD_ID,/* IARG_BOOL, FALSE,*/ IARG_END);
					}
					else
						INS_InsertPredicatedCall (ins, IPOINT_BEFORE, (AFUNPTR) PrintMemAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_MEMORYWRITE_EA, IARG_THREAD_ID,/* IARG_BOOL, FALSE,*/ IARG_END);
				}

				//Lea指令插装
				if (INS_IsLea(ins))
				{
					string temp = "R"+decstr(++memcount)+"\n";
					int tlen = memcount<10?3:(memcount<100?4:5);
					memcpy(bbllist+offset_list, temp.data(), tlen);
					offset_list+=tlen;

					//传入写寄存器的值作为内存地址
					if (InstSpec)
					{
						INS_InsertIfCall (ins, IPOINT_BEFORE, (AFUNPTR) JudgeThread, IARG_FAST_ANALYSIS_CALL, IARG_THREAD_ID, IARG_END);
						INS_InsertThenCall (ins, IPOINT_AFTER, (AFUNPTR) PrintLeaAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_REG_VALUE, INS_RegW(ins, 0), IARG_THREAD_ID, IARG_END);
					}
					else
						INS_InsertPredicatedCall (ins, IPOINT_AFTER, (AFUNPTR) PrintLeaAddr, IARG_FAST_ANALYSIS_CALL,
							IARG_REG_VALUE, INS_RegW(ins, 0), IARG_THREAD_ID, IARG_END);
				}
			}
#endif
#ifdef TRACEREG
			OPCODE opcode = INS_Opcode(ins);
			BOOL HasRegW = FALSE;
			//对MOV指令进行插装
			if (opcode==XED_ICLASS_MOV || opcode==XED_ICLASS_MOVSX || opcode==XED_ICLASS_MOVZX)
			{
				if (INS_MemoryOperandCount(ins)==1 && INS_MemoryOperandIsRead(ins,0))
				{
					HasRegW = TRUE;
					INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR) PushRegModValue, IARG_FAST_ANALYSIS_CALL,
						IARG_REG_VALUE, INS_RegW(ins, 0), IARG_THREAD_ID, IARG_END);
				}
			}
			//对运算指令指令进行插装
			else if (opcode==XED_ICLASS_ADD || opcode==XED_ICLASS_ADC || opcode==XED_ICLASS_SUB || opcode==XED_ICLASS_SBB
				|| opcode==XED_ICLASS_AND || opcode==XED_ICLASS_OR || opcode==XED_ICLASS_XOR)
			{
				if (INS_MemoryOperandCount(ins)==1 && INS_MemoryOperandIsRead(ins,0))
				{
					HasRegW = TRUE;
					INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR) PushRegModValue, IARG_FAST_ANALYSIS_CALL,
						IARG_REG_VALUE, INS_RegW(ins, 0), IARG_THREAD_ID, IARG_END);
				}
			}
			//对POP指令进行插装
			else if (opcode==XED_ICLASS_POP && !INS_IsMemoryWrite(ins))
			{
				HasRegW = TRUE;
				INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR) PushRegModValue, IARG_FAST_ANALYSIS_CALL,
					IARG_REG_VALUE, INS_RegW(ins, 0), IARG_THREAD_ID, IARG_END);
			}
			//若存在寄存器写操作
			if (HasRegW)
			{
				string temp = REG_StringShort(INS_RegW(ins,0))+"\n";
				int tlen = temp.length();
				memcpy(bbllist+offset_list, temp.data(), tlen);
				offset_list+=tlen;
			}
			/*UINT32 RegWcnt=INS_MaxNumWRegs(ins);
			if (INS_HasFallThrough(ins))
				for (UINT32 i=0; i!=RegWcnt; ++i)
				{
					if (REG_is_gr8(INS_RegW(ins, i)) || REG_is_gr16(INS_RegW(ins, i)) || REG_is_gr32(INS_RegW(ins, i)))
					{
						string temp = REG_StringShort(INS_RegW(ins, i))+"\n";
						int tlen = temp.length();
						memcpy(bbllist+offset_list, temp.data(), tlen);
						offset_list+=tlen;
						INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR) PushRegModValue, IARG_FAST_ANALYSIS_CALL,
							IARG_REG_VALUE, INS_RegW(ins, i), IARG_THREAD_ID, IARG_END);
					}					
				}*/
#endif
		}

		memcpy(bbllist+offset_list, "0\n", 2);
		offset_list+=2;
	}
}

/*IDA函数导出列表读取函数*/
bool LoadFuncionList(string FunctionListPath)
{
	bool open_flag = false;
	ifstream ffunction;
	ADDRINT funcStart,funcEnd;
	ADDRINT ImageBase;
	string funcName;
	string lastFuncName;
	bool isChunk = false;

	//打开文件
	ffunction.open(FunctionListPath);
	if(!ffunction.fail())
		open_flag = true;

	ffunction >> ImageBase;
	ffunction >> TagetImageName;
	while(!ffunction.eof())
	{
		//读取函数起始地址和终止地址，并减去偏移量，判断是否是函数Chunk
		ffunction >> funcStart;
		ffunction >> funcEnd;
		ffunction >> funcName;
		funcStart -= ImageBase;
		funcEnd -= ImageBase;
		isChunk = (funcName == lastFuncName)?true:false;

		if(isChunk == true)
			cout << funcName << "'s Chunk" << endl;

		funclist.push_back(Function(funcName,funcStart,funcEnd,isChunk));
		lastFuncName = funcName;
	}
	return open_flag;
}

#if 0
// This routine is executed every time a thread is created.
VOID ThreadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
	PIN_GetLock(&lock, threadid+1);

	ADDRINT esp=PIN_GetContextReg(ctxt,REG_ESP)|0xffff;
	*(proc+(offset++))='N';
	*(proc+(offset++))=' ';
	_itoa(threadid,(char*)proc+offset,10);
	offset+=(threadid<10?2:(threadid<100?3:4));
	*(proc+offset-1)=' ';
	_itoa(esp,(char*)proc+offset,16);
	offset+=(esp>=0x10000000?9:(esp>=0x1000000?8:(esp>=0x100000?7:(esp>=0x10000?6:5))));
	*(proc+offset-1)='\n';

	string ctxtstr("N ");
	ctxtstr+=decstr(threadid);
	ctxtstr+=" EAX:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EAX)).substr(2));
	ctxtstr+=" EBX:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EBX)).substr(2));
	ctxtstr+=" ECX:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_ECX)).substr(2));
	ctxtstr+=" EDX:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EDX)).substr(2));
	ctxtstr+=" ESP:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_ESP)).substr(2));
	ctxtstr+=" EBP:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EBP)).substr(2));
	ctxtstr+=" ESI:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_ESI)).substr(2));
	ctxtstr+=" EDI:";
	ctxtstr+=(hexstr(PIN_GetContextReg(ctxt,REG_EDI)).substr(2));
	ctxtstr+="\n";
	UINT32 len=ctxtstr.length();
	memcpy(proc+offset,ctxtstr.data(),len);
	offset+=len;

	PIN_ReleaseLock(&lock);
}

// This routine is executed every time a thread is destroyed.
VOID ThreadFini(THREADID threadid, const CONTEXT *ctxt, INT32 code, VOID *v)
{
//	fOutput1 << "Thread " << threadid << "Finished." <<endl;
}
#endif

BOOL FollowChild(CHILD_PROCESS childProcess, VOID * userData)
{
	fprintf(stdout, "before child:%u\n", CHILD_PROCESS_GetId(childProcess));
	return TRUE;
}

int main (int argc, char *argv[])
{
	// 初始化pin
	if ( PIN_Init (argc, argv) )
	{
		return Usage();
	}
	PIN_InitLock (&lock);

	//判断是否需要针对特定线程插装
	UINT InstTidNo=KnobInstTid.NumberOfValues();
	if (InstTidNo)
	{
		InstSpec=true;
		for (UINT ti=0; ti!=InstTidNo; ++ti)
			InstTid.insert(KnobInstTid.Value(ti));
	}
	
	int pid=PIN_GetPid();
	string OutputPath(KnobOutputPath.Value());
	if (OutputPath.back()!='\\')
		OutputPath.push_back('\\');

	//创建image日志
	fOutput1.open (OutputPath + decstr(pid) + "_image_list.fw");
	if (fOutput1 == NULL)
	{
		printf ("File cannot be openned!\n");
		return -1;
	}
#ifdef LOGTIME
	//创建时间日志
	fTime.open(OutputPath + decstr(pid) + "Time.fw");
	if (fTime == NULL)
	{
		printf ("File cannot be openned!\n");
		return -1;
	}
#endif

	//创建内存文件映射--进程日志（内存和寄存器日志）
	hFile = WINDOWS::CreateFile((OutputPath + decstr(pid) + "_process.fw").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned __int64 FileMappingSz = ((unsigned __int64)(KnobMaxLogSize.Value()))*0x100000;
	hFileMapping = WINDOWS::CreateFileMapping(hFile,NULL,PAGE_READWRITE,(UINT32)(FileMappingSz>>32),(UINT32)(FileMappingSz&0xffffffff),NULL);
	if (hFileMapping == NULL)
	{
		fOutput1 << "FileMapping Failed!";
		fOutput1.close();
		exit(0);
	}
	proc = (WINDOWS::LPBYTE)WINDOWS::MapViewOfFile(hFileMapping, FILE_MAP_WRITE,0,0,BLOCK_SZ);
	if (proc == NULL)
	{
		fOutput1 << "FileMapping Failed 2!";
		fOutput1.close();
		exit(0);
	}

	//创建内存文件映射--基本块日志
	hFile_list = WINDOWS::CreateFile((OutputPath + decstr(pid) + "_bbl_list.fw").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	hFileMapping_list = WINDOWS::CreateFileMapping(hFile_list,NULL,PAGE_READWRITE,0,0x10000000,NULL);
	if (hFileMapping_list == NULL)
	{
		fOutput1 << "FileMapping Failed 3!";
		fOutput1.close();
		exit(0);
	}
	bbllist = (WINDOWS::LPBYTE)WINDOWS::MapViewOfFile(hFileMapping_list, FILE_MAP_WRITE,0,0,0x10000000);
	if (bbllist == NULL)
	{
		fOutput1 << "FileMapping Failed 4!";
		fOutput1.close();
		exit(0);
	}

	//获得文件打开相关系统调用的变量
	String2WString (KnobTraceFile.Value(), wstrTraceFileName);
	WINDOWS::OSVERSIONINFO osinfo;
	osinfo.dwOSVersionInfoSize = sizeof (WINDOWS::OSVERSIONINFO);
	if (!GetVersionEx (&osinfo) )
		return 1;
	if (osinfo.dwMajorVersion == 6 && osinfo.dwMinorVersion == 1)
	{
		num_create = 0x42;
		num_open = 0xb3;
		num_read = 0x111;
		num_close = 0x32;
		num_move = 0x149;
		num_createsection = 0x54;
		num_mapviewofsection = 0xa8;
		num_unmapviewofsection = 0x181;
		num_allocmem = 0x13;
		num_freemem = 0x83;
		num_create_64 = 0x52;
		num_open_64 = 0x30;
		num_read_64 = 0x3;
		num_close_64 = 0xc;
		num_move_64 = 0x24;
		num_createsection_64 = 0x47;
		num_mapviewofsection_64 = 0x25;
		num_unmapviewofsection_64 = 0x27;
		num_allocmem_64 = 0x15;
		num_freemem_64 = 0x1b;
	}
	else if (osinfo.dwMajorVersion == 6 && osinfo.dwMinorVersion == 0)
	{
		num_create = 0x3c;
		num_open = 0xba;
		num_read = 0x102;
		num_close = 0x30;
		num_move = 0x12d;
		num_createsection = 0x4b;
		num_mapviewofsection = 0xb1;
		num_unmapviewofsection = 0x15c;
		num_allocmem = 0x12;
		num_freemem = 0x93;
		num_create_64 = 0x52;
		num_open_64 = 0x30;
		num_read_64 = 0x3;
		num_close_64 = 0xc;
		num_move_64 = 0x24;
		num_createsection_64 = 0x47;
		num_mapviewofsection_64 = 0x25;
		num_unmapviewofsection_64 = 0x27;
		num_allocmem_64 = 0x15;
		num_freemem_64 = 0x1b;
	}
	else if (osinfo.dwMajorVersion == 5 && osinfo.dwMinorVersion == 1)
	{
		num_create = 0x25;
		num_open = 0x74;
		num_read = 0xb7;
		num_close = 0x19;
		num_move = 0xe0;
		num_createsection = 0x32;
		num_mapviewofsection = 0x6c;
		num_unmapviewofsection = 0x10b;
		num_allocmem = 0x11;
		num_freemem = 0x53;
		num_create_64 = 0x52;
		num_open_64 = 0x30;
		num_read_64 = 0x3;
		num_close_64 = 0xc;
		num_move_64 = 0x24;
		num_createsection_64 = 0x47;
		num_mapviewofsection_64 = 0x25;
		num_unmapviewofsection_64 = 0x27;
		num_allocmem_64 = 0x15;
		num_freemem_64 = 0x1b;
	}
	else
	{
		return 1;
	}

	string FunctionListPath(KnobFunctionFile.Value());
	if(!FunctionListPath.empty())
	{
		isRenameFunc = LoadFuncionList(FunctionListPath);
	}

	CountBBL = 0;

	//开始进行插装
	// Register Analysis routines to be called when a thread begins/ends
//	PIN_AddThreadStartFunction(ThreadStart, 0);
//	PIN_AddThreadFiniFunction(ThreadFini, 0);

//	INS_AddInstrumentFunction (Instrumentation, 0);
	PIN_AddSyscallEntryFunction (SyscallEntry, 0);
	PIN_AddSyscallExitFunction (SyscallExit, 0);
	PIN_InitSymbols();
	IMG_AddInstrumentFunction (ImageLoad, 0);
	TRACE_AddInstrumentFunction (Trace, 0);
	PIN_AddFiniFunction (Fini, 0);

//	PIN_AddDetachFunction(OnDetach, 0);
	//PIN_AddFollowChildProcessFunction(FollowChild, 0);

	CODECACHE_ChangeCacheLimit(512*1024*1024);

	// Start the program, never returns
	PIN_StartProgram();

	return 0;
}
