//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// TJS2 Script Managing
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "MainFormUnit.h"
#include "WindowIntf.h"
#include "ScriptMgnImpl.h"
#include "MsgIntf.h"
//#include "PadFormUnit.h"
#include "tjsScriptBlock.h"
#include "EventIntf.h"
#include "SysInitImpl.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "StorageImpl.h"
#include "tjsDebug.h"

#include "Application.h"
//---------------------------------------------------------------------------

/*
	Object Hash Map (implemented in tjsDebug) is a simple object memory leak
	detector.
	Object Hash Map rely on TVP logging facility for logging unfreed objects.
	But TVP logging facility ends before some of TJS2 objects had been freed.

	To solve this problem, TJS2 uses two method to track objects;
		on-memory hash map and interprocess communication (IPC).

	On-memory hash map is a simple method, tracking object's creation and
	destruction on one hash map.

	Interprocess communication throws all object creation/destruction log
	to a process, which is specially created as a child-process for processing
	received log. Interprocess communication is implemented using low-level
	API, works very end of the parent process.

	TVP switches these two methods at framework finalization.
	On-memory hash map is used during most of the time.
	At the end of the framework, the framework switches to interprocess
	communication method. This continues rest of Object Hash Map operation until
	the process ends.

	Process of receiving object hash map log is implemented as the same executable
	of Kirikiri (There is a command line option for running this facility).
*/

//---------------------------------------------------------------------------
// tTVPPipeStream to do IPC (used for Object Hash Map)
//---------------------------------------------------------------------------
class tTVPPipeStream : public tTJSBinaryStream
{
private:
	HANDLE Handle;

public:
	tTVPPipeStream(HANDLE handle)
	{
		Handle = handle;
	}

	~tTVPPipeStream()
	{
		CloseHandle(Handle);
	}

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence)
	{
		return 0; // pipes does not support seeking
	}

	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size)
	{
		DWORD ret = 0;
		ReadFile(Handle, buffer, read_size, &ret, NULL);
		return ret;
	}

	tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size)
	{
		DWORD ret = 0;
		WriteFile(Handle, buffer, write_size, &ret, NULL);
		FlushFileBuffers(Handle);
		return ret;
	}

	void TJS_INTF_METHOD SetEndOfStorage()
	{
		return;
	}

	tjs_uint64 TJS_INTF_METHOD GetSize()
	{
		return 0;
	}
};
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// Object Hash Map (memory leak detector) related
//---------------------------------------------------------------------------
static char TVPObjectHashMapLogStream[sizeof(tTVPPipeStream)];
//---------------------------------------------------------------------------
void TVPStartObjectHashMap()
{
	// addref ObjectHashMap if the program is being debugged.
	if(TJSEnableDebugMode)
		TJSAddRefObjectHashMap();
}
//---------------------------------------------------------------------------
void TVPStartObjectHashMapLog(void)
{
	if(TJSObjectHashMapEnabled())
	{
		// begin logging

		// create anonymous pipe to communicate with child kirikiri process
		HANDLE read, write;
		SECURITY_ATTRIBUTES sa;
		ZeroMemory(&sa, sizeof(sa));
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		CreatePipe(&read, &write, &sa, 0);

		// redirect stdin to output the object hash map log
		HANDLE org_stdin = GetStdHandle(STD_INPUT_HANDLE);
		HANDLE childdupwrite;
		SetStdHandle(STD_INPUT_HANDLE, read);
		DuplicateHandle(GetCurrentProcess(), write, GetCurrentProcess(),
			&childdupwrite, 0, FALSE, DUPLICATE_SAME_ACCESS);
		CloseHandle(write);
		write = childdupwrite;

		// create child kirikiri process
		STARTUPINFO si;
		PROCESS_INFORMATION pi;
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_SHOWNORMAL;
		
		TCHAR szFull[_MAX_PATH];
		::GetModuleFileName(NULL, szFull, sizeof(szFull) / sizeof(TCHAR));
		tstring exepath(szFull);
		BOOL ret =
			::CreateProcess(
				NULL,
				const_cast<LPTSTR>((exepath + _T(" -@processohmlog")).c_str()),
				NULL,
				NULL,
				TRUE,
				0,
				NULL,
				NULL,
				&si,
				&pi);

		if(ret)
		{
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}

		// close unneeded handle
		CloseHandle(read);

		// restore original stdin handle
		SetStdHandle(STD_INPUT_HANDLE, org_stdin);

		// create tTJSBinaryStream object in STATIC AREA
		::new (TVPObjectHashMapLogStream) tTVPPipeStream(write);

		// set object hash map log
		TJSObjectHashMapSetLog((tTVPLocalFileStream*)TVPObjectHashMapLogStream);

		// write all objects to log
		TJSWriteAllUnfreedObjectsToLog();

		// end object mapping
		TJSReleaseObjectHashMap();
	}
}
//---------------------------------------------------------------------------
static tTVPAtExit TVPReportUnfreedObjectsAtExit
	(TVP_ATEXIT_PRI_CLEANUP - 1, TVPStartObjectHashMapLog);
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
bool TVPCheckProcessLog()
{
	// process object hash map log
	int argc = Application->ArgC;
	char** argv = Application->ArgV;

	tjs_int i;
	for(i=1; i<argc; i++)
	{
		if(!strcmp(argv[i], "-@processohmlog")) // this does not refer TVPGetCommandLine
		{
			// create object hash map
			TJSAddRefObjectHashMap();

			// create pipe object
			tTVPPipeStream pipe(GetStdHandle(STD_INPUT_HANDLE));

			// set object hash map log
			TJSObjectHashMapSetLog(&pipe);

			// read from stdin
            TJSReplayObjectHashMapLog();

			// output report if object had been leaked
			if(TJSObjectHashAnyUnfreed())
			{
				TVPOnError();
				TJSReportAllUnfreedObjects(TVPGetTJS2ConsoleOutputGateway());
			}

			// release object hash map
			TJSReleaseObjectHashMap();

			return true; // processed
		}
	}

	return false;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPBeforeProcessUnhandledException
//---------------------------------------------------------------------------
void TVPBeforeProcessUnhandledException()
{
	TVPDumpHWException();
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPShowScriptException
//---------------------------------------------------------------------------
/*
	These functions display the error location, reason, etc.
	And disable the script event dispatching to avoid massive occurrence of
	errors.
*/
//---------------------------------------------------------------------------
void TVPShowScriptException(eTJS &e)
{
	TVPSetSystemEventDisabledState(true);
	TVPOnError();

	if(!TVPSystemUninitCalled)
	{
		if(TVPMainForm) TVPMainForm->SetVisible( true );
		ttstr errstr = (ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
		TVPAddLog(ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
		//MessageDlg(errstr.AsStdString(), mtError, TMsgDlgButtons() << mbOK, 0);
		MessageDlg(errstr.AsStdString(), mtError, MB_OK, 0);
	}
//	throw EAbort("Script Error Abortion");
}
//---------------------------------------------------------------------------
void TVPShowScriptException(eTJSScriptError &e)
{
	TVPSetSystemEventDisabledState(true);
	TVPOnError();

	if(!TVPSystemUninitCalled)
	{

		if(TVPMainForm) TVPMainForm->SetVisible( true );
#pragma message( __LOC__ "TODO Padクラス無効化" )
#if 0 // Not use pad class
		TTVPPadForm *pad = new TTVPPadForm(Application);
		pad->FreeOnTerminate = true;
		pad->ExecButtonEnabled = false;
		pad->SetLines(e.GetBlockNoAddRef()->GetScript());
		pad->GoToLine(1+e.GetBlockNoAddRef()->SrcPosToLine(e.GetPosition() )
			- e.GetBlockNoAddRef()->GetLineOffset());
		pad->ReadOnly = true;
		pad->StatusText = e.GetMessage();
		pad->Caption = ttstr(TVPExceptionCDPName).AsStdString();
		pad->Visible = true;
#endif
		ttstr errstr = (ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
		TVPAddLog(ttstr(TVPScriptExceptionRaised) + TJS_W("\n") + e.GetMessage());
		if(e.GetTrace().GetLen() != 0)
			TVPAddLog(ttstr(TJS_W("trace : ")) + e.GetTrace());
		//Application->MessageBox( errstr.AsStdString().c_str(), Application->GetTitle().c_str(), MB_OK|MB_ICONSTOP );
		::MessageBox( NULL, errstr.AsStdString().c_str(), Application->GetTitle().c_str(), MB_OK|MB_ICONSTOP );
		//	throw EAbort("Script Error Abortion");
	}
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// TVPInitializeStartupScript
//---------------------------------------------------------------------------
void TVPInitializeStartupScript()
{
	TVPStartObjectHashMap();

	TVPExecuteStartupScript();
	if(TVPTerminateOnNoWindowStartup && TVPGetWindowCount() == 0 && (TVPMainForm && !TVPMainForm->GetVisible()))
	{
		// no window is created and main window is invisible
		Application->Terminate();
	}
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// TVPCreateNativeClass_Scripts
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Scripts()
{
	tTJSNC_Scripts *cls = new tTJSNC_Scripts();

	// setup some platform-specific members

//----------------------------------------------------------------------

// currently none

//----------------------------------------------------------------------
	return cls;
}
//---------------------------------------------------------------------------

