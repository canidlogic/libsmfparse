#ifndef SMFPARSE_H_INCLUDED
#define SMFPARSE_H_INCLUDED

/*
 * smfparse.h
 * ==========
 * 
 * Standard MIDI File (.MID) parsing library.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Constants
 * =========
 */

/*
 * The maximum value of a variable-length integer in an SMF file.
 * 
 * The minimum value is zero.
 */
#define SMF_MAX_VARINT INT32_C(0x0FFFFFFF)

/*
 * The maximum value that can be used within a MIDI data byte.
 */
#define SMF_MAX_DATA (127)

/*
 * The maximum value for an 8-bit byte.
 */
#define SMF_MAX_BYTE (255)

/*
 * Pitch bend constants.
 * 
 * Valid pitch bend values are in range SMF_MIN_BEND to SMF_MAX_BEND,
 * inclusive.
 */
#define SMF_MIN_BEND (-8192)
#define SMF_MAX_BEND ( 8191)

/*
 * The maximum sequence number that can be used in Sequence Number
 * meta-events.
 * 
 * The minimum sequence number is zero.
 */
#define SMF_MAX_SEQ_NUM INT32_C(0xffff)

/*
 * The maximum number of microseconds per beat ("MIDI Quarter Note")
 * that can be set in Set Tempo events.
 * 
 * The minimum value is one.
 */
#define SMF_MAX_BEAT INT32_C(0xffffff)

/*
 * The maximum denominator for a time signature.
 * 
 * This must be a power of two.
 * 
 * The minimum value for a denominator is 1.
 */
#define SMF_MAX_TIME_DENOM (1024)

/*
 * The minimum and maximum values that can be used in the key field of a
 * key signature structure.
 */
#define SMF_MIN_KEYSIG (-7)
#define SMF_MAX_KEYSIG ( 7)

/*
 * Special return values for SMFSOURCE callbacks.
 */
#define SMFSOURCE_EOF   (-1)
#define SMFSOURCE_IOERR (-2)

/*
 * SMF entity type constants.
 * 
 * The first entity in a MIDI file is always SMF_TYPE_HEADER.  This is
 * the only place where the SMF_TYPE_HEADER entity occurs.  The last
 * entity in a MIDI file is always SMF_TYPE_EOF.
 * 
 * SMF_TYPE_CHUNK, SMF_TYPE_BEGIN_TRACK, and SMF_TYPE_EOF only occur
 * when no track is currently open.  SMF_TYPE_BEGIN_TRACK opens a track.
 * 
 * All other entities only occur when a track is open.  The entity
 * SMF_TYPE_END_TRACK will close a track.  No track will be open at the
 * end of parsing.
 */
#define SMF_TYPE_EOF            ( 0)  /* End Of File */
#define SMF_TYPE_HEADER         ( 1)  /* MIDI file header chunk */
#define SMF_TYPE_CHUNK          ( 2)  /* Unrecognized data chunk */
#define SMF_TYPE_BEGIN_TRACK    ( 3)  /* Start of track chunk */
#define SMF_TYPE_END_TRACK      ( 4)  /* End Of Track meta-event */
#define SMF_TYPE_NOTE_OFF       ( 5)  /* Note-Off message */
#define SMF_TYPE_NOTE_ON        ( 6)  /* Note-On message */
#define SMF_TYPE_KEY_AFTERTOUCH ( 7)  /* Key pressure message */
#define SMF_TYPE_CONTROL        ( 8)  /* Control change message */
#define SMF_TYPE_PROGRAM        ( 9)  /* Program change message */
#define SMF_TYPE_CH_AFTERTOUCH  (10)  /* Channel pressure message */
#define SMF_TYPE_PITCH_BEND     (11)  /* Pitch bend message */
#define SMF_TYPE_SYSEX          (12)  /* System-Exclusive F0 event */
#define SMF_TYPE_SYSESC         (13)  /* System-Exclusive F7 escape */
#define SMF_TYPE_SEQ_NUM        (14)  /* Sequence Number meta-event */
#define SMF_TYPE_TEXT           (15)  /* Text-type meta-event */
#define SMF_TYPE_CH_PREFIX      (16)  /* Channel prefix meta-event */
#define SMF_TYPE_TEMPO          (17)  /* Set Tempo meta-event */
#define SMF_TYPE_SMPTE          (18)  /* SMPTE Offset meta-event */
#define SMF_TYPE_TIME_SIG       (19)  /* Time Signature meta-event */
#define SMF_TYPE_KEY_SIG        (20)  /* Key Signature meta-event */
#define SMF_TYPE_META           (21)  /* Special FF-7F meta-event */

/*
 * SMF text entity subclass constants.
 * 
 * These are used for SMF_TYPE_TEXT entities to distinguish the specific
 * purpose of the text event.
 * 
 * The constant values here match the meta-event IDs of the text event
 * in the MIDI file.
 * 
 * The SMF_TEXT_GENERAL subclass can be used for any type of text
 * whatsoever.
 * 
 * The SMF_TEXT_COPYRIGHT subclass is for a copyright notice, which it
 * is recommended to place at time zero near the start of the first
 * track.
 * 
 * The SMF_TEXT_TITLE subclass provides a title for the whole MIDI file
 * when in the first track of the file.  When not in the first track of
 * the file, it provides a title for that specific track.
 * 
 * The SMF_TEXT_INSTRUMENT subclass is used for a textual description of
 * a synthesized instrument.  To associate this with a specific channel,
 * either include the channel within the text or precede the event with
 * an SMF_TYPE_CH_PREFIX meta-event.
 * 
 * The SMF_TEXT_LYRIC subclass is used for storing timed lyric
 * syllables.
 * 
 * The SMF_TEXT_MARKER subclass is intended for timed rehearsal letters
 * and other such synchronization markers.  It is recommended that these
 * events occur in the first track of the file.
 * 
 * The SMF_TEXT_CUE subclass is intended for timed textual descriptions
 * of extra-musical cues, such as something happening on-screen in a
 * movie if this is a movie soundtrack.
 */
#define SMF_TEXT_GENERAL    (1)
#define SMF_TEXT_COPYRIGHT  (2)
#define SMF_TEXT_TITLE      (3)
#define SMF_TEXT_INSTRUMENT (4)
#define SMF_TEXT_LYRIC      (5)
#define SMF_TEXT_MARKER     (6)
#define SMF_TEXT_CUE        (7)

/*
 * Type declarations
 * =================
 */

/*
 * SMFSOURCE structure prototype.
 * 
 * Structure definition given in implementation file.
 */
struct SMFSOURCE_TAG;
typedef struct SMFSOURCE_TAG SMFSOURCE;

/*
 * SMFPARSE structure prototype.
 * 
 * Structure definition given in implementation file.
 */
struct SMFPARSE_TAG
typedef struct SMFPARSE_TAG SMFPARSE;

/*
 * SMF_TIMESYS structure representing the time system used within a MIDI
 * file.
 */
typedef struct {
  
  /*
   * Number of times the delta time unit subdivides the main time unit.
   * 
   * If the frame_rate field is zero, then subdiv indicates how many
   * delta time units there are per beat ("MIDI quarter note").  The
   * length of delta time units then varies depending on the current
   * tempo, which sets the length of the beat in microseconds with Set
   * Tempo meta-events.
   * 
   * If the frame_rate field is non-zero, then subdiv indicates how many
   * delta time units there are per frame at the specified frame rate.
   * The delta time units are independent from tempo in this SMPTE
   * timing system.  Delta time units are constant for SMPTE timing.
   * 
   * Range is 1 to 32767 (inclusive) if frame_rate is set to zero, or
   * else 1 to 127 (inclusive) for SMPTE timing when frame_rate is
   * non-zero.
   */
  int32_t subdiv;
  
  /*
   * The frame rate for SMPTE timing.
   * 
   * The valid values are 24, 25, 29(*), or 30 frames per second.  Or,
   * if not  using SMPTE timing, this field is set to zero.
   * 
   * (*) - The value of 29 does NOT mean 29 frames per second.  Instead,
   * it selects a rate that is exactly equal to:
   * 
   *        1000
   *   30 * ---- Hz (= approx. 29.97 Hz)
   *        1001
   * 
   * For MIDI and SMPTE time codes, "drop-frame" timecodes are used.
   * This means that the timecode is given as if the frame rate were
   * exactly 30 Hz.  However, when the minute field of the timecode is
   * neither zero nor divisible by 10, the timecodes for the first two
   * frames of that minute are skipped.  This has the effect of slowing
   * down the average frame rate of the timecode sequence to almost
   * exactly the (30 * 1000 / 1001) Hz rate.
   * 
   * Note that "drop-frame" does NOT mean that frames are dropped.  It
   * merely means that certain timecodes are dropped.  If you are not
   * using timecodes, you can ignore the drop-frame scheme and just use
   * (30 * 1000 / 1001) as the frame rate.
   */
  int frame_rate;
  
} SMF_TIMESYS;

/*
 * SMF_HEADER structure representing the information parsed from a MIDI
 * header track.
 */
typedef struct {
  
  /*
   * The type of MIDI file.
   * 
   * Format 0 has a single track.
   * 
   * Format 1 has multiple tracks that proceed simultaneously.
   * 
   * Format 2 has multiple tracks that are independent of each other in
   * time.  This format is rarely used.
   */
  int fmt;
  
  /*
   * The declared number of tracks in the file.
   * 
   * This does not necessarily match the actual number of tracks in the
   * file.  The value must be 1 if the format is 0.
   */
  int32_t nTracks;
  
  /*
   * The time system declared in the MIDI file header.
   */
  SMF_TIMESYS ts;
  
} SMF_HEADER;

/*
 * SMF_TIMECODE structure for storing an SMPTE timecode.
 * 
 * This stores an hour:minute:second:frame of an SMPTE time.  This also
 * stores a fractional frame value ff.  The fractional frame value is
 * always in units of 1/100 of a frame, even if the MIDI file is using
 * SMPTE timing with some other division of the frame.
 */
typedef struct {
  
  /* Hour always in range 0-23, with wrap-around */
  uint8_t hour;
  
  /* Minute always in range 0-59 */
  uint8_t minute;
  
  /* Second always in range 0-59 (no leap seconds) */
  uint8_t second;
  
  /* 
   * Frame always in range 0-29.
   * 
   * If the MIDI file is using SMPTE timing, the parser will verify that
   * this does not exceed the maximum frame number for the selected
   * frame rate.
   * 
   * For the ~29.97 Hz frame rate, 30 drop-frame is used, so that the
   * valid range is 0-29.  Furthermore, the parser will verify that if
   * the minute field is zero or divisible by 10, then the frame number
   * is neither 0 nor 1, because those timecodes are skipped in
   * drop-frame.
   */
  uint8_t frame;
  
  /* Fractional frame in range 0-99 */
  uint8_t ff;
  
} SMF_TIMECODE;

/*
 * SMF_TIMESIG structure for storing time signature information.
 */
typedef struct {
  
  /*
   * The numerator of the notated time signature.
   * 
   * This is in range 1 to SMF_MAX_BYTE, inclusive.
   */
  int numerator;
  
  /*
   * The denominator of the notated time signature.
   * 
   * This is in range 1 to SMF_MAX_TIME_DENOM, and it will furthermore
   * always be a power of two.
   */
  int denominator;
  
  /*
   * The number of MIDI clock pulses per metronome click.
   * 
   * The beat ("MIDI quarter note") is always subdivided into 24 MIDI
   * clock pulses.  The metronome click frequency is therefore specified
   * in units of 1/24 of a beat.  A value of 24 for the click field
   * means the metronome should click every beat.
   * 
   * This is in range 1 to SMF_MAX_BYTE, inclusive.
   */
  int click;
  
  /*
   * The notated rhythmic unit that corresponds to a beat ("MIDI quarter
   * note").
   * 
   * This is expressed in units of 32nd-notes.  For example, if the beat
   * ("MIDI quarter note") corresponds to a notated dotted quarter note,
   * then this field will be 12.
   * 
   * This is in range 1 to SMF_MAX_BYTE, inclusive.
   */
  int beat_unit;
  
} SMF_TIMESIG;

/*
 * SMF_KEYSIG structure for storing key signature information.
 */
typedef struct {
  
  /*
   * The key expressed as a count of accidentals.
   * 
   * Positive values count the number of sharps in the key signature
   * while negative values count the number of flats in the key
   * signature.
   * 
   * The key of zero is C major or A minor (no accidentals)
   * 
   * The key of -1 is F major or D minor (one flat)
   * 
   * The key of +2 is D major or B minor (two sharps)
   * 
   */
  int key;
  
} SMF_KEYSIG;

/*
 * SMF_ENTITY structure for storing the results of a read operation from
 * the parser.
 */
typedef struct {
  
  /*
   * If the status is zero or greater, it indicates that the parse
   * operation succeeded.  If the status is negative, it indicates that
   * the parse operation failed.
   * 
   * If less than zero, the status will equal one of the SMF_ERR_
   * constants defining various error codes.  The smf_errorString()
   * function can be used to get an error message string from the error
   * code.  All other fields of this structure should be ignored if
   * there is an error status.
   * 
   * If zero or greater, the status will equal one of the SMF_TYPE_
   * constants defining various entity types.
   */
  int status;
  
  /*
   * Pointer to a structure representing a parsed MIDI header chunk.
   * 
   * This is only used for an SMF_TYPE_HEADER entity.  It is NULL in all
   * other cases.
   * 
   * If non-NULL, the structure will be owned by the parser object.  It
   * remains valid until the next call to read an entity, or until the
   * parser object is freed (whichever occurs first).
   */
  SMF_HEADER *pHead;
  
  /*
   * The 32-bit chunk type, with the most significant byte being the
   * first character of the chunk type.
   * 
   * This is only used for an SMF_TYPE_CHUNK entity, which is used for
   * chunks after the header chunk which are not recognized and skipped.
   * It never has the values for MThd or MTrk chunks, which are
   * recognized and parsed.
   * 
   * For all other entities, this is ignored and set to zero.
   */
  uint32_t chunk_type;
  
  /*
   * The delta time offset before this entity.
   * 
   * This is used for every entity except SMF_TYPE_HEADER,
   * SMF_TYPE_CHUNK, and SMF_TYPE_BEGIN_TRACK.  Note that
   * SMF_TYPE_END_TRACK *does* have a delta time offset, because the
   * End Of Track is an actual meta-event within the track chunk.
   * 
   * If present, the delta time is in range zero up to SMF_MAX_VARINT,
   * inclusive.  For the entities that do not have a delta, this field
   * value is set to -1 and ignored.
   */
  int32_t delta;
  
  /*
   * The MIDI channel number associated with this entity.
   * 
   * This is used with the MIDI message entities:
   * 
   *   - SMF_TYPE_NOTE_OFF
   *   - SMF_TYPE_NOTE_ON
   *   - SMF_TYPE_KEY_AFTERTOUCH
   *   - SMF_TYPE_CONTROL
   *   - SMF_TYPE_PROGRAM
   *   - SMF_TYPE_CH_AFTERTOUCH
   *   - SMF_TYPE_PITCH_BEND
   * 
   * It is also used with the Channel Prefix meta-event:
   * 
   *   - SMF_TYPE_CH_PREFIX
   * 
   * For these entity types, the channel values will be in range 0 to
   * 15 inclusive.  Note that many MIDI systems refer to channels with
   * 1-based offsets.  In such cases, the ch value zero corresponds to
   * channel 1, the ch value one corresponds to channel 2, and so forth.
   * 
   * For other entity types, this is set to -1 and ignored.
   */
  int ch;
  
  /*
   * The MIDI key number associated with this entity.
   * 
   * This is used for SMF_TYPE_NOTE_OFF, SMF_TYPE_NOTE_ON, and
   * SMF_TYPE_KEY_AFTERTOUCH entities.
   * 
   * The range is zero up to SMF_MAX_DATA, inclusive.  The decimal value
   * 60 corresponds to middle C (C4) on a piano keyboard, and one unit
   * on this scale is a semitone in equal temperament.
   * 
   * If not used, this field is set to -1 and ignored.
   */
  int key;
  
  /*
   * The controller index of this entity.
   * 
   * This is only used for the SMF_TYPE_CONTROL entity.  All other
   * entities set this to -1 and ignore it.
   * 
   * Controllers are things like the sustain pedal of a keyboard.  The
   * MIDI specification has a list of controllers.  Certain controller
   * indices have a special interpretation for setting Channel Modes.
   * 
   * Note, however, that the pitch bend wheel has its own entity type.
   */
  int ctl;
  
  /*
   * The "value" of this entity.
   * 
   * For the following entities, the value is defined and refers to a
   * velocity or pressure intensity:
   * 
   *   - SMF_TYPE_NOTE_OFF
   *   - SMF_TYPE_NOTE_ON
   *   - SMF_TYPE_KEY_AFTERTOUCH
   *   - SMF_TYPE_CH_AFTERTOUCH
   * 
   * For SMF_TYPE_NOTE_ON, the value of zero has the special
   * interpretation of releasing the key.  All other values are key-down
   * velocities.
   * 
   * For SMF_TYPE_NOTE_OFF, the value is the velocity of the note
   * release.
   * 
   * For the following entity, the value is defined and is the data
   * value to send to the controller:
   * 
   *   - SMF_TYPE_CONTROL
   * 
   * For the following entity, the value is defined and is the program
   * number to set:
   * 
   *   - SMF_TYPE_PROGRAM
   * 
   * When the value is defined, it is always in range zero up to
   * SMF_MAX_DATA, inclusive.
   * 
   * If not used, this field is set to -1 and ignored.
   */
  int val;
  
  /*
   * The pitch bend setting of this entity.
   * 
   * This is only used for SMF_TYPE_PITCH_BEND.  For all other entities,
   * it is set to zero and ignored.
   * 
   * The valid range is SMF_MIN_BEND to SMF_MAX_BEND, inclusive.
   */
  int bend;
  
  /*
   * The buffer fields.
   * 
   * These are used for the following entity types to store the data
   * payload:
   * 
   *   - SMF_TYPE_SYSEX
   *   - SMF_TYPE_SYSESC
   *   - SMF_TYPE_TEXT
   *   - SMF_TYPE_META
   * 
   * For SMF_TYPE_SYSEX and SMF_TYPE_SYSESC, the data payload represents
   * data bytes that should be transmitted.  For SMF_TYPE_SYSEX, the
   * data payload should be preceded by an 0xF0 byte.  On the other
   * hand, for SMF_TYPE_SYSESC, the data payload should be transmitted
   * *without* an 0xF0 byte before it.
   * 
   * The SMF_TYPE_SYSEX entity is used for complete System-Exclusive
   * messages.  The last byte of the data payload is supposed to be 0xF7
   * in this case, but this is not guaranteed.
   * 
   * For System-Exclusive messages that must be broken up into multiple
   * packets, the first packet should use SMF_TYPE_SYSEX, and all
   * subsequent packets should use SMF_TYPE_SYSESC.  The very last byte
   * of the last SMF_TYPE_SYSESC packet is supposed to be 0xF7 in this
   * case, but this is not guaranteed.
   * 
   * The SMF_TYPE_SYSESC entity can also be used by itself to transmit
   * other kinds of MIDI messages that would not ordinarily be allowed
   * within a MIDI file, such as realtime messages.
   * 
   * For SMF_TYPE_TEXT entities, the data payload represents the text
   * string.  This is usually an ASCII string, but there are no
   * guarantees about the encoding of the text.  In particular, the text
   * might contain nul bytes, so it is NOT generally safe to assume the
   * string is nul-terminated.
   * 
   * For SMF_TYPE_META entities, the data payload is the custom data
   * associated with the entity.
   * 
   * When in use, buf_len will be the length in bytes of the data in the
   * buffer.  This may be zero.  If it is greater than zero, buf_ptr
   * must be non-NULL.  buf_len is the only way to determine the length
   * of the data.
   * 
   * buf_ptr points to the actual data buffer.  However, it may be set
   * to NULL if buf_len is zero.
   * 
   * If non-NULL, the data buffer will be owned by the parser object.
   * It remains valid until the next call to read an entity, or until
   * the parser object is freed (whichever occurs first).
   * 
   * For entities that do not use the data buffer, the length will be
   * set to zero and the pointer will be set to NULL.
   */
  int32_t buf_len;
  uint8_t *buf_ptr;
  
  /*
   * The sequence number.
   * 
   * This is used with SMF_TYPE_SEQ_NUM entities to identify the
   * specific sequence.
   * 
   * The valid range is zero to SMF_MAX_SEQ_NUM, inclusive.  For other
   * entity types, this is set to -1 and ignored.
   * 
   * Format 0 and format 1 MIDI files are supposed to have at most one
   * Sequence Number event in their first track, which identifies the
   * sequence number of the whole file, though the limitation of at most
   * one such event is not enforced by this parser library.
   * 
   * Format 2 MIDI files are supposed to use a Sequence Number event at
   * the start of the track to identify the specific track, with track
   * numbers used as identifiers for individual sequences if the
   * Sequence Number events are not present.
   */
  int32_t seq_num;
  
  /*
   * The text subclass.
   * 
   * This is used with SMF_TYPE_TEXT entities to specify the function of
   * the text.  It is one of the SMF_TEXT_ constants.
   * 
   * For all other entities, it is set to -1 and ignored.
   */
  int txtype;
  
  /*
   * The duration of a beat ("MIDI quarter note"), measured in
   * microseconds.
   * 
   * This is used with SMF_TYPE_TEMPO entities to specify the tempo.  It
   * will be in range 1 to SMF_MAX_BEAT, inclusive.
   * 
   * For all other entities, it is set to -1 and ignored.
   */
  int32_t beat_dur;
  
  /*
   * The SMPTE timecode structure that specifies the time offset.
   * 
   * This is used with SMF_TYPE_SMPTE.  For all other entities, it is
   * set to NULL and ignored.
   * 
   * If non-NULL, the structure will be owned by the parser object.  It
   * remains valid until the next call to read an entity, or until the
   * parser object is freed (whichever occurs first).
   */
  SMF_TIMECODE *tcode;
  
  /*
   * The time signature structure describing the event.
   * 
   * This is used with SMF_TYPE_TIME_SIG.  For all other entities, it is
   * set to NULL and ignored.
   * 
   * If non-NULL, the structure will be owned by the parser object.  It
   * remains valid until the next call to read an entity, or until the
   * parser object is freed (whichever occurs first).
   */
  SMF_TIMESIG *tsig;
  
  /*
   * The key signature structure describing the event.
   * 
   * This is used with SMF_TYPE_KEY_SIG.  For all other entities, it is
   * set to NULL and ignored.
   * 
   * If non-NULL, the structure will be owned by the parser object.  It
   * remains valid until the next call to read an entity, or until the
   * parser object is freed (whichever occurs first).
   */
  SMF_KEYSIG *ksig;
  
} SMF_ENTITY;

/*
 * Function pointer types
 * ======================
 */

/*
 * Callback function pointer type for a fault handler.
 * 
 * This function must not return.  The recommended implementation is to
 * display an error message and then exit the program.
 * 
 * This function is NOT used for error conditions like I/O errors or
 * MIDI file syntax errors.  Rather, this is used for error conditions
 * that should never arise in a correctly written program.
 * 
 * The passed lnum is the __LINE__ within the smfparse.h module where
 * the fault occurred.
 * 
 * Parameters:
 * 
 *   lnum - the line number of the fault (__LINE__)
 */
typedef void (*smf_fp_fault)(long lnum);

/*
 * Callback function pointer type for SMFSOURCE read functions.
 * 
 * This function shall read a single byte from the input source and
 * return the unsigned byte value, which must be in range 0 to 255
 * (inclusive).
 * 
 * If there are no bytes left to read, SMFSOURCE_EOF shall be returned.
 * The callback will not be invoked again unless the input source is
 * rewound.
 * 
 * If there is an error reading from the source, SMFSOURCE_IOERR shall
 * be returned.  No further callbacks will be made except to the rewind
 * and close callbacks.  If a rewind completes successfully, the input
 * source is returned to a non-error state.
 * 
 * The pInstance parameter is passed through from the constructor of the
 * SMFSOURCE object.
 * 
 * Parameters:
 * 
 *   pInstance - the passed-through instance pointer
 * 
 * Return:
 * 
 *   the unsigned byte value read from the input source, in range 0 to
 *   255, or one of the special conditions SMFSOURCE_EOF or
 *   SMFSOURCE_IOERR
 */
typedef int (*smfsource_fp_read)(void *pInstance);

/*
 * Callback function pointer type for SMFSOURCE rewind functions.
 * 
 * Not all SMFSOURCE objects have to support this callback.  Only input
 * sources that support rewinding.
 * 
 * If successful, the input source shall be returned to the beginning of
 * input so that the input can be read through again.  Any I/O error
 * conditions must be cleared.
 * 
 * If unsuccessful, the input source is in an error state.  The only
 * callback that may still be made in this state is to the close
 * callback.
 * 
 * The pInstance parameter is passed through from the constructor of the
 * SMFSOURCE object.
 * 
 * Parameters:
 * 
 *   pInstance - the passed-through instance pointer
 * 
 * Return:
 * 
 *   non-zero if successful, zero if failure
 */
typedef int (*smfsource_fp_rewind)(void *pInstance);

/*
 * Callback function pointer type for SMFSOURCE close functions.
 * 
 * Not all SMFSOURCE objects have to support this callback.  Only input
 * sources that need to clean up while closing down.
 * 
 * This function must always close down the input source.  The return
 * value indicates whether something went wrong during close down, which
 * a caller might then choose to warn the user about.  However,
 * returning failure does NOT mean that the input source remains open.
 * The input source is always closed down by this function no matter
 * what.
 * 
 * The pInstance parameter is passed through from the constructor of the
 * SMFSOURCE object.  If it points to a dynamically allocated memory
 * block, you should free it with this callback.
 * 
 * Parameters:
 * 
 *   pInstance - the passed-through instance pointer
 * 
 * Return:
 * 
 *   non-zero if normal close-down, zero if closed down but something
 *   was abnormal during the close-down process
 */
typedef int (*smfsource_fp_close)(void *pInstance);

/*
 * Callback function pointer type for SMFSOURCE skip functions.
 * 
 * Not all SMFSOURCE objects have to support this callback.  Only input
 * sources that have some random-access method for efficiently skipping
 * the file pointer ahead.  If not provided, skip operations will be
 * implemented by calling the read function repeatedly and discarding
 * what it returns.  That is, of course, much less efficient than having
 * a random-access seek-ahead.
 * 
 * The skip value will always be greater than zero.  If the skip
 * distance would go beyond the end of the file, the source object
 * should position the file pointer so that the next read will return
 * SMFSOURCE_EOF.
 * 
 * If this function returns failure, no further callbacks will be made
 * except to the rewind and close callbacks.  If rewind completes
 * successfully, the input source will be returned to a non-error state.
 * 
 * The pInstance parameter is passed through from the constructor of the
 * SMFSOURCE object.
 * 
 * Parameters:
 * 
 *   pInstance - the passed-through instance pointer
 * 
 *   skip - the number of bytes to skip ahead
 * 
 * Return:
 * 
 *   non-zero if successful, zero if failure
 */
typedef int (*smfsource_fp_skip)(void *pInstance, int32_t skip);

/*
 * Public functions
 * ================
 */

/*
 * Set a fault handler.
 * 
 * If a non-NULL value is passed, any currently registered fault handler
 * is overwritten with the new fault handler.  If a NULL value is
 * passed, any currently registered fault handler is uninstalled.
 * 
 * The default fault handler that is used if a fault occurs but no fault
 * handler is installed will write an error message to standard error
 * and then exit the program with EXIT_FAILURE.  If you want a different
 * behavior, install a custom fault handler.
 * 
 * Fault handlers must never return to the caller.  Undefined behavior
 * occurs if they do.
 * 
 * See the function pointer type documentation for further information.
 * 
 * Parameters:
 * 
 *   fFault - the fault handler to install, or NULL
 */
void smf_set_fault(smf_fp_fault fFault);

/*
 * Create a custom SMFSOURCE object.
 * 
 * For most common cases, you can use one of the pre-made input source
 * types, which use smfsource_new_ constructor functions.  If none of
 * the pre-made input sources will work for you, then you can use this
 * custom constructor to define your own input source.
 * 
 * pInstance can have any value, including NULL.  It will always be
 * passed through as-is when invoking the callback functions.  If this
 * points to a dynamically allocated memory block, it's a good idea to
 * define an fClose destructor that will release the memory block and
 * any resources owned by it.
 * 
 * fRead is the only callback that is required to be defined with a
 * non-NULL value.  It allows bytes to be read from the input source in
 * sequential order.
 * 
 * fRewind should only be defined for input sources that support
 * rewinding back to the beginning.  For input sources that do not
 * support this, pass NULL.
 * 
 * fClose is a destructor routine that is invoked when the input source
 * is released.  You can pass NULL if you do not need a destructor
 * routine.
 * 
 * fSkip is a callback for skipping ahead by a given number of bytes.
 * It should only be defined for input sources that support random
 * access or fast skips.  For input sources that do not support this,
 * pass NULL.  If no skip callback is provided, skips will be simulated
 * by repeatedly invoking the fRead callback.
 * 
 * The returned object should eventually be freed with
 * smfsource_close().
 * 
 * The object starts out without any error state.
 * 
 * Parameters:
 * 
 *   pInstance - value passed through to all callbacks
 * 
 *   fRead - the required read function callback
 * 
 *   fRewind - the rewind function callback or NULL
 * 
 *   fClose - the close function callback or NULL
 * 
 *   fSkip - the skip function callback or NULL
 * 
 * Return:
 * 
 *   the new input source object
 */
SMFSOURCE *smfsource_custom(
    void                * pInstance,
    smfsource_fp_read     fRead,
    smfsource_fp_rewind   fRewind,
    smfsource_fp_close    fClose,
    smfsource_fp_skip     fSkip);

/*
 * Construct an SMFSOURCE object around an open file handle.
 * 
 * pIn is the file handle, which should be open for reading.  You should
 * not use the file handle while an SMFSOURCE object is open around it.
 * 
 * is_owner is non-zero if the SMFSOURCE object should act like the
 * owner of the file handle.  Specifically, this means that the file
 * handle will be closed when the SMFSOURCE object is closed.  You
 * should NOT specify is_owner if you are wrapping stdin!
 * 
 * can_seek is non-zero if the given file handle supports random access.
 * If this is non-zero, then rewinding will be available.  You should
 * NOT specify can_seek if you are wrapping stdin!
 * 
 * If can_seek is specified, then this function will rewind the file.
 * The object will start out in a double-error state if this rewind
 * fails.
 * 
 * If can_seek is not specified, then the object always starts out
 * without any error state.
 * 
 * Parameters:
 * 
 *   pIn - the file handle to wrap
 * 
 *   is_owner - non-zero if handle should be closed when source object
 *   is released
 * 
 *   can_seek - non-zero if handle supports random access
 * 
 * Return:
 * 
 *   the new input source object
 */
SMFSOURCE *smfsource_new_handle(FILE *pIn, int is_owner, int can_seek);

/*
 * Construct an SMFSOURCE object by opening a file at a given path.
 * 
 * This is a wrapper around smfsource_new_handle().  Files opened with
 * this wrapper will always specify both is_owner and can_seek.  This
 * means that rewinds are always supported when using this constructor.
 * 
 * This constructor can fail!  It returns NULL if it fails to open the
 * file at the given path.
 * 
 * Parameters:
 * 
 *   pPath - the file path to open
 * 
 * Return:
 * 
 *   the new input source object, or NULL if the file could not be
 *   opened
 */
SMFSOURCE *smfsource_new_path(const char *pPath);

/*
 * Release an SMFSOURCE object.
 * 
 * If NULL is passed, this function will succeed without doing anything.
 * 
 * Otherwise, the close callback of the source object will be invoked if
 * defined, and then the source object will be released.
 * 
 * This function always releases the source object.  The return value
 * merely indicates whether something went wrong during the close-down
 * process, which you may want to warn the user about.  The source
 * object will be released even if this function returns failure.
 * 
 * This function can be used on any source object regardless of its
 * state.
 * 
 * Parameters:
 * 
 *   pSrc - the source object to close, or NULL
 * 
 * Return:
 * 
 *   non-zero if regular close-down, zero if object is closed but
 *   something went awry during the close-down process
 */
int smfsource_close(SMFSOURCE *pSrc);

/*
 * Check whether a given SMFSOURCE object is capable of rewinding back
 * to the beginning of input.
 * 
 * This function can be called on any SMFSOURCE object regardless of its
 * state.
 * 
 * Parameters:
 * 
 *   pSrc - the source object to query
 * 
 * Return:
 * 
 *   non-zero if source can be rewound, zero if not
 */
int smfsource_canRewind(SMFSOURCE *pSrc);

/*
 * Rewind a given SMFSOURCE object back to the beginning of input and
 * clear any error state or EOF state.
 * 
 * If called on an object that does not support rewinding, the function
 * fails but does not change the error state of the object.
 * 
 * If called on an object that supports rewinding and is in an error
 * state that is not a double-error state, this function will attempt a
 * rewind.  If successful, the error state is cleared in addition to
 * rewinding back to beginning of input.  If failure, the object is
 * changed to double-error state.
 * 
 * If called on an object that is in double-error state, then this
 * function fails without making an actual attempt to rewind.
 * 
 * Parameters:
 * 
 *   pSrc - the source object to rewind
 * 
 * Return:
 * 
 *   non-zero if successful, zero if rewind failed
 */
int smfsource_rewind(SMFSOURCE *pSrc);

/*
 * Skip a source ahead by a given number of bytes.
 * 
 * The skip distance must be greater than or equal to zero.  If zero is
 * passed, the function succeeds without doing anything, unless the
 * object is already in an error state, in which case the function still
 * fails.
 * 
 * This skip function can even be used with input sources that do not
 * define a skip callback.  In that case, the skip will be simulated
 * with repeated calls to the read callback.
 * 
 * If the skip would go beyond the end of the input, the skip is
 * shortened so that the next read will read EOF.  The function will
 * still succeed in this case.
 * 
 * If called on an object in error state, this function fails without
 * actually attempting a skip.
 * 
 * If a skip is attempted but fails, the object is switched into an
 * error state.
 * 
 * Parameters:
 * 
 *   pSrc - the source object
 * 
 *   skip - the non-negative skip distance
 * 
 * Return:
 * 
 *   non-zero if successful, zero if skip failed
 */
int smfsource_skip(SMFSOURCE *pSrc, int32_t skip);

/*
 * Read the next byte from a given input source object.
 * 
 * The return value is either an unsigned byte value in range 0 to 255
 * (inclusive), or one of the special codes SMFSOURCE_EOF or
 * SMFSOURCE_IOERR, indicating End Of File or I/O error, respectively.
 * 
 * If called on an object that is in error state, this function fails
 * with an I/O error without actually attempting a read.
 * 
 * If a read is attempted but fails, the object is changed into an error
 * state.
 * 
 * If the SMFSOURCE_EOF condition is returned, the object state switches
 * to EOF state, which causes subsequent reads to also return EOF
 * without actually attempting a read.  EOF state can be cleared with a
 * successful rewind.
 * 
 * Parameters:
 * 
 *   pSrc - the source object
 * 
 * Return:
 * 
 *   an unsigned byte value that was read from input, or SMFSOURCE_EOF
 *   if at end of file, or SMFSOURCE_IOERR if I/O error
 */
int smfsource_read(SMFSOURCE *pSrc);

/*
 * Allocate a new SMFPARSE object instance.
 * 
 * The parser instance should eventually be released with
 * smfparse_free().
 * 
 * Return:
 * 
 *   a new SMFPARSE object
 */
SMFPARSE *smfparse_alloc(void);

/*
 * Free an SMFPARSE instance.
 * 
 * If NULL is passed, the call is ignored.
 * 
 * Parameters:
 * 
 *   ps - the SMFPARSE instance to release, or NULL
 */
void smfparse_free(SMFPARSE *ps);

/*
 * Read the next entity from a MIDI file.
 * 
 * ps is the parser object to use.  If the parser is in an error state,
 * subsequent calls to read will just return the same error without
 * reading any further.  If the parser had previously returned the
 * entity SMF_TYPE_EOF, all subsequent calls will also return the entity
 * SMF_TYPE_EOF without reading anything further.
 * 
 * pEnt is the entity structure to receive the results of the parse.
 * Look at the status field to determine what entity was read or whether
 * there was an error or End Of File.
 * 
 * pSrc is the input source to read data from.  Bytes are read in
 * sequential order from this data source.
 * 
 * Parameters:
 * 
 *   ps - the parser object
 * 
 *   pEnt - the entity structure to fill with parsing results
 * 
 *   pSrc - the input source to read from
 */
void smfparse_read(SMFPARSE *ps, SMF_ENTITY *pEnt, SMFSOURCE *pSrc);

#endif
