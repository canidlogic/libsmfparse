/*
 * smfparse.c
 * ==========
 * 
 * Implementation of smfparse.h
 * 
 * See the header for further information.
 */

#include "smfparse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Constants
 * =========
 */

/*
 * State constants for source objects.
 * 
 * The NORMAL state is the regular state.
 * 
 * The ERROR and DOUBLE states are both error states.  The ERROR state
 * is used for most errors.  The DOUBLE state is used when a rewind
 * operation fails on a source object that supports rewind.
 * 
 * The EOF state indicates that the End Of File has been reached.
 */
#define SOURCE_STATE_NORMAL (0)
#define SOURCE_STATE_ERROR  (1)
#define SOURCE_STATE_DOUBLE (2)
#define SOURCE_STATE_EOF    (3)

/*
 * The maximum length of files that can be read with the built-in
 * HANDLE_SOURCE reader.
 * 
 * This is 1 GiB.
 */
#define HANDLE_FILE_MAXLEN INT32_C(1073741824)

/*
 * Type declarations
 * =================
 */

/*
 * SMFSOURCE structure.
 * 
 * Prototype given in header.
 */
struct SMFSOURCE_TAG {
  
  /*
   * The current state of the source object.
   * 
   * This is one of the SOURCE_STATE constants.
   */
  int state;
  
  /*
   * The instance pointer that is passed through from the constructor to
   * each callback function.
   */
  void *pInstance;
  
  /*
   * Read function pointer.
   * 
   * Required, never NULL.
   */
  smfsource_fp_read fRead;
  
  /*
   * Rewind function pointer.
   * 
   * Optional, NULL if rewind is not supported.
   */
  smfsource_fp_rewind fRewind;
  
  /*
   * Close function pointer.
   * 
   * Optional, NULL if no destructor required.
   */
  smfsource_fp_close fClose;
  
  /*
   * Skip function pointer.
   * 
   * Optional, NULL if no special skip function supported.
   */
  smfsource_fp_skip fSkip;
};

/*
 * Instance data for built-in source type that wraps a file handle.
 */
typedef struct {
  
  /*
   * The file handle that is wrapped.
   */
  FILE *fh;
  
  /*
   * The byte offset of the next byte that will be read from the input
   * file.
   * 
   * If flen is not -1, this must not exceed flen.  Otherwise, this must
   * not exceed HANDLE_FILE_MAXLEN.
   */
  int32_t fptr;
  
  /*
   * The cached total length of the file.
   * 
   * Only available if can_seek is non-zero.  Else, set to -1.  May not
   * exceed HANDLE_FILE_MAXLEN.
   */
  int32_t flen;
  
  /*
   * Non-zero if the source object owns the handle and should close it
   * upon release.
   */
  int is_owner;
  
  /*
   * Non-zero if the handle supports random access seeking.
   */
  int can_seek;
  
} HANDLE_SOURCE;

/*
 * Static data
 * ===========
 */

/*
 * The fault handler, or NULL if nothing is registered.
 */
static smf_fp_fault m_fault = NULL;

/*
 * Local functions
 * ===============
 */

/* Prototypes */
static void fault(long lnum);

static int handle_source_read(void *pInstance);
static int handle_source_rewind(void *pInstance);
static int handle_source_close(void *pInstance);
static int handle_source_skip(void *pInstance, int32_t skip);

/*
 * Raise a fault.
 * 
 * This function does not return.
 * 
 * Parameters:
 * 
 *   lnum - the line number (__LINE__)
 */
static void fault(long lnum) {
  if (m_fault != NULL) {
    m_fault(lnum);
  } else {
    fprintf(stderr, "Fault within libsmfparse at line %ld\n", lnum);
  }
  exit(EXIT_FAILURE);
}

/*
 * Implementation of the read callback for HANDLE_SOURCE.
 * 
 * See the specification of the smfsource_fp_read function pointer type
 * for the interface.
 */
static int handle_source_read(void *pInstance) {
  
  HANDLE_SOURCE *ps = NULL;
  int c = 0;
  
  /* Check parameter */
  if (pInstance == NULL) {
    fault(__LINE__);
  }
  
  /* Cast instance data */
  ps = (HANDLE_SOURCE *) pInstance;
  
  /* If we don't know the file length, check that this read won't exceed
   * the size limit; if we know the file length, check that this read
   * does not go beyond the end of the file; in the former case, a
   * failed check causes an I/O error, while in the latter case a failed
   * check causes EOF condition */
  if (ps->flen < 0) {
    if (ps->fptr >= HANDLE_FILE_MAXLEN) {
      c = SMFSOURCE_IOERR;
    }
  } else {
    if (ps->fptr >= ps->flen) {
      c = SMFSOURCE_EOF;
    }
  }
  
  /* Read from file if we're not already in a special condition */
  if (c >= 0) {
    c = fgetc(ps->fh);
    if (c == EOF) {
      if (feof(ps->fh)) {
        c = SMFSOURCE_EOF;
      } else {
        c = SMFSOURCE_IOERR;
      }
    }
  }
  
  /* If read was successful, increase the file pointer */
  if (c >= 0) {
    (ps->fptr)++;
  }
  
  /* Return result */
  return c;
}

/*
 * Implementation of the rewind callback for HANDLE_SOURCE.
 * 
 * See the specification of the smfsource_fp_rewind function pointer
 * type for the interface.
 * 
 * This callback must only be registered for handles that support random
 * access.
 */
static int handle_source_rewind(void *pInstance) {
  
  int status = 1;
  HANDLE_SOURCE *ps = NULL;
  
  /* Check parameter */
  if (pInstance == NULL) {
    fault(__LINE__);
  }
  
  /* Cast instance data and make sure random access supported */
  ps = (HANDLE_SOURCE *) pInstance;
  if (!(ps->can_seek)) {
    fault(__LINE__);
  }
  
  /* Attempt a rewind */
  errno = 0;
  rewind(ps->fh);
  if (errno) {
    status = 0;
  }
  
  /* If successful rewind, reset byte offset field */
  if (status) {
    ps->fptr = 0;
  }
  
  /* Return status */
  return status;
}

/*
 * Implementation of the close callback for HANDLE_SOURCE.
 * 
 * See the specification of the smfsource_fp_close function pointer type
 * for the interface.
 */
static int handle_source_close(void *pInstance) {
  
  int status = 1;
  HANDLE_SOURCE *ps = NULL;
  
  /* Check parameter */
  if (pInstance == NULL) {
    fault(__LINE__);
  }
  
  /* Cast instance data */
  ps = (HANDLE_SOURCE *) pInstance;
  
  /* If we are the owner of the file handle, close it */
  if (ps->is_owner) {
    if (fclose(ps->fh)) {
      status = 0;
    }
  }
  
  /* Clear file handle and release instance data */
  ps->fh = NULL;
  free(ps);
  ps = NULL;
  pInstance = NULL;
  
  /* Return status */
  return status;
}

/*
 * Implementation of the skip callback for HANDLE_SOURCE.
 * 
 * See the implementation of the smfsource_fp_skip function pointer type
 * for the interface.
 * 
 * This callback must only be registered for handles that support random
 * access.
 */
static int handle_source_skip(void *pInstance, int32_t skip) {
  
  HANDLE_SOURCE *ps = NULL;
  int status = 1;
  
  /* Check parameters */
  if (pInstance == NULL) {
    fault(__LINE__);
  }
  if (skip < 0) {
    fault(__LINE__);
  }
  
  /* Cast instance data and make sure random access supported */
  ps = (HANDLE_SOURCE *) pInstance;
  if (!(ps->can_seek)) {
    fault(__LINE__);
  }
  
  /* If skip would go beyond end of file, shorten skip so it just goes
   * to the end of the file */
  if (skip > ps->flen - ps->fptr) {
    skip = ps->flen - ps->fptr;
  }
  
  /* Only proceed if non-zero skip */
  if (skip > 0) {
    /* Attempt to seek ahead */
    if (fseek(ps->fh, (long) skip, SEEK_CUR)) {
      status = 0;
    }
    
    /* If successful, update file pointer */
    if (status) {
      ps->fptr += skip;
    }
  }
  
  /* Return status */
  return status;
}

/*
 * Public function implementations
 * ===============================
 * 
 * See the header for specifications
 */

/*
 * smf_set_fault function.
 */
void smf_set_fault(smf_fp_fault fFault) {
  m_fault = fFault;
}

/*
 * smfsource_custom function.
 */
SMFSOURCE *smfsource_custom(
    void                * pInstance,
    smfsource_fp_read     fRead,
    smfsource_fp_rewind   fRewind,
    smfsource_fp_close    fClose,
    smfsource_fp_skip     fSkip) {
  
  SMFSOURCE *ps = NULL;
  
  /* Check parameters */
  if (fRead == NULL) {
    fault(__LINE__);
  }
  
  /* Allocate new source object */
  ps = (SMFSOURCE *) calloc(1, sizeof(SMFSOURCE));
  if (ps == NULL) {
    fault(__LINE__);
  }
  
  /* Initialize object */
  ps->state     = SOURCE_STATE_NORMAL;
  ps->pInstance = pInstance;
  ps->fRead     = fRead;
  ps->fRewind   = fRewind;
  ps->fClose    = fClose;
  ps->fSkip     = fSkip;
  
  /* Return new object */
  return ps;
}

/*
 * smfsource_new_handle function.
 */
SMFSOURCE *smfsource_new_handle(
    FILE * pIn,
    int    is_owner,
    int    can_seek,
    int  * pErr) {
  
  int status = 1;
  int dummy = 0;
  long flen = -1;
  
  HANDLE_SOURCE *ph = NULL;
  SMFSOURCE *ps = NULL;
  
  /* Check parameters */
  if (pIn == NULL) {
    fault(__LINE__);
  }
  
  /* If no error return given, redirect to dummy */
  if (pErr == NULL) {
    pErr = &dummy;
  }
  
  /* Clear error return */
  *pErr = 0;
  
  /* If random access supported, attempt to find the file length and
   * rewind the file */
  if (can_seek) {
    if (fseek(pIn, 0, SEEK_END)) {
      status = 0;
      *pErr = SMF_ERR_IO;
    }
    
    if (status) {
      flen = ftell(pIn);
      if (flen < 0) {
        status = 0;
        *pErr = SMF_ERR_IO;
      } else if (flen > HANDLE_FILE_MAXLEN) {
        status = 0;
        *pErr = SMF_ERR_HUGE_FILE;
      }
    }
    
    if (status) {
      errno = 0;
      rewind(pIn);
      if (errno) {
        status = 0;
        *pErr = SMF_ERR_IO;
      }
    }
  }
  
  /* Allocate new handle source structure */
  if (status) {
    ph = (HANDLE_SOURCE *) calloc(1, sizeof(HANDLE_SOURCE));
    if (ph == NULL) {
      fault(__LINE__);
    }
  }
  
  /* Fill in the handle source structure */
  if (status) {
    ph->fh = pIn;
    ph->fptr = 0;
    
    if (can_seek) {
      ph->flen = (int32_t) flen;
    } else {
      ph->flen = -1;
    }
    
    if (is_owner) {
      ph->is_owner = 1;
    } else {
      ph->is_owner = 0;
    }
    
    if (can_seek) {
      ph->can_seek = 1;
    } else {
      ph->can_seek = 0;
    }
  }
  
  /* Construct the new input source */
  if (status) {
    if (can_seek) {
      ps = smfsource_custom(
              ph,
              &handle_source_read,
              &handle_source_rewind,
              &handle_source_close,
              &handle_source_skip);
    } else {
      ps = smfsource_custom(
              ph,
              &handle_source_read,
              NULL,
              &handle_source_close,
              NULL);
    }
  }
  
  /* Return the new source, or NULL */
  return ps;
}

/*
 * smfsource_new_path function.
 */
SMFSOURCE *smfsource_new_path(const char *pPath, int *pErr) {
  
  int dummy = 0;
  int status = 1;
  FILE *fh = NULL;
  SMFSOURCE *ps = NULL;
  
  /* Check parameters */
  if (pPath == NULL) {
    fault(__LINE__);
  }
  
  /* If pErr not provided, redirect to dummy */
  if (pErr == NULL) {
    pErr = &dummy;
  }
  
  /* Reset pErr */
  *pErr = 0;
  
  /* Open the file */
  fh = fopen(pPath, "rb");
  if (fh == NULL) {
    status = 0;
    *pErr = SMF_ERR_OPEN_FILE;
  }
  
  /* Call through */
  if (status) {
    ps = smfsource_new_handle(fh, 1, 1, pErr);
  }
  
  /* Return source object or NULL */
  return ps;
}

/*
 * smfsource_close function.
 */
int smfsource_close(SMFSOURCE *pSrc) {
  int status = 1;
  
  /* Only proceed if non-NULL passed */
  if (pSrc != NULL) {
    
    /* If destructor is registered, invoke it */
    if (pSrc->fClose != NULL) {
      if (!(pSrc->fClose(pSrc->pInstance))) {
        status = 0;
      }
    }
    
    /* Release memory block */
    free(pSrc);
    pSrc = NULL;
  }
  
  /* Return status */
  return status;
}

/*
 * smfsource_canRewind function.
 */
int smfsource_canRewind(SMFSOURCE *pSrc) {
  
  int result = 0;
  
  /* Check parameters */
  if (pSrc == NULL) {
    fault(__LINE__);
  }
  
  /* Query object */
  if (pSrc->fRewind != NULL) {
    result = 1;
  } else {
    result = 0;
  }
  
  /* Return result */
  return result;
}

/*
 * smfsource_rewind function.
 */
int smfsource_rewind(SMFSOURCE *pSrc) {
  
  int status = 1;
  
  /* Check parameters */
  if (pSrc == NULL) {
    fault(__LINE__);
  }
  
  /* If rewinding not supported, fail without any state change */
  if (pSrc->fRewind == NULL) {
    status = 0;
  }
  
  /* If in double-error mode, fail without attempting rewind again */
  if (status) {
    if (pSrc->state == SOURCE_STATE_DOUBLE) {
      status = 0;
    }
  }
  
  /* If we got here successfully, attempt a rewind */
  if (status) {
    if (!pSrc->fRewind(pSrc->pInstance)) {
      status = 0;
      pSrc->state = SOURCE_STATE_DOUBLE;
    }
  }
  
  /* Return status */
  return status;
}

/*
 * smfsource_skip function.
 */
int smfsource_skip(SMFSOURCE *pSrc, int32_t skip) {
  
  int status = 1;
  int32_t i = 0;
  int c = 0;
  
  /* Check parameters */
  if ((pSrc == NULL) || (skip < 0)) {
    fault(__LINE__);
  }
  
  /* If in an error state, fail */
  if ((pSrc->state == SOURCE_STATE_ERROR) ||
      (pSrc->state == SOURCE_STATE_DOUBLE)) {
    status = 0;
  }
  
  /* Only proceed if non-zero skip distance and in normal state */
  if (status && (skip > 0) && (pSrc->state == SOURCE_STATE_NORMAL)) {
    /* Check whether we have a skip callback */
    if (pSrc->fSkip != NULL) {
      /* We have a skip callback, so use that */
      if (!pSrc->fSkip(pSrc->pInstance, skip)) {
        status = 0;
        pSrc->state = SOURCE_STATE_ERROR;
      }
      
    } else {
      /* We don't have a skip callback, so use read callback
       * repeatedly */
      for(i = 0; i < skip; i++) {
        /* Attempt a read */
        c = pSrc->fRead(pSrc->pInstance);
        
        /* If we got EOF, then leave loop successfully after changing to
         * EOF state; if we got an error, then leave loop in error after
         * changing to error state */
        if (c == SMFSOURCE_EOF) {
          pSrc->state = SOURCE_STATE_EOF;
          break;
          
        } else if (c == SMFSOURCE_IOERR) {
          status = 0;
          pSrc->state = SOURCE_STATE_ERROR;
          break;
        }
      }
    }
  }
  
  /* Return status */
  return status;
}

/*
 * smfsource_read function.
 */
int smfsource_read(SMFSOURCE *pSrc) {
  
  int c = 0;
  
  /* Check parameters */
  if (pSrc == NULL) {
    fault(__LINE__);
  }
  
  /* If in an error state, fail */
  if ((pSrc->state == SOURCE_STATE_ERROR) ||
      (pSrc->state == SOURCE_STATE_DOUBLE)) {
    c = SMFSOURCE_IOERR;
  }
  
  /* Call through if in normal state; else, if in EOF state, set an EOF
   * return */
  if (c != SMFSOURCE_IOERR) {
    if (pSrc->state == SOURCE_STATE_NORMAL) {
      c = pSrc->fRead(pSrc->pInstance);
      
      if (c == SMFSOURCE_IOERR) {
        pSrc->state = SOURCE_STATE_ERROR;
      
      } else if (c == SMFSOURCE_EOF) {
        pSrc->state = SOURCE_STATE_EOF;
      }
      
    } else if (pSrc->state == SOURCE_STATE_EOF) {
      c = SMFSOURCE_EOF;
      
    } else {
      fault(__LINE__);
    }
  }
  
  /* Return result */
  return c;
}

/*
 * smf_errorString function.
 */
const char *smf_errorString(int code) {
  const char *pResult = NULL;
  
  switch (code) {
    case SMF_ERR_IO:
      pResult = "I/O error";
      break;
    
    case SMF_ERR_HUGE_FILE:
      pResult = "MIDI file exceeds 1 GiB in size";
      break;
    
    case SMF_ERR_OPEN_FILE:
      pResult = "Failed to open MIDI file";
      break;
    
    default:
      pResult = "Unknown error";
  }
  
  return pResult;
}
