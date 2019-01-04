#include "stdafx.h"
#include <errno.h>
#include <string.h>
#include <process.h>
#include <WinUSB.h>

#include "usbops.h"
#include "osdepend.h"
#include "tsbuff.h"
#include "tsthread.h"

//# number of compensated read only bytes before the buffer busy memory area
#define TS_DeadZone  (TS_BufSize/2) //1835008

#define ROUNDUP(n,w) (((n) + (w)) & ~(unsigned)(w))

struct TSIO_CONTEXT {
	OVERLAPPED ol;
	int index;
	DWORD bytesRead;
};

struct tsthread_param {
	HANDLE hThread;    //# handle to thread data
	unsigned char volatile  flags;
	/* if 0x01 flagged, issue a new request.
	   if 0x02 flagged, cancel requests and stop thread.
	*/
	const struct usb_endpoint_st*  pUSB;
	char* buffer;    //# data buffer (in heap memory)
	int*  actual_length;    //# actual length of each buffer block
	unsigned buff_unitSize;
	int buff_num;
	int buff_push;
	int buff_pop;
	int total_submit ;
	struct TSIO_CONTEXT ioContext[TS_MaxNumIO];
	HANDLE hTsEvents[TS_MaxNumIO*2-1] ;
	HANDLE hTsAvailable,hTsRead,hTsRestart ;
	CRITICAL_SECTION csTsRead;
};

static void tsthread_purgeURB(const tsthread_ptr ptr)
{
	struct tsthread_param* const ps = ptr;
	int i;

	WinUsb_AbortPipe(ps->pUSB->fd, ps->pUSB->endpoint & 0xFF);

	EnterCriticalSection(&ps->csTsRead);

	if(ps->total_submit>0) {

		for (i = 0;i < TS_MaxNumIO;i++) {
			struct TSIO_CONTEXT* pContext = &ps->ioContext[i];
			if(pContext->index>=0) {
				ResetEvent(pContext->ol.hEvent);
				pContext->index=-1 ;
			}
		}

		ps->total_submit = 0;

	}

	for (i = 0;i < ps->buff_num;i++)
		ps->actual_length[i]=-1 ;

	ps->buff_pop = ps->buff_push ;

	LeaveCriticalSection(&ps->csTsRead);

	WinUsb_FlushPipe(ps->pUSB->fd, ps->pUSB->endpoint & 0xFF);
}

static unsigned int tsthread_bulkURB(struct tsthread_param* const ps)
{
	//# number of the prefetching buffer busy area that shouldn't be submitted
	const int POPDELTA = (TS_DeadZone+ps->buff_unitSize-1)/ps->buff_unitSize;

	int ri=0; //# the circular index based ioContext cursor for reaping
	int si=0; //# the circular index based ioContext cursor for submitting
	int next_wait_index=0 ;
	BOOL bRet ;
	DWORD dRet=0 ;

	//# bulk loop
	while(!(ps->flags&0x02)) {

		struct TSIO_CONTEXT* pContext = &ps->ioContext[ri];
		BOOL isTimeout = FALSE;
		BOOL isSync = FALSE;

		if(WaitForSingleObject(ps->hTsRestart,0)==WAIT_OBJECT_0) {
			tsthread_purgeURB(ps) ;
			ResetEvent(ps->hTsRestart) ;
			continue;
		}

		if (!(ps->flags & 0x01)) {
			WaitForSingleObject(ps->hTsAvailable, TS_PollTimeout) ;
			continue ;
		}

		if (ps->pUSB->endpoint & 0x100) { //# Isochronous
			if(ps->flags & 0x01) {
				tsthread_stop(ps);
				continue ;
			}
		}

		if(ps->total_submit>0) {

			if(pContext->index>=0) {

				DWORD bytesRead= 0;

				//# poll
				if (WaitForSingleObject(ps->hTsEvents[ri], 0) == WAIT_OBJECT_0) {
					isSync = TRUE;
					if(ri==next_wait_index) {
						if (++next_wait_index >= TS_MaxNumIO)
							next_wait_index -= TS_MaxNumIO ;
					}
				}else if (ri == next_wait_index) {
					dRet = WaitForMultipleObjects(ps->total_submit, &ps->hTsEvents[ri] , FALSE, TS_PollTimeout );
					if(WAIT_OBJECT_0 <= dRet&&dRet < WAIT_OBJECT_0+ps->total_submit) {
						if(dRet==WAIT_OBJECT_0) isSync = TRUE ;
						next_wait_index = (dRet - WAIT_OBJECT_0) + ri + 1 ;
						if (next_wait_index >= TS_MaxNumIO)
							next_wait_index -= TS_MaxNumIO ;
					}else if(WAIT_TIMEOUT==dRet)
						isTimeout=TRUE ;
					else {
						dRet = GetLastError();
						warn_info(dRet,"poll failed");
						break;
					}
				}else
					isTimeout=TRUE ;

				//# reap
				bytesRead=pContext->bytesRead ;
				if(bytesRead) {
					bRet = TRUE ; dRet = 0 ;
				}else {
					if(isSync||HasOverlappedIoCompleted(&(pContext->ol))) {
						bRet = WinUsb_GetOverlappedResult( ps->pUSB->fd, &(pContext->ol), &bytesRead, isSync);
						dRet = GetLastError();
					}else {
						bRet = FALSE ;
						dRet = ERROR_IO_INCOMPLETE ;
					}
				}
				if (ps->buff_unitSize < bytesRead) {
					warn_info(bytesRead, "reapURB overflow");
					bytesRead = ps->buff_unitSize;
				}
				if(bRet) {
					if (ps->pUSB->endpoint & 0x100) bytesRead = 0;
					ps->actual_length[pContext->index] = bytesRead;
					if(bytesRead) SetEvent(ps->hTsAvailable) ;
					ps->total_submit--;
					ResetEvent(ps->hTsEvents[ri]);
					pContext->index=-1 ;
					isTimeout = FALSE;
				}else {
					if(ERROR_IO_INCOMPLETE == dRet && !(ps->pUSB->endpoint & 0x100)) {
						//SetEvent(ps->hTsEvents[ri]);
						isTimeout=TRUE ;
					}
					else {
						if( ERROR_OPERATION_ABORTED==dRet||
							ERROR_SEM_TIMEOUT==dRet||
							(ps->pUSB->endpoint & 0x100) )
								bytesRead = 0;
						//# failed
						ps->actual_length[pContext->index] = bytesRead;
						warn_msg(dRet, "reapURB%u failed", ri);
						ps->total_submit--;
						ResetEvent(ps->hTsEvents[ri]);
						pContext->index = -1;
						isTimeout = FALSE;
					}
				}

			}

		}

		if(!ps->total_submit) {
			//# I/O stall
			next_wait_index = si = ri ;
			isTimeout=TRUE ;
		}

		if(ps->total_submit<TS_MaxNumIO) {

			//# submit
			if(ps->flags & 0x01) {
				void *buffer;
				DWORD lnTransfered;
				int num_empties,max_empties;
				int last_state;
				dRet = 0;bRet = FALSE;
				//# calculate the real maximum number of submittable empties
				EnterCriticalSection(&ps->csTsRead) ;
				last_state = ps->actual_length[ps->buff_pop] ;
				if(ps->buff_push==ps->buff_pop)
					max_empties =
						last_state>0||last_state==-2 ? 0 : ps->buff_num ;
				else
					max_empties = ps->buff_push<ps->buff_pop ?
						ps->buff_pop-ps->buff_push :
						ps->buff_num-ps->buff_push + ps->buff_pop ;
				ResetEvent(ps->hTsRead);
				LeaveCriticalSection(&ps->csTsRead) ;
				max_empties -= POPDELTA; //# subtract deadzone
				//# summary amount of empties
				num_empties=TS_MaxNumIO-ps->total_submit;
				if(num_empties>max_empties) {
					num_empties = max_empties ;
					#if 1
					if(num_empties<=0) {
						//# in the dead zone
						if(last_state>0) {
							HANDLE events[3];
							events[0]=ps->hTsRead;
							events[1]=ps->hTsRestart ;
							events[2]=ps->hTsEvents[ri+(isTimeout?0:1)] ;
							//# wait for reading buffer...
							WaitForMultipleObjects(3, events , FALSE, TS_PollTimeout );
						}
					}
					#endif
				}
				//# submit to empties
				while(num_empties-->0) {
					pContext = &ps->ioContext[si];
					if (pContext->index>=0) break ; //# I/O busy
					if (WaitForSingleObject(ps->hTsRestart,0)==WAIT_OBJECT_0) break;
					if (WaitForSingleObject(ps->hTsEvents[ri+(isTimeout?0:1)],0)==WAIT_OBJECT_0) break;
					if (!(ps->flags & 0x01)) break;
					if (ps->pUSB->endpoint & 0x100) { //# Isochronous
						//tsthread_stop(ps);
						break;
					}
					if(ps->actual_length[ps->buff_push]>0||ps->actual_length[ps->buff_push]==-2)
						break ; //# buffer busy
					else {
						buffer = ps->buffer + (ps->buff_push * ps->buff_unitSize) ;
						last_state =  ps->actual_length[ps->buff_push] ;
						ps->actual_length[ps->buff_push] = -2;
					}
					pContext->index = ps->buff_push;
					pContext->bytesRead = 0 ;
					ZeroMemory(&pContext->ol,sizeof(OVERLAPPED));
					pContext->ol.hEvent = ps->hTsEvents[si];
					lnTransfered = 0;
					ResetEvent(pContext->ol.hEvent);
					bRet = WinUsb_ReadPipe(ps->pUSB->fd, ps->pUSB->endpoint & 0xFF,
						buffer, ps->buff_unitSize, &lnTransfered, &(pContext->ol));
					dRet = GetLastError();
					if (FALSE == bRet && ERROR_IO_PENDING != dRet) {
						warn_info(dRet, "submitURB failed");
						ps->actual_length[ps->buff_push] = last_state;
						ResetEvent(ps->hTsEvents[si]);
						pContext->index = -1;
					}else {
						if(ps->buff_push+1>=ps->buff_num)
							ps->buff_push=0;
						else
							ps->buff_push++;
						if(bRet) {
							pContext->bytesRead = lnTransfered ;
							SetEvent(ps->hTsEvents[si]) ;
						}
						ps->total_submit++;
						bRet=TRUE ; dRet = 0;
					}
					if(dRet) break ;
					if(bRet && ++si >= TS_MaxNumIO) si=0;
				}
				//if(dRet) break ;
			}

		}

		if(isTimeout) continue ;
		if(++ri >= TS_MaxNumIO) ri=0 ;

	}

	//# dispose
	tsthread_purgeURB(ps);

	return dRet ;
}

/* TS thread function issues URB requests. */
static unsigned int __stdcall tsthread(void* const param)
{
	struct tsthread_param* const ps = param;
	unsigned int result = 0;

	ps->buff_push = 0;

	result = tsthread_bulkURB(ps);

	_endthreadex( 0 );
	return result ;
}

/* public function */

int tsthread_create( tsthread_ptr* const tptr,
					 const struct usb_endpoint_st* const pusbep
				   )
{
	struct tsthread_param* ps;
	DWORD dwRet, i;

	{ //#
		const unsigned param_size = ROUNDUP( sizeof( struct tsthread_param ), 0xF );
		const unsigned buffer_size = ROUNDUP( TS_BufSize , 0xF );
		const unsigned unitSize = ROUNDUP( pusbep->xfer_size, 0x1FF ) ;
		const unsigned unitNum = TS_BufSize / unitSize;
		const unsigned actlen_size = sizeof( int ) * unitNum;
		char *ptr, *buffer_ptr;
		unsigned totalSize = param_size + actlen_size + buffer_size ;
		ptr = uHeapAlloc( totalSize );
		if ( NULL == ptr ) {
			dwRet = GetLastError();
			warn_msg( dwRet, "failed to allocate TS buffer" );
			return -1;
		}
		buffer_ptr = ptr;
		ptr += buffer_size;
		ps = ( struct tsthread_param* ) ptr;
		ps->buffer = buffer_ptr;
		ptr += param_size;
		ps->actual_length = ( int* ) ptr;
		//ptr += actlen_size;
		ps->buff_unitSize = unitSize;
		ps->buff_num = unitNum;
		if ( actlen_size ) {
			for ( i = 0;i < unitNum;i++ )
				ps->actual_length[ i ] = -1;   //# rest all values to empty
		}
	}
	ps->pUSB = pusbep;
	ps->flags = 0;
	ps->buff_pop = 0;
	ps->total_submit = 0;

	for ( i = 0; i < TS_MaxNumIO; i++ ) {
		ps->ioContext[ i ].index = -1;    //# mark it unused
		ps->hTsEvents[ i ] = CreateEvent( NULL, TRUE, FALSE, NULL );
		ZeroMemory( &ps->ioContext[ i ].ol, sizeof( OVERLAPPED ) );
		ps->ioContext[ i ].ol.hEvent = ps->hTsEvents[ i ];
	}

	//# it arranges for event handles to look like circular buffer
	for ( i = 0; i < TS_MaxNumIO - 1; i++ ) {
		ps->hTsEvents[ i + TS_MaxNumIO ] = ps->hTsEvents[ i ];
	}
	ps->hTsAvailable = CreateEvent( NULL, FALSE, FALSE, NULL );
	ps->hTsRead = CreateEvent( NULL, FALSE, FALSE, NULL );
	ps->hTsRestart = CreateEvent( NULL, TRUE, FALSE, NULL );
	InitializeCriticalSection(&ps->csTsRead) ;

	//# USB endpoint
	WinUsb_ResetPipe( pusbep->fd, pusbep->endpoint & 0xFF );
	i = 0x01;
	WinUsb_SetPipePolicy( pusbep->fd, pusbep->endpoint & 0xFF, RAW_IO, sizeof( UCHAR ), &i );
	WinUsb_SetPipePolicy( pusbep->fd, pusbep->endpoint & 0xFF, AUTO_CLEAR_STALL, sizeof( UCHAR ), &i );

	#ifdef _DEBUG

	dwRet = sizeof( i );
	WinUsb_GetPipePolicy( ps->pUSB->fd, ps->pUSB->endpoint & 0xFF, MAXIMUM_TRANSFER_SIZE, &dwRet, &i );
	dmsg( "MAX_TRANSFER_SIZE=%u", i );
	#endif

	ps->hThread = ( HANDLE ) _beginthreadex( NULL, 0, tsthread, ps, 0, NULL );
	if ( INVALID_HANDLE_VALUE == ps->hThread ) {
		warn_info( errno, "tsthread_create failed" );
		uHeapFree( ps->buffer );
		return -1;
	} else {
		SetThreadPriority( ps->hThread, THREAD_PRIORITY_HIGHEST );
	}
	*tptr = ps;
	return 0;
}

void tsthread_destroy(const tsthread_ptr ptr)
{
	int i;
	struct tsthread_param* const p = ptr;

	tsthread_stop(ptr);
	p->flags |= 0x02;    //# canceled = T
	SetEvent(p->hTsRead);
	SetEvent(p->hTsAvailable);
	if (WaitForSingleObject(p->hThread, 1000) != WAIT_OBJECT_0) {
		warn_msg(GetLastError(), "tsthread_destroy timeout");
		TerminateThread(p->hThread, 0);
	}
	for (i = 0; i < TS_MaxNumIO; i++)
		CloseHandle(p->hTsEvents[i]);
	CloseHandle(p->hTsAvailable);
	CloseHandle(p->hTsRead);
	CloseHandle(p->hTsRestart);
	CloseHandle(p->hThread);
	DeleteCriticalSection(&p->csTsRead);

	uHeapFree(p->buffer);
}

void tsthread_start(const tsthread_ptr ptr)
{
	struct tsthread_param* const p = ptr;
	WinUsb_FlushPipe(p->pUSB->fd, p->pUSB->endpoint & 0xFF);
	p->flags |= 0x01;    //# continue = T
	if (p->pUSB->startstopFunc)
		p->pUSB->startstopFunc(p->pUSB->dev, 1);

	SetEvent(p->hTsAvailable);
}

void tsthread_stop(const tsthread_ptr ptr)
{
	struct tsthread_param* const p = ptr;

	p->flags &= ~0x01U;    //# continue = F

	if(p->pUSB->startstopFunc)
		p->pUSB->startstopFunc(p->pUSB->dev, 0);


	if(!(p->pUSB->endpoint & 0x100) ) { //# Bulk
		WinUsb_AbortPipe(p->pUSB->fd, p->pUSB->endpoint & 0xFF);
	}

	SetEvent(p->hTsRestart);
}

int tsthread_read(const tsthread_ptr tptr, void ** const ptr)
{
	struct tsthread_param* const ps = tptr;
	int i, j;

	if(!ptr) {
		SetEvent(ps->hTsRestart) ;
		return 0 ;
	}

	EnterCriticalSection(&ps->csTsRead) ;
	i = tsthread_readable(tptr);
	if(0 < i) {
		j = ps->buff_pop;
		ps->actual_length[ps->buff_pop] = -1;
		*ptr = ps->buffer + (j * ps->buff_unitSize);
		ps->buff_pop = (ps->buff_num - 1 > j) ? j + 1 : 0;
		SetEvent(ps->hTsRead) ;
	}
	LeaveCriticalSection(&ps->csTsRead) ;

	return i<0 ? 0:i ;
}

int tsthread_readable(const tsthread_ptr tptr)
{
	struct tsthread_param* const ps = tptr;
	int j ;

	if(!(ps->flags&0x01U)) return 0;
	if(WaitForSingleObject(ps->hTsRestart,0)==WAIT_OBJECT_0)
		return 0 ;

	EnterCriticalSection(&ps->csTsRead) ;
	j= ps->buff_pop;
	if(0 > j || ps->buff_num <= j) {  //# bug check
		warn_info(j,"ts.buff_pop Out of range");
	    j = -1;
	}
	else do {  //# skip empty blocks
		if(0 != ps->actual_length[j] ) break;
		if(ps->buff_num -1 > j) {
			j++;
		}else{
			j = 0;
		}
	} while(j != ps->buff_pop);
	ps->buff_pop = j<0 ? 0 : j ;
	LeaveCriticalSection(&ps->csTsRead) ;
	return j<0 ? 0 : ps->actual_length[j];
}

int tsthread_wait(const tsthread_ptr tptr, const int timeout)
{
	struct tsthread_param* const ps = tptr;
	DWORD dRet ;
	if(tsthread_readable(tptr)>0) return 1 ; //# already available
	dRet = WaitForSingleObject( ps->hTsAvailable , timeout );
	if(WAIT_OBJECT_0 == dRet)  return 1;
	else if(WAIT_TIMEOUT == dRet)  return 0;

	warn_info(dRet,"poll failed");
	return -1;
}


/*EOF*/