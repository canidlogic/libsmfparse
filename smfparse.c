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
 * The initial and maximum capacities of the data buffer used for
 * storing system exclusive message payloads, text data payloads, and
 * custom meta-event data.
 */
#define BCAP_INIT INT32_C(256)
#define BCAP_MAX  INT32_C(32768)

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
 * SMFPARSE structure declaration.
 * 
 * Prototype given in header.
 */
struct SMFPARSE_TAG {
  
  /*
   * Zero if just constructed, one if header has been read, two if in
   * EOF state, less than zero if in error state.
   * 
   * If less than zero, it matches the error code that caused the error
   * condition.
   */
  int status;
  
  /*
   * The amount of bytes remaining to be read in the currently open
   * chunk, or -1 if no chunk is currently open.
   */
  int32_t ckrem;
  
  /*
   * The number of track chunks that have been encountered.
   */
  int32_t trkcount;
  
  /*
   * The header information, if status is greater than zero.
   * 
   * When returning to client, a separate header structure is used to
   * prevent client modifications (rHead).
   */
  SMF_HEADER head;
  
  /*
   * Structures used for returning values with the entity.
   */
  SMF_HEADER   rHead;
  SMF_TIMECODE rTC;
  SMF_TIMESIG  rTS;
  SMF_KEYSIG   rKS;
  
  /*
   * Data buffer state.
   * 
   * blen is the actual number of bytes currently in the buffer.  It
   * must be in range zero to bcap, inclusive.
   * 
   * bcap is the total size of the buffer in bytes.  The initial
   * capacity is BCAP_INIT and the maximum capacity is BCAP_MAX.
   * Capacity grows by doubling, maxing out at BCAP_MAX.
   * 
   * bptr is a pointer to the dynamically-allocated buffer block.  If
   * bcap is zero, this will be NULL.  Otherwise, it will be non-NULL
   * with a size matching bcap bytes.
   */
  int32_t   blen;
  int32_t   bcap;
  uint8_t * bptr;
  
  /*
   * Running status state.
   * 
   * This holds a status byte that can be used for running status, or -1
   * if there is no buffered running status byte.
   */
  int run;
};

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

static int pushBuffer(SMFPARSE *ps, int c);

static int32_t readUint16BE(SMFSOURCE *pSrc, int *pErr);
static int readUint32BE(uint32_t *pv, SMFSOURCE *pSrc, int *pErr);
static int readChunkByte(SMFSOURCE *pSrc, int32_t *pRem, int *pErr);
static int32_t readChunkVar(SMFSOURCE *pSrc, int32_t *pRem, int *pErr);

static int readChunkHead(
    uint32_t  * pType,
    int32_t   * pLen,
    SMFSOURCE * pSrc,
    int       * pErr);
static int readHeaderChunk(SMF_HEADER *ph, SMFSOURCE *pSrc, int *pErr);
static int parseEvent(
    SMFPARSE   * ps,
    SMF_ENTITY * pEnt,
    int          ev,
    int          a,
    int          b,
    int32_t      delta,
    int        * pErr);
static int readEvent(
    SMFPARSE   * ps,
    SMF_ENTITY * pEnt,
    SMFSOURCE  * pSrc,
    int        * pErr);

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
 * Push a data byte into the data buffer of the given parser object.
 * 
 * The buffer will be expanded if necessary.  The function fails if the
 * buffer is full and has reached the maximum allowed capacity.
 * 
 * Parameters:
 * 
 *   ps - the parser object
 * 
 *   c - the unsigned byte value to append to the buffer, range 0-255
 * 
 * Return:
 * 
 *   non-zero if successful, zero if no more room nor capacity
 */
static int pushBuffer(SMFPARSE *ps, int c) {
  
  int status = 1;
  int32_t new_cap = 0;
  
  /* Check parameters */
  if ((ps == NULL) || (c < 0) || (c > 255)) {
    fault(__LINE__);
  }
  
  /* If data buffer not allocated, allocate it with initial capacity */
  if (ps->bcap < 1) {
    ps->bptr = (uint8_t *) calloc((size_t) BCAP_INIT, 1);
    if (ps->bptr == NULL) {
      fault(__LINE__);
    }
    ps->bcap = BCAP_INIT;
    ps->blen = 0;
  }
  
  /* Expand capacity if necessary */
  if (ps->blen >= ps->bcap) {
    /* Check that we have not reached maximum capacity */
    if (ps->bcap >= BCAP_MAX) {
      status = 0;
    }
    
    /* Proceed if we can expand capacity */
    if (status) {
      /* New capacity is minimum of double current capacity and the
       * maximum allowed capacity */
      new_cap = ps->bcap * 2;
      if (new_cap > BCAP_MAX) {
        new_cap = BCAP_MAX;
      }
      
      /* Expand buffer */
      ps->bptr = (uint8_t *) realloc(ps->bptr, (size_t) new_cap);
      memset(
        &((ps->bptr)[ps->bcap]),
        0,
        (size_t) (new_cap - ps->bcap));
      ps->bcap = new_cap;
    }
  }
  
  /* Add byte to buffer */
  if (status) {
    (ps->bptr)[ps->blen] = (uint8_t) c;
    (ps->blen)++;
  }
  
  /* Return status */
  return status;
}

/*
 * Read an unsigned 16-bit integer in big endian order from the input
 * source.
 * 
 * Parameters:
 * 
 *   pSrc - the input source to read from
 * 
 *   pErr - receives an error code if failure
 * 
 * Return:
 * 
 *   the unsigned 16-bit value, or -1 if failure
 */
static int32_t readUint16BE(SMFSOURCE *pSrc, int *pErr) {
  
  int c = 0;
  int i = 0;
  int32_t result = 0;
  
  /* Check parameters */
  if ((pSrc == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  
  /* Read the two bytes and add to result */
  for(i = 0; i < 2; i++) {
    c = smfsource_read(pSrc);
    
    if (c == SMFSOURCE_IOERR) {
      *pErr = SMF_ERR_IO;
      result = -1;
      break;
      
    } else if (c == SMFSOURCE_EOF) {
      *pErr = SMF_ERR_EOF;
      result = -1;
      break;
    }
    
    result <<= 8;
    result |= ((int32_t) c);
  }
  
  /* Return result or -1 */
  return result;
}

/*
 * Read an unsigned 32-bit integer in big-endian order from the input
 * source.
 * 
 * Parameters:
 * 
 *   pv - the variable to receive the result
 * 
 *   pSrc - the input source to read from
 * 
 *   pErr - receives an error code if failure
 * 
 * Return:
 * 
 *   non-zero if successful, zero if error
 */
static int readUint32BE(uint32_t *pv, SMFSOURCE *pSrc, int *pErr) {
  
  int status = 1;
  int c = 0;
  int i = 0;
  
  /* Check parameters */
  if ((pi == NULL) || (pSrc == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  
  /* Clear result */
  *pv = 0;
  
  /* Read the four bytes and add to result */
  for(i = 0; i < 4; i++) {
    c = smfsource_read(pSrc);
    
    if (c == SMFSOURCE_IOERR) {
      *pErr = SMF_ERR_IO;
      status = 0;
      break;
      
    } else if (c == SMFSOURCE_EOF) {
      *pErr = SMF_ERR_EOF;
      status = 0;
      break;
    }
    
    *pv <<= 8;
    *pv |= ((uint32_t) c);
  }
  
  /* Return status */
  return status;
}

/*
 * Read a byte from within a chunk.
 * 
 * pRem points to a variable that tracks the number of remaining bytes
 * in the chunk.  If less than one, the function fails.  If this
 * function is successful, the remainder count will be decremented.
 * 
 * Parameters:
 * 
 *   pSrc - the input source to read from
 * 
 *   pRem - the remaining bytes counter
 * 
 *   pErr - receives an error code in case of failure
 * 
 * Return:
 * 
 *   the unsigned byte value that was read, in range 0 to 255 inclusive,
 *   or -1 if failure
 */
static int readChunkByte(SMFSOURCE *pSrc, int32_t *pRem, int *pErr) {
  
  int c = 0;
  
  /* Check parameters */
  if ((pSrc == NULL) || (pRem == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  
  /* If no remaining bytes, fail */
  if (*pRem < 1) {
    c = -1;
    *pErr = SMF_ERR_OPEN_TRACK;
  }
  
  /* Read from the track */
  if (c >= 0) {
    c = smfsource_read(pSrc);
    
    if (c == SMFSOURCE_EOF) {
      c = -1;
      *pErr = SMF_ERR_EOF;
      
    } else if (c == SMFSOURCE_IOERR) {
      c = -1;
      *pErr = SMF_ERR_IO;
    }
  }
  
  /* Decrement byte counter */
  if (c >= 0) {
    (*pRem)--;
  }
  
  /* Return byte read or -1 */
  return c;
}

/*
 * Read a variable-length integer from within a chunk.
 * 
 * pRem points to a variable that tracks the number of remaining bytes
 * in the chunk.  If less than one, the function fails.  If this
 * function is successful, the remainder count will be decremented by
 * the number of bytes read in the variable-length integer.
 * 
 * Parameters:
 * 
 *   pSrc - the input source to read
 * 
 *   pRem - the remaining bytes counter
 * 
 *   pErr - receives an error code in case of failure
 * 
 * Return:
 * 
 *   the variable-length integer quantity, which is always zero or
 *   greater, or -1 if failure
 */
static int32_t readChunkVar(SMFSOURCE *pSrc, int32_t *pRem, int *pErr) {
  
  int bc = 0;
  int c = 0;
  int32_t result = 0;
  
  /* Check parameters */
  if ((pSrc == NULL) || (pRem == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  
  /* Processing loop */
  while (1) {
    /* Read a byte from the chunk */
    c = readChunkByte(pSrc, pRem, pErr);
    if (c < 0) {
      result = -1;
      break;
    }
    
    /* Incorporate the seven least significant bits of this byte into
     * the result */
    result = (result << 7) | ((int32_t) (c & 0x7f));
    
    /* If most significant bit of this byte is clear, then leave loop */
    if (c < 0x80) {
      break;
    }
    
    /* If we got here, increment the byte count before looping back; if
     * byte count has reached 4 then error because variable-length
     * quantities with more than 4 encoded bytes are not allowed */
    bc++;
    if (bc >= 4) {
      result = -1;
      *pErr = SMF_ERR_LONG_VARINT;
      break;
    }
  }
  
  /* Return result or -1 */
  return result;
}

/*
 * Read the header of a chunk within a MIDI file.
 * 
 * pType and pLen point to the variables to receive the type of chunk
 * and the length of the chunk in bytes.  The length of the chunk does
 * not include the chunk header.
 * 
 * pSrc is the input source to read the header from.  Reading starts
 * sequentially at current position.  Upon successful return, the input
 * source will be positioned to read the first byte of data within the
 * chunk.
 * 
 * pErr points to a variable to receive an error code if the function
 * fails.
 * 
 * Parameters:
 * 
 *   pType - pointer to the output chunk type variable
 * 
 *   pLen - pointer to the output chunk length variable
 * 
 *   pSrc - the input source
 * 
 *   pErr - pointer to the error code variable
 * 
 * Return:
 * 
 *   non-zero if successful, zero if error
 */
static int readChunkHead(
    uint32_t  * pType,
    int32_t   * pLen,
    SMFSOURCE * pSrc,
    int       * pErr) {
  
  int status = 1;
  uint32_t lv = 0;
  
  /* Check parameters */
  if ((pType == NULL) || (pLen == NULL) ||
      (pSrc == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  
  /* Read the type */
  if (!readUint32BE(pType, pSrc, pErr)) {
    status = 0;
  }
  
  /* Read the length as unsigned */
  if (status) {
    if (!readUint32BE(&lv, pSrc, pErr)) {
      status = 0;
    }
  }
  
  /* Make sure length in signed range */
  if (status) {
    if (lv > INT32_MAX) {
      status = 0;
      *pErr = SMF_ERR_HUGE_CHUNK;
    }
  }
  
  /* Write the signed length and return status */
  if (status) {
    *pLen = (int32_t) lv;
  }
  
  return status;
}

/*
 * Read the MIDI header chunk.
 * 
 * Parameters:
 * 
 *   ph - the structure to store the parsed results
 * 
 *   pSrc - the source to read from
 * 
 *   pErr - stores the error code on failure
 * 
 * Return:
 * 
 *   non-zero if successful, zero if error
 */
static int readHeaderChunk(SMF_HEADER *ph, SMFSOURCE *pSrc, int *pErr) {
  
  int status = 1;
  
  uint32_t ck_type = 0;
  int32_t ck_len = 0;
  
  int32_t fmt = 0;
  int32_t ntrks = 0;
  int32_t division = 0;
  
  int32_t subdiv = 0;
  int frame_rate = 0;
  
  /* Check parameters */
  if ((ph == NULL) || (pSrc == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  
  /* Read the header chunk header */
  if (!readChunkHead(&ck_type, &ck_len, pSrc, pErr)) {
    status = 0;
  }
  
  /* Make sure header chunk is correct type */
  if (status) {
    if (ck_type != UINT32_C(0x4d546864)) {
      status = 0;
      *pErr = SMF_ERR_SIGNATURE;
    }
  }
  
  /* Make sure header chunk is at least six bytes */
  if (status) {
    if (ck_len < 6) {
      status = 0;
      *pErr = SMF_ERR_HEADER;
    }
  }
  
  /* Read the three header fields */
  if (status) {
    fmt = readUint16BE(pSrc, pErr);
    if (fmt < 0) {
      status = 0;
    }
  }
  
  if (status) {
    ntrks = readUint16BE(pSrc, pErr);
    if (ntrks < 0) {
      status = 0;
    }
  }
  
  if (status) {
    division = readUint16BE(pSrc, pErr);
    if (division < 0) {
      status = 0;
    }
  }
  
  /* Skip any remaining bytes in the header */
  if (status) {
    if (!smfsource_skip(pSrc, ck_len - 6)) {
      status = 0;
      *pErr = SMF_ERR_IO;
    }
  }
  
  /* Check fmt range */
  if (status) {
    if (fmt > 2) {
      status = 0;
      *pErr = SMF_ERR_MIDI_FMT;
    }
  }
  
  /* Check at least one track */
  if (status) {
    if (ntrks < 1) {
      status = 0;
      *pErr = SMF_ERR_NO_TRACKS;
    }
  }
  
  /* Check that if format 0, there is at most one track */
  if (status) {
    if ((fmt == 0) && (ntrks > 1)) {
      status = 0;
      *pErr = SMF_ERR_MULTI_TRACK;
    }
  }
  
  /* Decypher the division field */
  if (status) {
    if ((division & INT32_C(0x8000)) == 0) {
      /* High bit clear, so the division gives the number of delta units
       * per beat ("MIDI quarter note") */
      if (division > 0) {
        subdiv = division;
        frame_rate = 0;
        
      } else {
        status = 0;
        *pErr = SMF_ERR_HEADER;
      }
      
    } else {
      /* High bit set, so this is an SMPTE time; parse the subfields */
      frame_rate = (int) (division >> 8);
      frame_rate = (frame_rate ^ 0xff) + 1;
      subdiv = division & 0xff;
      
      /* Check frame rate and subdivision in range */
      if (((frame_rate != 24) && (frame_rate != 25) &&
            (frame_rate != 29) && (frame_rate != 30)) ||
          (subdiv < 1)) {
        status = 0;
        *pErr = SMF_ERR_HEADER;
      }
    }
  }
  
  /* Fill in the header structure */
  if (status) {
    ph->fmt = (int) fmt;
    ph->nTracks = ntrks;
    (ph->ts).subdiv = subdiv;
    (ph->ts).frame_rate = frame_rate;
  }
  
  /* Return status */
  return status;
}

/*
 * Parse an event that we have fully read from input and fill in the
 * entity structure appropriately.
 * 
 * The parser state must have a status code of 1 and a ckrem value that
 * is zero or greater.  The entity structure is assumed to be in a reset
 * state.
 * 
 * The ev parameter is the lead byte of the message to parse.  If
 * running status is used, this must be the cached running status byte.
 * 
 * The "A" parameter is the first parameter, or -1 if there is no such
 * parameter.
 * 
 * The "B" parameter is the second parameter, or -1 if there is no such
 * parameter.
 * 
 * For System Exclusive and meta-events, the data payload should already
 * be read into the parser data buffer.  For End Of Track, all remaining
 * data in the track should have already been skipped.
 * 
 * For meta-events, the event type should be the "A" parameter.
 * 
 * If the parse operation fails, the entity is not modified and the
 * parser status is not changed.  Instead, the error code is written to
 * pErr and zero is returned.
 * 
 * Parameters:
 * 
 *   ps - the parser object
 * 
 *   pEnt - the entity structure to fill in
 * 
 *   ev - the lead/status byte
 * 
 *   a - the first parameter, or -1
 * 
 *   b - the second parameter, or -1
 * 
 *   delta - the delta time prior to the event
 * 
 *   pErr - receives the error code if failure
 * 
 * Return:
 * 
 *   non-zero if successful, zero if error
 */
static int parseEvent(
    SMFPARSE   * ps,
    SMF_ENTITY * pEnt,
    int          ev,
    int          a,
    int          b,
    int32_t      delta,
    int        * pErr) {
  
  int status = 1;
  int msg = 0;
  int ch = 0;
  
  /* Check parameters */
  if ((ps == NULL) || (pEnt == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  if ((ev < 0) || (ev > 255)) {
    fault(__LINE__);
  }
  if ((a < -1) || (a > 255)) {
    fault(__LINE__);
  }
  if ((b < -1) || (b > 255)) {
    fault(__LINE__);
  }
  if ((b >= 0) && (a < 0)) {
    fault(__LINE__);
  }
  if ((delta < 0) || (delta > SMF_MAX_VARINT)) {
    fault(__LINE__);
  }
  
  /* Handle the different event classes */
  if ((ev == 0xf0) || (ev == 0xf7)) {
    /* System exclusive event, so there shouldn't be any parameters */
    if ((a != -1) || (b != -1)) {
      fault(__LINE__);
    }
    
    /* Set the entity type, delta, and data buffer */
    if (ev == 0xf0) {
      pEnt->status = SMF_TYPE_SYSEX;
      
    } else if (ev == 0xf7) {
      pEnt->status = SMF_TYPE_SYSESC;
      
    } else {
      fault(__LINE__);
    }
    
    pEnt->delta = delta;
    pEnt->buf_len = ps->blen;
    if (pEnt->buf_len > 0) {
      pEnt->buf_ptr = ps->bptr;
    } else {
      pEnt->buf_ptr = NULL;
    }
    
  } else if (ev == 0xff) {
    /* Meta-event, so there should be an "A" parameter with the event
     * type and no "B" parameter */
    if ((a == -1) || (b != -1)) {
      fault(__LINE__);
    }
    
    /* Handle the different meta-events */
    if (a == 0x00) {
      /* Sequence Number, so there should be exactly two buffered data
       * bytes */
      if (ps->buf_len != 2) {
        status = 0;
        *pErr = SMF_ERR_SEQ_NUM;
      }
      
      /* Decode the payload as a big-endian 16-bit value containing the
       * sequence number */
      if (status) {
        pEnt->seq_num =
          (((int32_t) (ps->bptr)[0]) << 8) |
           ((int32_t) (ps->bptr)[1]);
      }
      
      /* Also set the status and delta */
      if (status) {
        pEnt->status = SMF_TYPE_SEQ_NUM;
        pEnt->delta = delta;
      }
      
    } else if ((a >= 0x01) && (a <= 0x07)) {
      /* One of the text events, so use the "A" parameter as the text
       * subclass, and also set the status, delta, and data buffer */
      pEnt->status = SMF_TYPE_TEXT;
      pEnt->delta = delta;
      pEnt->buf_len = ps->blen;
      if (pEnt->buf_len > 0) {
        pEnt->buf_ptr = ps->bptr;
      } else {
        pEnt->buf_ptr = NULL;
      }
      pEnt->txtype = a;
      
    } else if (a == 0x20) {
      /* MIDI channel prefix, so there should be exactly one buffered
       * data byte */
      if (ps->buf_len != 1) {
        status = 0;
        *pErr = SMF_ERR_CH_PREFIX;
      }
      
      /* Get the buffered data byte into "B" */
      if (status) {
        b = (ps->bptr)[0];
      }
      
      /* The buffered data byte must be in range 0-15 */
      if (status) {
        if ((b < 0) || (b > 15)) {
          status = 0;
          *pErr = SMF_ERR_CH_PREFIX;
        }
      }
      
      /* Set the status, delta, and channel */
      if (status) {
        pEnt->status = SMF_TYPE_CH_PREFIX;
        pEnt->delta = delta;
        pEnt->ch = b;
      }
      
    } else if (a == 0x2f) {
      /* End Of Track, so there should be no buffered data bytes */
      if (ps->buf_len != 0) {
        status = 0;
        *pErr = SMF_ERR_BAD_EOT;
      }
      
      /* Verify that chunk remaining bytes are zero and then set to -1
       * to leave the chunk */
      if (status) {
        if (ps->ckrem != 0) {
          fault(__LINE__);
        }
        ps->ckrem = -1;
      }
      
      /* Set the status and delta */
      if (status) {
        pEnt->status = SMF_TYPE_END_TRACK;
        pEnt->delta = delta;
      }
      
    } else if (a == 0x51) {
      /* Set Tempo, so there should be exactly 3 data bytes */
      if (ps->buf_len != 3) {
        status = 0;
        *pErr = SMF_ERR_SET_TEMPO;
      }
      
      /* Verify the bytes aren't all zero */
      if (status) {
        if (((ps->bptr)[0] == 0) && ((ps->bptr)[1] == 0) &&
            ((ps->bptr)[2] == 0)) {
          status = 0;
          *pErr = SMF_ERR_SET_TEMPO;
        }
      }
      
      /* Set the status, delta, and beat duration */
      if (status) {
        pEnt->status = SMF_TYPE_TEMPO;
        pEnt->delta = delta;
        pEnt->beat_dur =
          (((int32_t) (ps->bptr)[0]) << 16) |
          (((int32_t) (ps->bptr)[1]) <<  8) |
          (((int32_t) (ps->bptr)[2]));
      }
      
    } else if (a == 0x54) {
      /* SMPTE Offset, so there should be exactly 5 data bytes */
      if (ps->buf_len != 5) {
        status = 0;
        *pErr = SMF_ERR_SMPTE_OFF;
      }
      
      /* Clear the timecode structure and read the data bytes into it */
      if (status) {
        memset(&(ps->rTC), 0, sizeof(SMF_TIMECODE));
        (ps->rTC).hour   = (ps->bptr)[0];
        (ps->rTC).minute = (ps->bptr)[1];
        (ps->rTC).second = (ps->bptr)[2];
        (ps->rTC).frame  = (ps->bptr)[3];
        (ps->rTC).ff     = (ps->bptr)[4];
      }
      
      /* Check the ranges of all the fields */
      if (status) {
        if (((ps->rTC).hour   > 23) ||
            ((ps->rTC).minute > 59) ||
            ((ps->rTC).second > 59) ||
            ((ps->rTC).frame  > 29) ||
            ((ps->rTC).ff     > 99)) {
          status = 0;
          *pErr = SMF_ERR_SMPTE_OFF;
        }
      }
      
      /* If the MIDI file is in an SMPTE timing mode and the frame rate
       * is 24 or 25, then enforce restricted range (the 29 value uses
       * the same range as 30 because of drop-frame) */
      if (status) {
        if (((ps->head).ts.frame_rate >  0) &&
            ((ps->head).ts.frame_rate < 29)) {
          if ((ps->rTC).frame >= (ps->head).ts.frame_rate) {
            status = 0;
            *pErr = SMF_ERR_SMPTE_OFF;
          }
        }
      }
      
      /* If the MIDI file is in SMPTE drop-frame timing and the minutes
       * are neither zero nor divisible by 10, then enforce that neither
       * frame 0 nor 1 are used because those timecodes are dropped */
      if (status) {
        if ((ps->head).ts.frame_rate == 29) {
          if (((ps->rTC).minute % 10) != 0) {
            if ((ps->rTC).frame < 2) {
              status = 0;
              *pErr = SMF_ERR_SMPTE_OFF;
            }
          }
        }
      }
      
      /* Set the status, delta, and timecode fields */
      if (status) {
        pEnt->status = SMF_TYPE_SMPTE;
        pEnt->delta = delta;
        pEnt->tcode = &(ps->rTC);
      }
      
    } else if (a == 0x58) {
      /* Time Signature, so there should be exactly 4 data bytes */
      if (ps->buf_len != 4) {
        status = 0;
        *pErr = SMF_ERR_TIME_SIG;
      }
      
      /* Clear the time signature structure and read the data bytes
       * into it */
      if (status) {
        memset(&(ps->rTS), 0, sizeof(SMF_TIMESIG));
        (ps->rTS).numerator = (ps->bptr)[0];
        (ps->rTS).denominator = (ps->bptr)[1];
        (ps->rTS).click = (ps->bptr)[2];
        (ps->rTS).beat_unit = (ps->bptr)[3];
      }
      
      /* Numerator, click, and beat unit should be at least one */
      if (status) {
        if (((ps->rTS).numerator < 1) ||
            ((ps->rTS).click     < 1) ||
            ((ps->rTS).beat_unit < 1)) {
          status = 0;
          *pErr = SMF_ERR_TIME_SIG;
        }
      }
      
      /* Denominator should not exceed 15 so we can safely perform the
       * shifting */
      if (status) {
        if ((ps->rTS).denominator > 15) {
          status = 0;
          *pErr = SMF_ERR_TIME_SIG;
        }
      }
      
      /* Decode the denominator as a power of two, such that a
       * denominator of 0 decodes to 1, 1 to 2, 2 to 4, 3 to 8, and so
       * forth; then, make sure it is in range */
      if (status) {
        (ps->rTS).denominator = 1 << ((ps->rTS).denominator);
        if ((ps->rTS).denominator > SMF_MAX_TIME_DENOM) {
          status = 0;
          *pErr = SMF_ERR_TIME_SIG;
        }
      }
      
      /* Set the status, delta, and time signature */
      if (status) {
        pEnt->status = SMF_TYPE_TIME_SIG;
        pEnt->delta = delta;
        pEnt->tsig = &(ps->rTS);
      }
      
    } else if (a == 0x59) {
      /* Key signature, so there should be exactly 2 data bytes */
      if (ps->buf_len != 2) {
        status = 0;
        *pErr = SMF_ERR_KEY_SIG;
      }
      
      /* Clear the key signature structure and read in the data bytes */
      if (status) {
        memset(&(ps->rKS), 0, sizeof(SMF_KEYSIG));
        (ps->rKS).key = (ps->bptr)[0];
        (ps->rKS).is_minor = (ps->bptr)[1];
      }
      
      /* Decode the key value in two's-complement */
      if (status) {
        if ((ps->rKS).key > 0x7f) {
          (ps->rKS).key = (ps->rKS).key - 0x100;
        }
      }
      
      /* Check ranges */
      if (status) {
        if (((ps->rKS).key < SMF_MIN_KEYSIG) ||
            ((ps->rKS).key > SMF_MAX_KEYSIG)) {
          status = 0;
          *pErr = SMF_ERR_KEY_SIG;
        }
      }
      if (status) {
        if (((ps->rKS).is_minor != 1) &&
            ((ps->rKS).is_minor != 0)) {
          status = 0;
          *pErr = SMF_ERR_KEY_SIG;
        }
      }
      
      /* Set the status, delta, and key signature */
      if (status) {
        pEnt->status = SMF_TYPE_KEY_SIG;
        pEnt->delta = delta;
        pEnt->ksig = &(ps->rKS);
      }
      
    } else {
      /* Sequencer-specific meta-event or some undocumented meta-event,
       * so use a META entity and set the status, delta, data buffer,
       * and meta_type */
      pEnt->status = SMF_TYPE_META;
      pEnt->delta = delta;
      pEnt->buf_len = ps->blen;
      if (pEnt->buf_len > 0) {
        pEnt->buf_ptr = ps->bptr;
      } else {
        pEnt->buf_ptr = NULL;
      }
      pEnt->meta_type = a;
    }
    
  } else if ((ev >= 0x80) && (ev <= 0xef)) {
    /* MIDI message, so first split into a message and a channel */
    msg = ev & 0xf0;
    ch  = ev & 0x0f;
    
    /* Make sure that all messages except 0xC0 and 0xD0 have both
     * parameters, and 0xC0 and 0xD0 have only the "A" parameter */
    if ((msg == 0xc0) || (msg == 0xd0)) {
      if ((a == -1) || (b != -1)) {
        fault(__LINE__);
      }
    } else {
      if ((a == -1) || (b == -1)) {
        fault(__LINE__);
      }
    }
    
    /* Make sure that the defined parameters have their most significant
     * bits clear */
    if ((a >= 0) && (a > 0x7f)) {
      status = 0;
      *pErr = SMF_ERR_MIDI_DATA;
    }
    if (status) {
      if ((b >= 0) && (b > 0x7f)) {
        status = 0;
        *pErr = SMF_ERR_MIDI_DATA;
      }
    }
    
    /* Set the delta and channel for all MIDI messages */
    if (status) {
      pEnt->delta = delta;
      pEnt->ch = ch;
    }
    
    /* Set the status and message-specific data */
    if (status) {
      if (msg == 0x80) {
        pEnt->status = SMF_TYPE_NOTE_OFF;
        pEnt->key = a;
        pEnt->val = b;
        
      } else if (msg == 0x90) {
        pEnt->status = SMF_TYPE_NOTE_ON;
        pEnt->key = a;
        pEnt->val = b;
        
      } else if (msg == 0xa0) {
        pEnt->status = SMF_TYPE_KEY_AFTERTOUCH;
        pEnt->key = a;
        pEnt->val = b;
        
      } else if (msg == 0xb0) {
        pEnt->status = SMF_TYPE_CONTROL;
        pEnt->ctl = a;
        pEnt->val = b;
        
      } else if (msg == 0xc0) {
        pEnt->status = SMF_TYPE_PROGRAM;
        pEnt->val = a;
        
      } else if (msg == 0xd0) {
        pEnt->status = SMF_TYPE_CH_AFTERTOUCH;
        pEnt->val = a;
        
      } else if (msg == 0xe0) {
        pEnt->status = SMF_TYPE_PITCH_BEND;
        pEnt->bend = ((b << 7) | a) - 8192;
        
      } else {
        fault(__LINE__);
      }
    }
    
  } else {
    status = 0;
    *pErr = SMF_ERR_BAD_EVENT;
  }
  
  /* Return status */
  return status;
}

/*
 * Read an event from within a track.
 * 
 * The parser state must have a status code of 1 and a ckrem value that
 * is zero or greater.  The entity structure is assumed to be in a reset
 * state.
 * 
 * If the read operation fails, the entity is not modified and the
 * parser status is not changed.  Instead, the error code is written to
 * pErr and zero is returned.
 * 
 * Parameters:
 * 
 *   ps - the parser object
 * 
 *   pEnt - the entity to fill in
 * 
 *   pSrc - the input source to read from
 * 
 *   pErr - receives an error code if there is a failure
 * 
 * Return:
 * 
 *   non-zero if successful, zero if failure
 */
static int readEvent(
    SMFPARSE   * ps,
    SMF_ENTITY * pEnt,
    SMFSOURCE  * pSrc,
    int        * pErr) {
  
  int status = 1;
  int c = 0;
  int d = 0;
  int a = -1;
  int b = -1;
  int32_t delta = 0;
  int32_t vl = 0;
  int32_t vi = 0;
  
  /* Check parameters */
  if ((ps == NULL) || (pEnt == NULL) ||
      (pSrc == NULL) || (pErr == NULL)) {
    fault(__LINE__);
  }
  
  /* Check state */
  if ((ps->status != 1) || (ps->ckrem < 0)) {
    fault(__LINE__);
  }
  
  /* Reset data buffer */
  ps->blen = 0;
  
  /* Read the delta value */
  delta = readChunkVar(pSrc, &(ps->ckrem), pErr);
  if (delta < 0) {
    status = 0;
  }
  
  /* Read a byte to determine how to proceed */
  if (status) {
    c = readChunkByte(pSrc, &(ps->ckrem), pErr);
    if (c < 0) {
      status = 0;
    }
  }
  
  /* If we got a byte that doesn't have its most significant bit set,
   * then we need to retrieve a running status byte and set this byte we
   * just read as the "A" byte */
  if (status && (c < 0x80)) {
    if (ps->run < 0) {
      status = 0;
      *pErr = SMF_ERR_RUN_STATUS;
    }
    
    if (status) {
      a = c;
      c = ps->run;
    }
  }
  
  /* Handle the different cases of status byte to read the whole
   * message; we might already have read the "A" parameter for MIDI
   * messages if running status was used */
  if (status) {
    if (((c >= 0x80) && (c <= 0xbf)) || ((c >= 0xe0) && (c <= 0xef))) {
      /* MIDI message that has two parameters "A" and "B" */
      if (a < 0) {
        a = readChunkByte(pSrc, &(ps->ckrem), pErr);
        if (a < 0) {
          status = 0;
        }
      }
      
      if (status) {
        b = readChunkByte(pSrc, &(ps->ckrem), pErr);
        if (b < 0) {
          status = 0;
        }
      }
      
    } else if ((c >= 0xc0) && (c <= 0xdf)) {
      /* MIDI message that has one parameter "A" */
      if (a < 0) {
        a = readChunkByte(pSrc, &(ps->ckrem), pErr);
        if (a < 0) {
          status = 0;
        }
      }
      
    } else if ((c == 0xf0) || (c == 0xf7)) {
      /* System-Exclusive event that has a variable-length size and then
       * a payload which we read into the data buffer */
      vl = readChunkVar(pSrc, &(ps->ckrem), pErr);
      if (vl < 0) {
        status = 0;
      }
      
      /* Read the data payload */
      if (status) {
        for(vi = 0; vi < vl; vi++) {
          d = readChunkByte(pSrc, &(ps->ckrem), pErr);
          if (d < 0) {
            status = 0;
            break;
          }
          if (!pushBuffer(ps, d)) {
            status = 0;
            *pErr = SMF_ERR_BIG_PAYLOAD;
            break;
          }
        }
      }
      
    } else if (c == 0xff) {
      /* Meta-event that has a type we will store in "A" (no running
       * status allowed here) and then a variable-length size and then a
       * payload which we read into the data buffer */
      a = readChunkByte(pSrc, &(ps->ckrem), pErr);
      if (a < 0) {
        status = 0;
      }
      
      if (status) {
        vl = readChunkVar(pSrc, &(ps->ckrem), pErr);
        if (vl < 0) {
          status = 0;
        }
      }
      
      if (status) {
        for(vi = 0; vi < vl; vi++) {
          d = readChunkByte(pSrc, &(ps->ckrem), pErr);
          if (d < 0) {
            status = 0;
            break;
          }
          if (!pushBuffer(ps, d)) {
            status = 0;
            *pErr = SMF_ERR_BIG_PAYLOAD;
            break;
          }
        }
      }
      
    } else {
      /* Something invalid */
      status = 0;
      *pErr = SMF_ERR_BAD_EVENT;
    }
  }
  
  /* If we have a MIDI message status byte, update running status; else,
   * clear running status */
  if (status) {
    if ((c >= 0x80) && (c <= 0xef)) {
      ps->run = c;
      
    } else {
      ps->run = -1;
    }
  }
  
  /* For the End Of Track meta-event (type 2F), skip any remaining data
   * in the track */
  if (status && (a == 0x2f)) {
    if (!smfsource_skip(pSrc, ps->ckrem)) {
      status = 0;
      *pErr = SMF_ERR_IO;
    }
    if (status) {
      ps->ckrem = 0;
    }
  }
  
  /* Parse the event */
  if (status) {
    if (!parseEvent(ps, pEnt, c, a, b, delta, pErr)) {
      status = 0;
    }
  }
  
  /* Return status */
  return status;
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
 * smfparse_alloc function.
 */
SMFPARSE *smfparse_alloc(void) {
  SMFPARSE *ps = NULL;
  
  ps = (SMFPARSE *) calloc(1, sizeof(SMFPARSE));
  if (ps == NULL) {
    fault(__LINE__);
  }
  
  ps->status   = 0;
  ps->ckrem    = -1;
  ps->trkcount = 0;
  ps->blen     = 0;
  ps->bcap     = 0;
  ps->bptr     = NULL;
  ps->run      = -1;
  
  return ps;
}

/*
 * smfparse_free function.
 */
void smfparse_free(SMFPARSE *ps) {
  
  if (ps != NULL) {
    if (ps->bptr != NULL) {
      free(ps->bptr);
      ps->bptr = NULL;
    }
    free(ps);
    ps = NULL;
  }
}

/*
 * smfparse_read function.
 */
void smfparse_read(SMFPARSE *ps, SMF_ENTITY *pEnt, SMFSOURCE *pSrc) {
  
  int status = 1;
  int err_code = 0;
  
  uint32_t ck_type = 0;
  int32_t ck_len = 0;
  
  /* Check parameters */
  if ((ps == NULL) || (pEnt == NULL) || (pSrc == NULL)) {
    fault(__LINE__);
  }
  
  /* Reset entity structure */
  memset(pEnt, 0, sizeof(SMF_ENTITY));
  
  pEnt->status     = 0;
  pEnt->pHead      = NULL;
  pEnt->chunk_type = 0;
  pEnt->delta      = -1;
  pEnt->ch         = -1;
  pEnt->key        = -1;
  pEnt->ctl        = -1;
  pEnt->val        = -1;
  pEnt->bend       = 0;
  pEnt->buf_len    = 0;
  pEnt->buf_ptr    = NULL;
  pEnt->seq_num    = -1;
  pEnt->txtype     = -1;
  pEnt->beat_dur   = -1;
  pEnt->tcode      = NULL;
  pEnt->tsig       = NULL;
  pEnt->ksig       = NULL;
  pEnt->meta_type  = -1;
  
  /* Determine what to do */
  if (ps->status < 0) {
    /* We are in an error state, so just use that */
    status = 0;
    err_code = ps->status;
    
  } else if (ps->status == 0) {
    /* We are in initial state, so read the header chunk */
    if (!readHeaderChunk(&(ps->head), pSrc, &err_code)) {
      status = 0;
    }
    
    /* Change to header-read state */
    if (status) {
      ps->status = 1;
    }
    
    /* Copy parsed header into header return structure */
    if (status) {
      memcpy(&(ps->rHead), &(ps->head), sizeof(SMF_HEADER));
    }
    
    /* Set up header entity */
    if (status) {
      pEnt->status = SMF_TYPE_HEADER;
      pEnt->pHead  = &(ps->rHead);
    }
    
  } else if (ps->status == 2) {
    /* We are in EOF state, so just use that */
    pEnt->status = SMF_TYPE_EOF;
    
  } else if ((ps->status == 1) && (ps->ckrem < 0)) {
    /* We've read the header but are outside of any chunk -- check first
     * whether there's another declared track to read */
    if (ps->trkcount < (ps->head).nTracks) {
      /* More declared tracks remain to be read, so read a chunk
       * header */
      if (!readChunkHead(&ck_type, &ck_len, pSrc, &err_code)) {
        status = 0;
      }
      
      /* Check the chunk type */
      if (status) {
        if (ck_type == UINT32_C(0x4d54726b)) {
          /* Track chunk, so increase track count and load the track
           * length */
          (ps->trkcount)++;
          ps->ckrem = ck_len;
          
          /* Return BEGIN_TRACK entity and reset running status */
          pEnt->status = SMF_TYPE_BEGIN_TRACK;
          ps->run = -1;
          
        } else if (ck_type == UINT32_C(0x4d546864)) {
          /* Another MIDI header chunk, which shouldn't happen */
          status = 0;
          err_code = SMF_ERR_MULTI_HEAD;
          
        } else {
          /* Unrecognized chunk type, which is OK, so skip its data
           * payload and report it */
          if (!smfsource_skip(pSrc, ck_len)) {
            status = 0;
            err_code = SMF_ERR_IO;
          }
          
          if (status) {
            pEnt->status = SMF_TYPE_CHUNK;
            pEnt->chunk_type = ck_type;
          }
        }
      }
      
    } else {
      /* We've read all the declared tracks, so go to EOF state and
       * return EOF */
      ps->status = 2;
      pEnt->status = SMF_TYPE_EOF;
    }
    
  } else if ((ps->status == 1) && (ps->ckrem >= 0)) {
    /* We're inside a track, so read an event */
    if (!readEvent(ps, pEnt, pSrc, &err_code)) {
      status = 0;
    }
    
  } else {
    fault(__LINE__);
  }
  
  /* If status indicates failure, copy error code into entity, make sure
   * that entity status is negative, and copy into parser status */
  if (!status) {
    if (err_code >= 0) {
      fault(__LINE__);
    }
    
    pEnt->status = err_code;
    ps->status   = err_code;
  }
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
    
    case SMF_ERR_EOF:
      pResult = "Unexpected end of MIDI file";
      break;
    
    case SMF_ERR_HUGE_CHUNK:
      pResult = "MIDI file chunk is too large";
      break;
    
    case SMF_ERR_SIGNATURE:
      pResult = "MIDI file lacks correct file header signature";
      break;
    
    case SMF_ERR_HEADER:
      pResult = "MIDI file has invalid header chunk";
      break;
    
    case SMF_ERR_MIDI_FMT:
      pResult = "MIDI file has unrecognized format type";
      break;
    
    case SMF_ERR_NO_TRACKS:
      pResult = "MIDI file has no declared tracks";
      break;
    
    case SMF_ERR_MULTI_TRACK:
      pResult = "MIDI format 0 file can't have multiple tracks";
      break;
    
    case SMF_ERR_MULTI_HEAD:
      pResult = "Multiple MIDI header chunks";
      break;
    
    case SMF_ERR_OPEN_TRACK:
      pResult = "MIDI track ended without End Of Track event";
      break;
    
    case SMF_ERR_LONG_VARINT:
      pResult = "MIDI variable-length quantity is too large";
      break;
    
    case SMF_ERR_RUN_STATUS:
      pResult = "Missing status when using MIDI running status bytes";
      break;
    
    case SMF_ERR_BIG_PAYLOAD:
      pResult = "Data payload of MIDI event is too large";
      break;
    
    case SMF_ERR_BAD_EVENT:
      pResult = "Invalid MIDI event in track";
      break;
    
    case SMF_ERR_SEQ_NUM:
      pResult = "Invalid Sequence Number MIDI meta-event";
      break;
    
    case SMF_ERR_CH_PREFIX:
      pResult = "Invalid Channel Prefix MIDI meta-event";
      break;
    
    case SMF_ERR_BAD_EOT:
      pResult = "Invalid End Of Track MIDI meta-event";
      break;
    
    case SMF_ERR_SET_TEMPO:
      pResult = "Invalid Set Tempo MIDI meta-event";
      break;
    
    case SMF_ERR_SMPTE_OFF:
      pResult = "Invalid SMPTE Offset MIDI meta-event";
      break;
    
    case SMF_ERR_TIME_SIG:
      pResult = "Invalid Time Signature MIDI meta-event";
      break;
    
    case SMF_ERR_KEY_SIG:
      pResult = "Invalid Key Signature MIDI meta-event";
      break;
    
    case SMF_ERR_MIDI_DATA:
      pResult = "Invalid data bytes in MIDI message";
      break;
    
    default:
      pResult = "Unknown error";
  }
  
  return pResult;
}
