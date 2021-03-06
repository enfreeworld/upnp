//
// SSDP.C - UPnP SSDP-related functions
//
// EBS - UPnP
//
// Copyright EBS Inc. , 2005
// All rights reserved.
// This code may not be redistributed in source or linkable object form
// without the consent of its author.
//

/*****************************************************************************/
// Header files
/*****************************************************************************/

#include "ssdpsrv.h"
#include "rtp.h"
#include "rtpnet.h"
#include "rtpstr.h"
#include "rtptime.h"
#include "rtpstdup.h"
#include "rtpscnv.h"
#include "rtpmem.h"
#include "httpp.h"
#include "rtpprint.h"
#include "rtprand.h"
#include "rtplog.h"
#define USE_INTEL_SDK_ORDER 1

/*****************************************************************************/
// Macros
/*****************************************************************************/
// #define SSDP_ProcessError(X)
#define	SSDP_BUFFER_SIZE     8192
/* Maximum value that can be returned by the rand function. */
#define SSDP_RAND_MAX 0x7fff

/*****************************************************************************/
// Types
/*****************************************************************************/
struct s_SSDPReadSession
{
    SSDPServerContext *ctx;
    RTP_NET_ADDR       clientAddr;
};

/*****************************************************************************/
// Data
/*****************************************************************************/
const unsigned short mcastPort = 1900;
const unsigned char v4mcastAddr[] = {239, 255, 255, 250};
const char IPV6_LINK_LOCAL_ADDRESS[] = "FF02::C";
/*****************************************************************************/
// Function Prototypes
/*****************************************************************************/
SSDP_INT32   SSDP_CheckPendingResponses(SSDPServerContext *ctx, SSDP_UINT32 currentTimeMsec);
int          SSDP_ParseRequest(SSDPServerContext *ctx, SSDPServerRequest *request);
void         SSDP_FreeRequest(SSDPServerContext *ctx, SSDPServerRequest *request);
int          SSDP_McastRead(void *ctxPtr, HTTP_UINT8 *buffer, HTTP_INT32 min,
                            HTTP_INT32 max);
#ifndef SSDP_ProcessError
void         SSDP_ProcessError (SSDP_CHAR *errMsg);
#endif
SSDP_UINT32  SSDP_RandMax(SSDP_UINT32);
SSDP_INT32   _SSDP_ProcessOneRequest (SSDPServerContext *ctx);

int          _SSDP_ReadMSearchHeader (void *request,
                                      HTTPSession *ptr,
									  HTTPHeaderType type,
									  const HTTP_CHAR *name,
                                      const HTTP_CHAR *value);

int          _SSDP_ReadNotifyHeader  (void *request,
                                      HTTPSession *ptr,
									  HTTPHeaderType type,
									  const HTTP_CHAR *name,
                                      const HTTP_CHAR *value);

SSDP_INT32   SSDP_SendResponse (SSDPServerContext *ctx, SSDPPendingResponse *response);

/*****************************************************************************/
// Function Definitions
/*****************************************************************************/

/*----------------------------------------------------------------------------*
                          SSDP_ServerInit
 *----------------------------------------------------------------------------*/
/** @memo SSDP server initialization routine.
    @doc This routine starts up SSDP services by creating a UDP socket, getting the ssdp
    multicast membership for the socket and initlializing ssdp context.

    @return error code
 */
SSDP_INT32  SSDP_ServerInit(
             SSDPServerContext *ctx,    /** pointer to an uninitialized SSDP context structure */
             SSDP_UINT8 *ipAddr,        /** IP address of the host to bind the UDP socket to, if a NULL is
                                            supplied, the UDP socket is bound to the local IP address */
             SSDP_INT16 ipType,         /** ip version 4 or ipversion 6 */
             const SSDP_CHAR *serverId, /** String holding platform name */
             SSDPCallback cb,           /** SSDP Callback routine */
             void *cookie               /** cookie(runtime) to be stored in ssdp context to be passed
                                            later to ssdp callback */)
{
    RTP_HANDLE   ssdpSock;
    SSDP_INT32   ttl;
    unsigned char localAddr[RTP_NET_NI_MAXHOST];
    unsigned char mcastAddr[RTP_NET_NI_MAXHOST];
  
   /*
    * Open a datagram sockets used
    */
    if (rtp_net_socket_datagram_dual (&ssdpSock, ipType) < 0)
    {
        SSDP_ProcessError("Datagram UDP socket");
        return(-1);
    }

    /* Allow the socket to be bound to an address that is already in use. */
    if (rtp_net_setreuseaddr (ssdpSock, 1) < 0)
    {
        SSDP_ProcessError("Socket reuse option");
        return(-1);
    }

    if(ipType == RTP_NET_TYPE_IPV6)
	{
		rtp_strcpy((char *) mcastAddr, IPV6_LINK_LOCAL_ADDRESS);
		ttl = 255;
	}
	else if (ipType == RTP_NET_TYPE_IPV4)
	{
		rtp_strcpy((char *) mcastAddr,(const char *) v4mcastAddr);
		ttl = 4;
	}

	/* If ipaddress to bind to is not supplied, bind it to the local
       ip address */
	if (!ipAddr)
	{
		/* obtain ipaddress on the current interface, localAddr will contains
		ipaddress of the interface */
		if (rtp_net_getifaceaddr (localAddr, mcastAddr, mcastPort, ipType) < 0)
		{
			SSDP_ProcessError("error obtaining address");
		}
	}
	else
	{
		rtp_strcpy((char *) localAddr, (const char *) ipAddr);
	}

    if (rtp_net_bind_dual (ssdpSock, RTP_NET_DATAGRAM, localAddr, mcastPort,
        ipType) < 0)
    {
        SSDP_ProcessError("Binding");
        return(-1);
    }


    /* join the socket to multicast group */
    if (rtp_net_setmembership(ssdpSock, mcastAddr, ipType, 1) < 0)
    {
        SSDP_ProcessError("Multicast Socket Option");
        return(-1);
    }

    /* Sets the TTL value associated with multicast traffic on the socket */
    if (rtp_net_setmcastttl(ssdpSock, ttl) < 0)
    {
        SSDP_ProcessError("Multicast TTL Option");
        return(-1);
    }

    ctx->announceSocket = ssdpSock;
    ctx->userCallback = cb;
    ctx->userCookie = cookie;
    ctx->ipType = ipType;
    ctx->serverName = rtp_strdup(serverId);
    DLLIST_INIT(&ctx->pendingResponses);

    return(0);
}

/*----------------------------------------------------------------------------*
                        SSDP_ServerAddToSelectList
 *----------------------------------------------------------------------------*/
/**
 */
SSDP_INT32 SSDP_ServerAddToSelectList (
		SSDPServerContext* ctx,
		RTP_FD_SET* readList,
		RTP_FD_SET* writeList,
		RTP_FD_SET* errList
	)
{
	SSDP_INT32 timeout = RTP_TIMEOUT_INFINITE;

    /* Check the pending response list to see if any response is scheduled to be send */
	if (ctx->pendingResponses.next != &ctx->pendingResponses)
	{
		SSDP_UINT32 currentTimeMsec = rtp_get_system_msec();
		SSDPPendingResponse  *pendingResponse = (SSDPPendingResponse *) ctx->pendingResponses.next;
		timeout = (SSDP_INT32) (pendingResponse->scheduledTimeMsec - currentTimeMsec);
	}

	rtp_fd_set(readList, ctx->announceSocket);

	return (timeout);
}

/*----------------------------------------------------------------------------*
                        SSDP_ServerProcessState
 *----------------------------------------------------------------------------*/
/**
 */
SSDP_BOOL SSDP_ServerProcessState (
		SSDPServerContext* ctx,
		RTP_FD_SET* readList,
		RTP_FD_SET* writeList,
		RTP_FD_SET* errList
	)
{
	SSDP_UINT32 currentTimeMsec = rtp_get_system_msec();

	SSDP_CheckPendingResponses(ctx, currentTimeMsec);

	if (rtp_fd_isset(readList, ctx->announceSocket))
	{
		_SSDP_ProcessOneRequest(ctx);
		rtp_fd_clr(readList, ctx->announceSocket);
	}

	return (0);
}

/*----------------------------------------------------------------------------*
                        SSDP_ServerDestroy
 *----------------------------------------------------------------------------*/
/**
 */

void SSDP_ServerDestroy (
		SSDPServerContext *ctx /** pointer to SSDP context */
	)
{
	rtp_strfree(ctx->serverName);
	rtp_net_closesocket(ctx->announceSocket);
}

/*----------------------------------------------------------------------------*
                       SSDP_SendAlive
 *----------------------------------------------------------------------------*/
/** @memo Send a SSDP notification for the device or service.
    @doc This routine sends ssdp alive notifications on the mulicast address for devices and servces

    @return error code
 */

SSDP_INT32 SSDP_SendAlive (
            SSDPServerContext *ctx,         /** pointer to SSDP context */
            const SSDP_CHAR *notifyType,    /** the notification type (NT) string */
            const SSDP_CHAR *usn,           /** pointer to string containing USN header */
            const SSDP_CHAR *location,      /** pointer to string containing Location header */
            SSDP_UINT32 *timeout            /** pointer to max-age header value */)
{
    SSDP_CHAR     ipAddr[RTP_NET_NI_MAXHOST];
    SSDP_CHAR     *msgBfr;
    SSDP_SOCKET    notifySock = ctx->announceSocket;


	if(ctx->ipType == RTP_NET_TYPE_IPV6)
	{
		rtp_sprintf(ipAddr, "[FF02::C]:1900");
	}
	else if (ctx->ipType == RTP_NET_TYPE_IPV4)
	{
		rtp_sprintf(ipAddr, "239.255.255.250:1900");
	}

    msgBfr = (SSDP_CHAR *) rtp_malloc(sizeof(SSDP_CHAR) * SSDP_BUFFER_SIZE);

	if (!msgBfr)
	{
		return (-1);
	}

    /* Send Advertisements */

    /* Fix (03/18/2007) - Correct formatting changed format fields from:
                      "HOST:%s\r\n"
                      "Cache-Control:max-age=%d\r\n"
                      "Location:%s\r\n"
                      "NT:%s\r\n"
                      "NTS:%s\r\n"
                      "Server:%s\r\n"
                      "USN:%s\r\n"

	*/

#if (USE_INTEL_SDK_ORDER)
	rtp_sprintf(msgBfr,"NOTIFY * HTTP/1.1\r\n"
		"LOCATION: %s\r\n"
		"HOST: %s\r\n"
		"SERVER: %s\r\n"
		"NTS: ssdp:alive\r\n"
		"USN: %s\r\n"
		"CACHE-CONTROL: max-age=%d\r\n"
		"NT: %s\r\n"
		"\r\n",
		location,
		ipAddr,
		ctx->serverName,
		usn,
		*timeout,
		notifyType);
#else
	rtp_sprintf(msgBfr,"NOTIFY * HTTP/1.1\r\n"
		"HOST: %s\r\n"
		"CACHE-CONTROL: max-age=%d\r\n"
		"LOCATION: %s\r\n"
		"NT: %s\r\n"
		"NTS: ssdp:alive\r\n"
		"SERVER: %s\r\n"
		"USN: %s\r\n"
		"\r\n",
		ipAddr,
		*timeout,
		location,
		notifyType,
		ctx->serverName,
		usn);
#endif
	if(ctx->ipType == RTP_NET_TYPE_IPV6)
	{
		rtp_strcpy(ipAddr, IPV6_LINK_LOCAL_ADDRESS);
	}
	else if (ctx->ipType == RTP_NET_TYPE_IPV4)
	{
		rtp_strcpy(ipAddr, (const char *)v4mcastAddr);
	}
	RTP_LOG_WRITE("SSDP_SendAlive SEND", msgBfr, rtp_strlen(msgBfr))
    /* Send the message to the multicast channel */
    if (rtp_net_sendto(notifySock, (const unsigned char*)msgBfr, rtp_strlen(msgBfr), (unsigned char *)ipAddr, mcastPort, ctx->ipType) < 0)
    {
        SSDP_ProcessError("SendTo");
        rtp_free(msgBfr);
        return(-1);
    }

    /* Free up the allocated memory */
    rtp_free(msgBfr);
    return(0);
}

/*----------------------------------------------------------------------------*
                       SSDP_SendByebye
 *----------------------------------------------------------------------------*/
/** @memo Send a SSDP notification for the device or service.
    @doc This routine sends ssdp bye-bye notifications on the mulicast
         address for devices and servces

    @return error code
 */

SSDP_INT32 SSDP_SendByebye (
            SSDPServerContext *ctx,         /** pointer to SSDP context */
            const SSDP_CHAR *notifyType,    /** the notification type (NT) string */
            const SSDP_CHAR *usn           /** pointer to string containing USN header */)
{
    SSDP_CHAR     ipAddr[RTP_NET_NI_MAXHOST];
    SSDP_CHAR     *msgBfr;
    SSDP_SOCKET    notifySock = ctx->announceSocket;

	if(ctx->ipType == RTP_NET_TYPE_IPV6)
	{
		rtp_sprintf(ipAddr, "[FF02::C]:1900");
	}
	else if (ctx->ipType == RTP_NET_TYPE_IPV4)
	{
		rtp_sprintf(ipAddr, "239.255.255.250:1900");
	}

    msgBfr = (SSDP_CHAR *) rtp_malloc(sizeof(SSDP_CHAR) * SSDP_BUFFER_SIZE);

	if (!msgBfr)
	{
		return (-1);
	}

    /* Send Advertisements */

    /* Fix (03/18/2007) - Correct formatting changed format fields from:
                      "HOST:%s\r\n"
                      "Cache-Control:max-age=%d\r\n"
                      "Location:%s\r\n"
                      "NT:%s\r\n"
                      "NTS:%s\r\n"
                      "Server:%s\r\n"
                      "USN:%s\r\n"

	*/
#if (USE_INTEL_SDK_ORDER)
    rtp_sprintf(msgBfr,"NOTIFY * HTTP/1.1\r\n"
                      "HOST: %s\r\n"
                      "NTS: ssdp:byebye\r\n"
                      "USN: %s\r\n"
                      "NT: %s\r\n"
                      "\r\n",
					  ipAddr,
                      usn,
                      notifyType);
#else
    rtp_sprintf(msgBfr,"NOTIFY * HTTP/1.1\r\n"
                      "HOST: %s\r\n"
                      "NT: %s\r\n"
                      "NTS: ssdp:byebye\r\n"
                      "USN: %s\r\n"
                      "\r\n",
					  ipAddr,
                      notifyType,
                      usn);
#endif
	if(ctx->ipType == RTP_NET_TYPE_IPV6)
	{
		rtp_strcpy(ipAddr, IPV6_LINK_LOCAL_ADDRESS);
	}
	else if (ctx->ipType == RTP_NET_TYPE_IPV4)
	{
		rtp_strcpy(ipAddr, (const char*)v4mcastAddr);
	}
	RTP_LOG_WRITE("SSDP_SendByeBye SEND", msgBfr, rtp_strlen(msgBfr))
    /* Send the message to the multicast channel */
    if (rtp_net_sendto(notifySock, (unsigned char*)msgBfr, rtp_strlen(msgBfr),(unsigned char *) ipAddr, mcastPort, ctx->ipType) < 0)
    {
        SSDP_ProcessError("SendTo");
        rtp_free(msgBfr);
        return(-1);
    }

    /* Free up the allocated memory */
    rtp_free(msgBfr);
    return(0);
}


/*----------------------------------------------------------------------------*
                        SSDP_SendResponse
 *----------------------------------------------------------------------------*/
/** Deliver a responce to SSDP discovery request.

    @return error code
 */

SSDP_INT32 SSDP_SendResponse (
            SSDPServerContext *ctx,       /** pointer to SSDP context */
            SSDPPendingResponse *response /** buffer holding response information */)
{
    SSDP_CHAR     *msgBuffer;
    SSDP_SOCKET    responseSock = ctx->announceSocket;

    msgBuffer = (SSDP_CHAR *) rtp_malloc(sizeof(SSDP_CHAR) * SSDP_BUFFER_SIZE);

    if (msgBuffer)
    {
	    /* Send response */
	    rtp_sprintf(msgBuffer,"HTTP/1.1 200 OK\r\n"
	                         "LOCATION: %s\r\n"
	                         "EXT: \r\n"
	                         "USN: %s\r\n"
	                         "SERVER: %s\r\n"
	                         "CACHE-CONTROL: max-age=%d\r\n"
	                         "ST: %s\r\n"
	                         "\r\n",
	                         response->targetLocation,
	                         response->targetUSN,
	                         ctx->serverName,
	                         response->targetTimeoutSec,
	                         response->searchTarget);

	    RTP_LOG_WRITE("SSDPSendResponse SEND", msgBuffer, rtp_strlen(msgBuffer))

	    /* Send response to the requesting client */
	    if (rtp_net_sendto(responseSock, (unsigned char*)msgBuffer,
	                       rtp_strlen(msgBuffer),
	                       response->clientAddr.ipAddr,
	                       response->clientAddr.port,
	                       response->clientAddr.type) < 0)
	    {
	        SSDP_ProcessError("Response");
	        rtp_free(msgBuffer);
	        return(-1);
	    }

	    /* Free up the allocated memory */
	    rtp_free(msgBuffer);
	}
	else
	{
		return (-1);
	}

    return(0);
}

/*----------------------------------------------------------------------------*
                        _SSDP_ProcessOneRequest
 *----------------------------------------------------------------------------*/
/** @memo Process an incoming SSDP discovery request.
    @doc Processes a SSDP discovery request available during a period given by
    timeoutMsec milli seconds.

    @return error code
 */

SSDP_INT32 _SSDP_ProcessOneRequest (
            SSDPServerContext *ctx /** pointer to SSDP context */
	)
{
	SSDPServerRequest request;

	if (rtp_net_read_select (ctx->announceSocket, 0) < 0)
	{
		SSDP_ProcessError ("Read Select");
		return (-1);
	}

	rtp_memset(&request, 0, sizeof(SSDPServerRequest));

	/* parse the http request */
	if (SSDP_ParseRequest(ctx, &request) < 0)
	{
		SSDP_ProcessError ("Read Select");
		return (-1);
	}

	/* Create a response on a valid request*/
	if (request.type > SSDP_SERVER_REQUEST_UNKNOWN &&
	    request.type < SSDP_NUM_SERVER_REQUEST_TYPES &&
		ctx->userCallback)
	{
		ctx->userCallback(ctx, &request, ctx->userCookie);
	}

	SSDP_FreeRequest(ctx, &request);

    return (0);
}

/*----------------------------------------------------------------------------*
                        SSDP_ParseRequest
 *----------------------------------------------------------------------------*/
/** @memo Extract SSDP request.
    @doc Retrieves messages from the multicast address, if ssdp request is
    detected a buffer holding request information is populated

    @return error code
 */

int SSDP_ParseRequest (
		SSDPServerContext* ctx,        /** pointer to SSDP context */
		SSDPServerRequest* ssdpRequest /** address of the SSDPServerEvent structure to fill up */
	)
{
    HTTPSession session;
    HTTPRequest request;
    SSDP_UINT8 *msgBuffer;
    SSDP_INT32 len;
    SSDP_INT32 bufferSize = SSDP_BUFFER_SIZE;
    struct s_SSDPReadSession readSession;
	int result = 0;

    readSession.ctx = ctx;
    msgBuffer = (SSDP_UINT8*) rtp_malloc(sizeof(SSDP_UINT8) * SSDP_BUFFER_SIZE);

	if (!msgBuffer)
	{
		return (-1);
	}

    /* create an HTTP session */
	if(HTTP_InitSession (&session, (HTTPStreamReadFn)SSDP_McastRead, 0, (void *)&readSession) < 0 )
    {
        SSDP_ProcessError("Initailizing http session");
        rtp_free(msgBuffer);
        return(-1);
    }

    /* Read the request */
    if((len = HTTP_ReadRequest(&session, &request, msgBuffer, bufferSize)) < 0)
    {
        SSDP_ProcessError("Read Request");
        HTTP_FreeSession(&session);
        rtp_free(msgBuffer);
        return(-1);
    }

    /* if request is M-SEARCH */

    if (rtp_stricmp(request.method, "M-SEARCH") == 0)
    {
		ssdpRequest->type = SSDP_SERVER_REQUEST_M_SEARCH;

		/* Store the Source address in request buffer */
		rtp_memmove(&ssdpRequest->data.search.clientAddr, &readSession.clientAddr, sizeof(RTP_NET_ADDR));

		/* Read Mx and St value in request buffer */
		if (HTTP_ReadHeaders (
				&session,
				_SSDP_ReadMSearchHeader,
				ssdpRequest,
				msgBuffer + len,
				bufferSize - len
			) < 0)
		{
			result = -1;
			SSDP_ProcessError("Read Header");
		}
    }
	else if (rtp_stricmp(request.method, "NOTIFY") == 0)
	{
		ssdpRequest->type = SSDP_SERVER_REQUEST_NOTIFY;

		/* Read Mx and St value in request buffer */
		if (HTTP_ReadHeaders (
				&session,
				_SSDP_ReadNotifyHeader,
				ssdpRequest,
				msgBuffer + len,
				bufferSize - len
			) < 0)
		{
			result = -1;
			SSDP_ProcessError("Read Header");
		}
	}
	else
	{
		ssdpRequest->type = SSDP_SERVER_REQUEST_UNKNOWN;
	}

	HTTP_FreeSession(&session);
    rtp_free(msgBuffer);

    return (result);
}

/*----------------------------------------------------------------------------*
                               SSDP_FreeRequest
 *----------------------------------------------------------------------------*/
/*

    @return nothing
 */
void SSDP_FreeRequest (
		SSDPServerContext *ctx,
		SSDPServerRequest *request
	)
{
	switch (request->type)
	{
		case SSDP_SERVER_REQUEST_M_SEARCH:
			if (request->data.search.target)
			{
				rtp_strfree(request->data.search.target);
			}
			break;

		case SSDP_SERVER_REQUEST_NOTIFY:
			if (request->data.notify.type)
			{
				rtp_strfree(request->data.notify.type);
			}
			if (request->data.notify.subType)
			{
				rtp_strfree(request->data.notify.subType);
			}
			if (request->data.notify.usn)
			{
				rtp_strfree(request->data.notify.usn);
			}
			if (request->data.notify.location)
			{
				rtp_strfree(request->data.notify.location);
			}
			break;
	}
}

/*----------------------------------------------------------------------------*
                               SSDP_McastRead
 *----------------------------------------------------------------------------*/
/** Reads all messages posted to the multicast address

    @return error code
 */

int SSDP_McastRead (
     void *cookie,       /** internal cookie */
     SSDP_UINT8 *buffer, /** pointer to buffer contaning request message */
     SSDP_INT32 min,     /**  */
     SSDP_INT32 max      /** max size to be read */)
{
    SSDP_INT32    retVal;
    struct s_SSDPReadSession *readSession;
    readSession = (struct s_SSDPReadSession *)cookie;

	if ((retVal = rtp_net_recvfrom(readSession->ctx->announceSocket, buffer, max,
	                               readSession->clientAddr.ipAddr,
	                               &readSession->clientAddr.port,
	                               &readSession->clientAddr.type)) < 0)
    {
        if (retVal == -2)
        {
            ;/* Non-Fatal error has occured */
        }
        else
        {
            SSDP_ProcessError("Recv From");
            return(-1);
        }
    }

    RTP_LOG_WRITE("SSDP_McastRead Recv", buffer, retVal)

    return(retVal);
}

/*----------------------------------------------------------------------------*
                           _SSDP_ReadMSearchHeader
 *----------------------------------------------------------------------------*/
/** Extracts MX and St headers from a SSDP request

    @return error code
 */

int _SSDP_ReadMSearchHeader (
		void *request,         /** request buffer to be populated */
		HTTPSession *ptr,      /** current HTTP session */
		HTTPHeaderType type,   /** HTTP header type */
		const HTTP_CHAR *name, /** holds the name of the header */
		const HTTP_CHAR *value /** holds the value of the header*/
	)
{
	SSDPServerRequest *ssdpRequest = (SSDPServerRequest*) request;
	switch (type)
	{
	    case HTTP_HEADER_MX:
            ssdpRequest->data.search.maxReplyTimeoutSec = rtp_atoi(value);
            break;

        case HTTP_HEADER_ST:
            ssdpRequest->data.search.target = rtp_strdup(value);
            break;
    }
    return(0);
}

/*----------------------------------------------------------------------------*
                           _SSDP_ReadNotifyHeader
 *----------------------------------------------------------------------------*/
/** Extracts MX and St headers from a SSDP request

    @return error code
 */

int _SSDP_ReadNotifyHeader (
		void *request,         /** request buffer to be populated */
		HTTPSession *ptr,      /** current HTTP session */
		HTTPHeaderType type,   /** HTTP header type */
		const HTTP_CHAR *name, /** holds the name of the header */
		const HTTP_CHAR *value /** holds the value of the header*/
	)
{
	SSDPServerRequest* ssdpRequest = (SSDPServerRequest*) request;
	switch (type)
	{
		case HTTP_HEADER_LOCATION:
            ssdpRequest->data.notify.location = rtp_strdup(value);
			break;

		case HTTP_HEADER_NT:
            ssdpRequest->data.notify.type = rtp_strdup(value);
			break;

		case HTTP_HEADER_NTS:
            ssdpRequest->data.notify.subType = rtp_strdup(value);
			break;

		case HTTP_HEADER_USN:
            ssdpRequest->data.notify.usn = rtp_strdup(value);
			break;

		case HTTP_HEADER_CACHE_CONTROL:
		{
			const HTTP_CHAR* maxAge = rtp_stristr(value, "max-age=");
			if (maxAge)
			{
				maxAge += 8;
				ssdpRequest->data.notify.timeout = rtp_atoi(maxAge);
			}
			break;
		}
    }
    return(0);
}


/*----------------------------------------------------------------------------*
                            SSDP_QueueResponse
 *----------------------------------------------------------------------------*/
/** Queues a response to the response list based on its scheduled delivery time.
    A random delivery time within targetTimeoutSec duration is calculated.
    This response is positioned in the list according to its scheduled delivery time.

    @return error code
 */

SSDP_INT32 SSDP_QueueSearchResponse(
            SSDPServerContext *ctx,          /** pointer to SSDP context */
            SSDPSearch *search,              /** pointer to the buffer containing
                                                 the request information */
            const SSDP_CHAR *targetLocation, /** pointer to string containing Location header*/
            const SSDP_CHAR *targetURN,      /** pointer to string containing USN header */
            SSDP_UINT32 targetTimeoutSec     /** max-age header value */)
{
    SSDPPendingResponse *responseNode;
    SSDPPendingResponse *nextResponse;
    SSDP_UINT32          mxTimeMsec;
    SSDP_UINT32          scheduledTimeMsec;
    SSDP_UINT32          currentTimeMsec;

    /* get random time between current time and MX time in request */
    mxTimeMsec = search->maxReplyTimeoutSec * 1000;

    /* Schedule time to send the response is a random number between mxTimeMsec and */
    /* current time */
    currentTimeMsec = rtp_get_system_msec();
    scheduledTimeMsec = currentTimeMsec + SSDP_RandMax(mxTimeMsec);

    /* populate the response node */
    if(search->target == 0 || targetLocation == 0 || targetURN == 0)
    {
        return (-1);
    }

	responseNode = (SSDPPendingResponse *) rtp_malloc(sizeof(SSDPPendingResponse));
	if (!responseNode)
	{
		return (-1);
	}

    responseNode->searchTarget      = rtp_strdup (search->target);
    responseNode->targetLocation    = rtp_strdup (targetLocation);
    responseNode->targetUSN         = rtp_strdup (targetURN);
	responseNode->clientAddr        = search->clientAddr;
    responseNode->scheduledTimeMsec = scheduledTimeMsec;
    responseNode->targetTimeoutSec  = targetTimeoutSec;

    /* placing the response node in the list based on the scheduled */
    /* delivery time */
    nextResponse = (SSDPPendingResponse *) ctx->pendingResponses.next;

    while (nextResponse != (SSDPPendingResponse *) &ctx->pendingResponses)
    {
    	if ((SSDP_INT32)(responseNode->scheduledTimeMsec - nextResponse->scheduledTimeMsec) < 0)
    	{
    		break;
    	}

    	nextResponse = (SSDPPendingResponse *) nextResponse->node.next;
    }

    DLLIST_INSERT_BEFORE(&nextResponse->node, &responseNode->node);

    return(0);
}

/*----------------------------------------------------------------------------*
                            SSDP_CheckPendingResponses
 *----------------------------------------------------------------------------*/
/** @memo Delivers responses scheduled for delivery.
    @doc Scan the pending response list and deliver responses for which the
    scheduled time count is less than supplied time count.

    @return error code
 */

SSDP_INT32 SSDP_CheckPendingResponses (
            SSDPServerContext *ctx,     /** pointer to SSDP context */
            SSDP_UINT32 currentTimeMsec /** time against which the scheduled
                                            time is checked */)
{
    SSDPPendingResponse* next;
    SSDPPendingResponse* pendingResponse;

    /* Check the pending response list to see if any response is scheduled to be send */
    pendingResponse = (SSDPPendingResponse*) ctx->pendingResponses.next;

    while ((pendingResponse != (SSDPPendingResponse*) &ctx->pendingResponses) &&
           (SSDP_INT32)(pendingResponse->scheduledTimeMsec - currentTimeMsec) <= 0)
    {
    	next = (SSDPPendingResponse *) pendingResponse->node.next;

        /* Send this response */
        SSDP_SendResponse(ctx, pendingResponse);

        /* delete this node */
		DLLIST_REMOVE(&pendingResponse->node);
		rtp_strfree(pendingResponse->searchTarget);
		rtp_strfree(pendingResponse->targetLocation);
		rtp_strfree(pendingResponse->targetUSN);
		rtp_free(pendingResponse);

        /* Free up this node from the pending response list */
        pendingResponse = next;
    }

    return(0);
}

#ifndef SSDP_ProcessError
/*----------------------------------------------------------------------------*
                        SSDP_ProcessError
 *----------------------------------------------------------------------------*/
/** Process SSDP Errors

    @return None
 */

void SSDP_ProcessError(
      SSDP_CHAR *errMsg /** error message string */)
{
    rtp_printf("%s\n", errMsg);
}
#endif

/*----------------------------------------------------------------------------*
                            SSDP_RandMax
 *----------------------------------------------------------------------------*/
/** generates a random number between 0 and mxLimit

    @return error code
 */

SSDP_UINT32 SSDP_RandMax (
             SSDP_UINT32 mxLimit /** upper limit of the random number */)
{
    SSDP_UINT32 randNum;
    rtp_srand(rtp_get_system_msec());
    randNum = 1+ (SSDP_UINT32)(mxLimit* rtp_rand()/(SSDP_RAND_MAX + 1.0));
    return(randNum);
}
