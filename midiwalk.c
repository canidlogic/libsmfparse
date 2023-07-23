/*
 * midiwalk.c
 * ==========
 * 
 * Dumps all the parsed information in a MIDI file into text format.
 * 
 * This is both a test program for the libsmfparse library and an
 * analysis tool for MIDI files.
 * 
 * Syntax
 * ------
 * 
 *   midiwalk < input.mid > output.txt
 *   midiwalk path/to/input.mid > output.txt
 * 
 * Requirements
 * ------------
 * 
 * Only requires libsmfparse.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smfparse.h"

/*
 * Diagnostics
 * ===========
 */

/*
 * The executable module name for diagnostic messages, or NULL if not
 * known.
 */
static const char *pModule = NULL;

/*
 * Report an error to standard error and exit the program with failure
 * code.
 * 
 * The message is a printf-style message, and it is followed by a
 * variable-length argument list of parameters.
 * 
 * Parameters:
 * 
 *   lnum - the source file line number (__LINE__)
 * 
 *   pMsg - the error message, or NULL for a generic message
 * 
 *   ... - extra parameters for the error message
 */
static void raiseErr(long lnum, const char *pMsg, ...) {
  
  va_list ap;
  va_start(ap, pMsg);
  
  /* Report module name */
  if (pModule != NULL) {
    fprintf(stderr, "%s: ", pModule);
  } else {
    fprintf(stderr, "midiwalk: ");
  }
  
  /* Report error and line number */
  if (lnum > 0) {
    fprintf(stderr, "[Error on source line %ld] ", lnum);
  } else {
    fprintf(stderr, "[Error]");
  }
  
  /* Report message */
  if (pMsg != NULL) {
    vfprintf(stderr, pMsg, ap);
  } else {
    fprintf(stderr, "Unexpected");
  }
  
  /* Finish report */
  fprintf(stderr, "\n");
  
  va_end(ap);
  exit(EXIT_FAILURE);
}

/*
 * Local functions
 * ===============
 */

/* Prototypes */
static void dumpBinary(SMF_ENTITY *pe);
static void dumpText(SMF_ENTITY *pe);

/*
 * Dump a binary payload as a sequence of space-separated base-16 pairs
 * all on a single line, with a leading space and nothing after the last
 * digit.
 * 
 * Parameters:
 * 
 *   pe - the entity containing the data buffer
 */
static void dumpBinary(SMF_ENTITY *pe) {
  int32_t i = 0;
  
  if (pe == NULL) {
    raiseErr(__LINE__, NULL);
  }
  
  for(i = 0; i < pe->buf_len; i++) {
    printf(" %02x", (int) (pe->buf_ptr)[i]);
  }
}

/*
 * Dump a text payload as ASCII characters, with backslash escaped as
 * two backslashes, and non-ASCII characters and control codes printed
 * as backslash followed by two base-16 digits, with nothing after the
 * string.
 * 
 * Parameters:
 * 
 *   pe - the entity containing the data buffer
 */
static void dumpText(SMF_ENTITY *pe) {
  int32_t i = 0;
  int c = 0;
  
  if (pe == NULL) {
    raiseErr(__LINE__, NULL);
  }
  
  for(i = 0; i < pe->buf_len; i++) {
    c = (pe->buf_ptr)[i];
    if ((c >= 0x20) && (c <= 0x7e) && (c != '\\')) {
      putchar(c);
      
    } else if (c == '\\') {
      printf("\\\\");
      
    } else {
      printf("\\%02x", c);
    }
  }
}

/*
 * Program entrypoint
 * ==================
 */

int main(int argc, char *argv[]) {
  
  int i = 0;
  const char *pPath = NULL;
  
  SMFSOURCE *pSrc = NULL;
  SMFPARSE *ps = NULL;
  SMF_ENTITY ent;
  int err_num = 0;
  
  int32_t offs = 0;
  int32_t tnum = 0;
  
  /* Initialize structures */
  memset(&ent, 0, sizeof(SMF_ENTITY));
  
  /* Get module name */
  if ((argc > 0) && (argv != NULL)) {
    pModule = argv[0];
  }
  
  /* Check parameters */
  if (argc > 0) {
    if (argv == NULL) {
      raiseErr(__LINE__, NULL);
    }
    for(i = 0; i < argc; i++) {
      if (argv[i] == NULL) {
        raiseErr(__LINE__, NULL);
      }
    }
  }
  
  /* Parse program arguments */
  if (argc == 2) {
    pPath = argv[1];
  } else if (argc < 2) {
    pPath = NULL;
  } else {
    raiseErr(__LINE__, "Wrong number of program arguments");
  }
  
  /* Open the input source */
  if (pPath != NULL) {
    pSrc = smfsource_new_path(pPath, &err_num);
    if (pSrc == NULL) {
      raiseErr(__LINE__, "Failed to open input: %s",
                smf_errorString(err_num));
    }
  } else {
    pSrc = smfsource_new_handle(stdin, 0, 0, &err_num);
    if (pSrc == NULL) {
      raiseErr(__LINE__, "Failed to open input: %s",
                smf_errorString(err_num));
    }
  }
  
  /* Allocate a parser */
  ps = smfparse_alloc();
  
  /* Iterate through all MIDI events */
  for(smfparse_read(ps, &ent, pSrc);
      ent.status > 0;
      smfparse_read(ps, &ent, pSrc)) {
    
    /* If not header, chunk, or begin track event, then update time
     * offset and display offset */
    if ((ent.status != SMF_TYPE_HEADER) &&
        (ent.status != SMF_TYPE_CHUNK) &&
        (ent.status != SMF_TYPE_BEGIN_TRACK)) {
      if (ent.delta <= INT32_MAX - offs) {
        offs += ent.delta;
      } else {
        raiseErr(__LINE__, "Time offset overflow");
      }
      printf("%08lx: ", (long) offs);
    }
    
    /* Display channel prefix if a channel-based event */
    if ((ent.status == SMF_TYPE_NOTE_OFF) ||
        (ent.status == SMF_TYPE_NOTE_ON) ||
        (ent.status == SMF_TYPE_KEY_AFTERTOUCH) ||
        (ent.status == SMF_TYPE_CONTROL) ||
        (ent.status == SMF_TYPE_PROGRAM) ||
        (ent.status == SMF_TYPE_CH_AFTERTOUCH) ||
        (ent.status == SMF_TYPE_PITCH_BEND) ||
        (ent.status == SMF_TYPE_CH_PREFIX)) {
      printf("[%2d] ", ent.ch + 1);
    }
    
    /* Handle the types of events */
    if (ent.status == SMF_TYPE_HEADER) {
      printf("MIDI Format %d with %ld track(s)\n",
                (ent.pHead)->fmt,
                (long) (ent.pHead)->nTracks);
      if (((ent.pHead)->ts).frame_rate == 0) {
        printf("Delta units per MIDI beat: %ld\n",
                (long) ((ent.pHead)->ts).subdiv);
        
      } else if (((ent.pHead)->ts).frame_rate == 29) {
        printf("SMPTE frame rate     :  29.97 (30 drop-frame)\n");
        printf("Delta units per frame:  %ld\n",
                  (long) ((ent.pHead)->ts).subdiv);
      
      } else {
        printf("SMPTE frame rate:  %d\n",
                ((ent.pHead)->ts).frame_rate);
        printf("Delta units per frame:  %ld\n",
                  (long) ((ent.pHead)->ts).subdiv);
      }
      printf("\n");
      
    } else if (ent.status == SMF_TYPE_CHUNK) {
      printf("FOREIGN CHUNK with ID %08lX\n\n",
              (unsigned long) ent.chunk_type);
      
    } else if (ent.status == SMF_TYPE_BEGIN_TRACK) {
      tnum++;
      offs = 0;
      printf("BEGIN TRACK %ld\n\n", (long) tnum);
      
    } else if (ent.status == SMF_TYPE_END_TRACK) {
      printf("END TRACK\n\n");
      
    } else if (ent.status == SMF_TYPE_NOTE_OFF) {
      printf("Note-Off K:%3d V:%3d\n",
              (int) ent.key, (int) ent.val);
      
    } else if (ent.status == SMF_TYPE_NOTE_ON) {
      printf("Note-On  K:%3d V:%3d\n",
              (int) ent.key, (int) ent.val);
      
    } else if (ent.status == SMF_TYPE_KEY_AFTERTOUCH) {
      printf("Pressure K:%3d V:%3d\n",
              (int) ent.key, (int) ent.val);
      
    } else if (ent.status == SMF_TYPE_CONTROL) {
      printf("Control  C:%3d V:%3d\n",
                (int) ent.ctl, (int) ent.val);
      
    } else if (ent.status == SMF_TYPE_PROGRAM) {
      printf("Program  P:%3d\n", (int) ent.val);
      
    } else if (ent.status == SMF_TYPE_CH_AFTERTOUCH) {
      printf("Pressure V:%3d\n", (int) ent.val);
      
    } else if (ent.status == SMF_TYPE_PITCH_BEND) {
      printf("Pitch %+d\n", (int) ent.bend);
      
    } else if (ent.status == SMF_TYPE_SYSEX) {
      printf("SYSEX (F0)");
      dumpBinary(&ent);
      printf("\n");
      
    } else if (ent.status == SMF_TYPE_SYSESC) {
      printf("SYSEX-ESC");
      dumpBinary(&ent);
      printf("\n");
      
    } else if (ent.status == SMF_TYPE_SEQ_NUM) {
      printf("Sequence ID %ld\n", (long) ent.seq_num);
      
    } else if (ent.status == SMF_TYPE_TEXT) {
      if (ent.txtype == SMF_TEXT_GENERAL) {
        printf("[Text] ");
        
      } else if (ent.txtype == SMF_TEXT_COPYRIGHT) {
        printf("[Copyright] ");
        
      } else if (ent.txtype == SMF_TEXT_TITLE) {
        printf("[Title] ");
        
      } else if (ent.txtype == SMF_TEXT_INSTRUMENT) {
        printf("[Instrument] ");
        
      } else if (ent.txtype == SMF_TEXT_LYRIC) {
        printf("[Lyric] ");
        
      } else if (ent.txtype == SMF_TEXT_MARKER) {
        printf("[Marker] ");
        
      } else if (ent.txtype == SMF_TEXT_CUE) {
        printf("[Cue] ");
        
      } else {
        raiseErr(__LINE__, "Unrecognized text type");
      }
      dumpText(&ent);
      printf("\n");
      
    } else if (ent.status == SMF_TYPE_CH_PREFIX) {
      printf("Meta Channel Prefix\n");
      
    } else if (ent.status == SMF_TYPE_TEMPO) {
      printf("Tempo %ld (%.1f bpm)\n",
        (long) ent.beat_dur,
        (double) (60000000.0 / ((double) ent.beat_dur))
      );
      
    } else if (ent.status == SMF_TYPE_SMPTE) {
      printf("SMPTE Offset %02d:%02d:%02d:%02d.%02d\n",
                (int) (ent.tcode)->hour,
                (int) (ent.tcode)->minute,
                (int) (ent.tcode)->second,
                (int) (ent.tcode)->frame,
                (int) (ent.tcode)->ff);
      
    } else if (ent.status == SMF_TYPE_TIME_SIG) {
      printf("Time Signature %d / %d (click %d) (beat %d)\n",
                (ent.tsig)->numerator,
                (ent.tsig)->denominator,
                (ent.tsig)->click,
                (ent.tsig)->beat_unit);
      
    } else if (ent.status == SMF_TYPE_KEY_SIG) {
      printf("Key Signature ");
      if ((ent.ksig)->key < 0) {
        printf("%d flats, ", (0 - (ent.ksig)->key));
        
      } else if ((ent.ksig)->key > 0) {
        printf("%d sharps, ", (ent.ksig)->key);
        
      } else if ((ent.ksig)->key == 0) {
        printf("0 sharps/flats, ");
        
      } else {
        raiseErr(__LINE__, NULL);
      }
      
      if ((ent.ksig)->is_minor) {
        printf("minor\n");
      } else {
        printf("major\n");
      }
      
    } else if (ent.status == SMF_TYPE_META) {
      printf("Custom Meta [%02x]", ent.meta_type);
      dumpBinary(&ent);
      printf("\n");
      
    } else {
      raiseErr(__LINE__, "Unexpected MIDI event type");
    }
  }
  
  /* Check whether we stopped on EOF or error */
  if (ent.status == SMF_TYPE_EOF) {
    printf("EOF\n");
  } else {
    raiseErr(__LINE__, "MIDI parsing error: %s",
              smf_errorString(ent.status));
  }
  
  /* Close the parser */
  smfparse_free(ps);
  ps = NULL;
  
  /* Close the input source */
  if (!smfsource_close(pSrc)) {
    raiseErr(__LINE__, "Failed to close input");
  }
  pSrc = NULL;
  
  /* If we got here, return successfully */
  return EXIT_SUCCESS;
}
