 /*
 | RTPMEM.C - Runtime Platform Memory System Services
 |
 |   PORTED TO THE WIN32 PLATFORM
 |
 | EBS - RT-Platform 
 |
 |  $Author: vmalaiya $
 |  $Date: 2006/07/17 15:29:02 $
 |  $Name:  $
 |  $Revision: 1.3 $
 |
 | Copyright EBS Inc. , 2006
 | All rights reserved.
 | This code may not be redistributed in source or linkable object form
 | without the consent of its author.
 |
 | Module description:
 |  [tbd]
*/



/************************************************************************
* Headers
************************************************************************/
#include "rtp.h"
#include "rtpmem.h"

#include <stdlib.h>
#include <malloc.h>

/************************************************************************
* Defines
************************************************************************/

/************************************************************************
* Types
************************************************************************/

/************************************************************************
* Data
************************************************************************/

/************************************************************************
* Macros
************************************************************************/

/************************************************************************
* Function Prototypes
************************************************************************/

/************************************************************************
* Function Bodies
************************************************************************/

/*----------------------------------------------------------------------*
                              _rtp_malloc
 *----------------------------------------------------------------------*/
void * _rtp_malloc (unsigned long size)
{
    return (malloc ((size_t) size));
}


/*----------------------------------------------------------------------*
                             _rtp_calloc
 *----------------------------------------------------------------------*/
void * _rtp_calloc (unsigned long num, unsigned long size)
{
    return (calloc ((size_t) num, (size_t) size));
}


/*----------------------------------------------------------------------*
                             _rtp_realloc
 *----------------------------------------------------------------------*/
void * _rtp_realloc (void * ptr, unsigned long size)
{
    return (realloc (ptr, (size_t) size));
}


/*----------------------------------------------------------------------*
                              _rtp_free
 *----------------------------------------------------------------------*/
void _rtp_free (void * ptr)
{
    free (ptr);
}


/* ----------------------------------- */
/*             END OF FILE             */
/* ----------------------------------- */
