/* AS path management routines.
   Copyright (C) 1996, 97, 98, 99 Kunihiro Ishiguro
   Copyright (C) 2005 Sun Microsystems, Inc.

This file is part of GNU Zebra.

GNU Zebra is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.

GNU Zebra is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Zebra; see the file COPYING.  If not, write to the Free
Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

#include <zebra.h>

#include "hash.h"
#include "memory.h"
#include "vector.h"
#include "vty.h"
#include "str.h"
#include "log.h"
#include "stream.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_aspath.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_attr.h"

/* Attr. Flags and Attr. Type Code. */
#define AS_HEADER_SIZE        2	 

/* Now FOUR octets are used for AS value. */
#define AS_VALUE_SIZE         sizeof (as_t)
/* This is the old one */
#define AS16_VALUE_SIZE	      sizeof (as16_t)

/* Maximum protocol segment length value */
#define AS_SEGMENT_MAX		255

/* The following length and size macros relate specifically to Quagga's
 * internal representation of AS-Segments, not per se to the on-wire
 * sizes and lengths.  At present (200508) they sort of match, however
 * the ONLY functions which should now about the on-wire syntax are
 * aspath_put, assegment_put and assegment_parse.
 *
 * JK: Warning.  It is really a sort of match.
 * In bgp_attr.c, where
 * aspath_put is being called, the amount of bytes needed on the
 * wire is calculated *by these macros* (through calling aspath_size)
 * and put onto the stream before aspath_put is called.
 * Because the amount of segments may be changed in aspath_put by merging
 * or splitting the internel aspath_segments, we currently have a problem
 * here.
 */

/* Calculated size in bytes of ASN segment data to hold N ASN's */
#define ASSEGMENT_DATA_SIZE(N) ((N) * AS_VALUE_SIZE)
#define ASSEGMENT16_DATA_SIZE(N) ((N) * AS16_VALUE_SIZE)

/* Calculated size of segment struct to hold N ASN's */
#define ASSEGMENT_SIZE(N)  (AS_HEADER_SIZE + ASSEGMENT_DATA_SIZE (N))
#define ASSEGMENT16_SIZE(N)  (AS_HEADER_SIZE + ASSEGMENT16_DATA_SIZE (N))

/* AS segment octet length. */
#define ASSEGMENT_LEN(X) ASSEGMENT_SIZE((X)->length)
#define ASSEGMENT16_LEN(X) ASSEGMENT16_SIZE((X)->length)

/* AS_SEQUENCE segments can be packed together */
/* Can the types of X and Y be considered for packing? */
#define ASSEGMENT_TYPES_PACKABLE(X,Y) \
  ( ((X)->type == (Y)->type) \
   && ((X)->type == AS_SEQUENCE))
/* Types and length of X,Y suitable for packing? */
#define ASSEGMENTS_PACKABLE(X,Y) \
  ( ASSEGMENT_TYPES_PACKABLE( (X), (Y)) \
   && ( ((X)->length + (Y)->length) <= AS_SEGMENT_MAX ) )

/* As segment header - the on-wire representation 
 * NOT the internal representation!
 */
struct assegment_header
{
  u_char type;
  u_char length;
};

/* Hash for aspath.  This is the top level structure of AS path. */
struct hash *ashash;

/* Stream for SNMP. See aspath_snmp_pathseg */
static struct stream *snmp_stream;

/* as4 related asnumber format things */

static
int bgp_asnumber_format = BGP_ASNUMBER_FORMAT_DEFAULT;

void
setasnumber_format ( int chosenformat )
{
  if ( chosenformat >= 0 && chosenformat < BGP_ASNUMBER_FORMAT_NOTUSED )
    {
      bgp_asnumber_format = chosenformat;
    }
}

int
get_asnumber_format ( )
{
  return bgp_asnumber_format;
}

/* provide asnumber->string conversion */
char *
as2str ( as_t as)
{
  /*
   * there are max. 2 asnumbers in one print
   * so if we use static memory, we can do it
   * by using two static places
   * space -- asdot has 5+1+5+1 +1 = 13Bytes ,
   * asip has 3+1+3+1+3+1+3+1 = 16 Bytes
   * asplain has 10+1 = 11 Bytes
   * so we are stuck with 16
   * Could also have done this with dynamic memory, but then the
   * caller would have to free it again which is a nuisance to
   * keep track of.  Or the caller could provide the buffer,
   * but that is also a headache.  (JK)
   */

  static char asstring1[16]; /* 16 for asip, which is max */
  static char asstring2[16]; /* 16 for asip, which is max */
  static int which = 0;
  char *usethis;

  which = 1 - which;
  usethis = (which) ? asstring1 : asstring2;

  switch ( bgp_asnumber_format )
    {
      case BGP_ASNUMBER_FORMAT_ASDOT:
            if ( as < 65536 )
                sprintf( usethis, "%u", as );
            else
                sprintf( usethis, "%u.%u", (as >> 16) & 0xffff, as & 0xffff );
            break;
      case BGP_ASNUMBER_FORMAT_ASDOTPLUS:
            sprintf( usethis, "%u.%u", (as >> 16) & 0xffff, as & 0xffff );
            break;
      case BGP_ASNUMBER_FORMAT_ASPLAIN:
            sprintf( usethis, "%u", as );
            break;
      case BGP_ASNUMBER_FORMAT_ASIP:
            sprintf( usethis, "%u.%u.%u.%u",
                ((as >> 24) & 0xff),
                ((as >> 16) & 0xff),
                ((as >> 8) & 0xff),
                (as & 0xff)
                );
            break;
    }
  return usethis;
}

/* return as number from string */
as_t str2asnum ( const char *p, const char **q )
{
  unsigned int dottedints[4]; /* max 4 ints */
  int numints; /* how many ints we saw */

  /* Eat AS4 regardless of syntax - eat asplain, asdot, asdot+, and asip,
   * while we're here
   */
  /* return current value of p in q if q is there.  Needed when
   * parsing a whole line of things
   */

  numints = 0;
  while  (numints < 4 && p && isdigit ((int) *p))
    {
      dottedints[numints] = (*p - '0');
      p++;
      while (isdigit ((int) *p))
        {
          dottedints[numints] *= 10;
          dottedints[numints] += (*p - '0');
          p++;
        }

      if ( *p == '.' )
        {
          p++;
          if ( ! isdigit ((int) *p) )
            {
              /* syntax error, ends with a dot */
	      if ( q != NULL )
		*q = p;
              return (as_t) 0;
            }
        }
      numints++;
    }
  if ( q != NULL )
      *q = p;
  switch ( numints )
    {
      case 0 : return (as_t) 0;
      case 1 : return dottedints[0];
      case 2 : return (dottedints[0] << 16) + dottedints[1];  /* asdot */
      case 3 : return (as_t) 0; /* syntax error - 3 numbers */
      case 4 : return (dottedints[0] << 24) + (dottedints[1] << 16)
                 + (dottedints[2] << 8) + dottedints[3]; /* asip */
    }
  return (as_t) 0;

}


static inline as_t *
assegment_data_new (int num)
{
  return (XCALLOC (MTYPE_AS_SEG_DATA, ASSEGMENT_DATA_SIZE (num)));
}

static inline void
assegment_data_free (as_t *asdata)
{
  XFREE (MTYPE_AS_SEG_DATA,asdata);
}

/* Get a new segment. Note that 0 is an allowed length,
 * and will result in a segment with no allocated data segment.
 * the caller should immediately assign data to the segment, as the segment
 * otherwise is not generally valid
 */
static struct assegment *
assegment_new (u_char type, u_short length)
{
  struct assegment *new;
  
  new = XCALLOC (MTYPE_AS_SEG, sizeof (struct assegment));
  
  if (length)
    new->as = assegment_data_new (length);
  
  new->length = length;
  new->type = type;
  
  return new;
}

static void
assegment_free (struct assegment *seg)
{
  if (!seg)
    return;
  
  if (seg->as)
    XFREE (MTYPE_AS_SEG_DATA, seg->as);
  memset (seg, 0xfe, sizeof(struct assegment));
  XFREE (MTYPE_AS_SEG, seg);
  
  return;
}

/* free entire chain of segments */
static void
assegment_free_all (struct assegment *seg)
{
  struct assegment *prev;
  
  while (seg)
    {
      prev = seg;
      seg = seg->next;
      assegment_free (prev);
    }
}

/* Duplicate just the given assegment and its data */
static struct assegment *
assegment_dup (struct assegment *seg)
{
  struct assegment *new;
  
  new = assegment_new (seg->type, seg->length);
  memcpy (new->as, seg->as, ASSEGMENT_DATA_SIZE (new->length) );
    
  return new;
}

/* Duplicate entire chain of assegments, return the head */
static struct assegment *
assegment_dup_all (struct assegment *seg)
{
  struct assegment *new = NULL;
  struct assegment *head = NULL;
  
  while (seg)
    {
      if (head)
        {
          new->next = assegment_dup (seg);
          new = new->next;
        }
      else
        head = new = assegment_dup (seg);
      
      seg = seg->next;
    }
  return head;
}

/* prepend the as number to given segment, given num of times */
static struct assegment *
assegment_prepend_asns (struct assegment *seg, as_t asnum, int num)
{
  as_t *newas;
  
  if (!num)
    return seg;
  
  if (num >= AS_SEGMENT_MAX)
    return seg; /* we don't do huge prepends */
  
  newas = assegment_data_new (seg->length + num);
  
  if (newas)
    {
      int i;
      for (i = 0; i < num; i++)
        newas[i] = asnum;
      
      memcpy (newas + num, seg->as, ASSEGMENT_DATA_SIZE (seg->length));
      XFREE (MTYPE_AS_SEG_DATA, seg->as);
      seg->as = newas; 
      seg->length += num;
      return seg;
    }

  assegment_free_all (seg);
  return NULL;
}

/* append given array of as numbers to the segment */
static struct assegment *
assegment_append_asns (struct assegment *seg, as_t *asnos, int num)
{
  as_t *newas;
  
  newas = XREALLOC (MTYPE_AS_SEG_DATA, seg->as,
		      ASSEGMENT_DATA_SIZE (seg->length + num));

  if (newas)
    {
      seg->as = newas;
      memcpy (seg->as + seg->length, asnos, ASSEGMENT_DATA_SIZE(num));
      seg->length += num;
      return seg;
    }

  assegment_free_all (seg);
  return NULL;
}

static int
int_cmp (const void *p1, const void *p2)
{
  const as_t *as1 = p1;
  const as_t *as2 = p2;
  
  return (*as1 == *as2) 
          ? 0 : ( (*as1 > *as2) ? 1 : -1);
}

/* normalise the segment.
 * In particular, merge runs of AS_SEQUENCEs into one segment
 * Internally, we do not care about the wire segment length limit, and
 * we want each distinct AS_PATHs to have the exact same internal
 * representation - eg, so that our hashing actually works..
 */
static struct assegment *
assegment_normalise (struct assegment *head)
{
  struct assegment *seg = head, *pin;
  struct assegment *tmp;
  
  if (!head)
    return head;
  
  while (seg)
    {
      pin = seg;
      
      /* Sort values SET segments, for determinism in paths to aid
       * creation of hash values / path comparisons
       * and because it helps other lesser implementations ;)
       */
      if (seg->type == AS_SET || seg->type == AS_CONFED_SET)
        qsort (seg->as, seg->length, sizeof(as_t), int_cmp);

      /* read ahead from the current, pinned segment while the segments
       * are packable/mergeable. Append all following packable segments
       * to the segment we have pinned and remove these appended
       * segments.
       */      
      while (pin->next && ASSEGMENT_TYPES_PACKABLE(pin, pin->next))
        {
          tmp = pin->next;
          seg = pin->next;
          
          /* append the next sequence to the pinned sequence */
          pin = assegment_append_asns (pin, seg->as, seg->length);
          
          /* bypass the next sequence */
          pin->next = seg->next;
          
          /* get rid of the now referenceless segment */
          assegment_free (tmp);
          
        }

      seg = pin->next;
    }
  return head;
}

static struct aspath *
aspath_new (void)
{
  struct aspath *aspath;

  aspath = XMALLOC (MTYPE_AS_PATH, sizeof (struct aspath));
  memset (aspath, 0, sizeof (struct aspath));
  return aspath;
}

/* Free AS path structure. */
void
aspath_free (struct aspath *aspath)
{
  if (!aspath)
    return;
  if (aspath->segments)
    assegment_free_all (aspath->segments);
  if (aspath->str)
    XFREE (MTYPE_AS_STR, aspath->str);
  XFREE (MTYPE_AS_PATH, aspath);
}

/* Unintern aspath from AS path bucket. */
void
aspath_unintern (struct aspath *aspath)
{
  struct aspath *ret;

  if (aspath->refcnt)
    aspath->refcnt--;

  if (aspath->refcnt == 0)
    {
      /* This aspath must exist in aspath hash table. */
      ret = hash_release (ashash, aspath);
      assert (ret != NULL);
      aspath_free (aspath);
    }
}

/* Return the start or end delimiters for a particular Segment type */
#define AS_SEG_START 0
#define AS_SEG_END 1
static char
aspath_delimiter_char (u_char type, u_char which)
{
  int i;
  struct
  {
    int type;
    char start;
    char end;
  } aspath_delim_char [] =
    {
      { AS_SET,             '{', '}' },
      { AS_CONFED_SET,      '[', ']' },
      { AS_CONFED_SEQUENCE, '(', ')' },
      { 0 }
    };

  for (i = 0; aspath_delim_char[i].type != 0; i++)
    {
      if (aspath_delim_char[i].type == type)
	{
	  if (which == AS_SEG_START)
	    return aspath_delim_char[i].start;
	  else if (which == AS_SEG_END)
	    return aspath_delim_char[i].end;
	}
    }
  return ' ';
}

/* countup asns from this segment and index onward */
static int
assegment_count_asns (struct assegment *seg, int from)
{
  int count = 0;
  while (seg)
    {
      if (!from)
        count += seg->length;
      else
        {
          count += (seg->length - from);
          from = 0;
        }
      seg = seg->next;
    }
  return count;
}

unsigned int
aspath_count_confeds (struct aspath *aspath)
{
  int count = 0;
  struct assegment *seg = aspath->segments;
  
  while (seg)
    {
      if (seg->type == AS_CONFED_SEQUENCE)
        count += seg->length;
      else if (seg->type == AS_CONFED_SET)
        count++;
      
      seg = seg->next;
    }
  return count;
}

unsigned int
aspath_count_hops (struct aspath *aspath)
{
  int count = 0;
  struct assegment *seg = aspath->segments;
  
  while (seg)
    {
      if (seg->type == AS_SEQUENCE)
        count += seg->length;
      else if (seg->type == AS_SET)
        count++;
      
      seg = seg->next;
    }
  return count;
}

unsigned int
aspath_size (struct aspath *aspath)
{
  int size = 0;
  struct assegment *seg = aspath->segments;
  
  while (seg)
    {
      size += ASSEGMENT_SIZE(seg->length);
      seg = seg->next;
    }
  return size;
}

/* Return highest public ASN in path */
as_t
aspath_highest (struct aspath *aspath)
{
  struct assegment *seg = aspath->segments;
  as_t highest = 0;
  unsigned int i;
  
  while (seg)
    {
      for (i = 0; i < seg->length; i++)
        if (seg->as[i] > highest
            && (seg->as[i] < BGP_PRIVATE_AS_MIN
                || seg->as[i] > BGP_PRIVATE_AS_MAX))
	  highest = seg->as[i];
      seg = seg->next;
    }
  return highest;
}

/* Return number of 32bit as numbers in in path */
unsigned int
aspath_count_num32as (struct aspath *aspath)
{
  struct assegment *seg = aspath->segments;
  unsigned int i, num;
  
  num=0;
  while (seg)
    {
      for (i = 0; i < seg->length; i++)
        if (seg->as[i] > BGP_AS_MAX)
	  num++;
      seg = seg->next;
    }
  return num;
}

/* Return number of as numbers in in path */
unsigned int
aspath_count_numas (struct aspath *aspath)
{
  struct assegment *seg = aspath->segments;
  unsigned int num;
  
  num=0;
  while (seg)
    {
      num += seg->length;
      seg = seg->next;
    }
  return num;
}

/* Convert aspath structure to string expression. */
static char *
aspath_make_str_count (struct aspath *as)
{
  struct assegment *seg;
  int str_size;
  int len = 0;
  char *str_buf;

  /* Empty aspath. */
  if (!as->segments)
    {
      str_buf = XMALLOC (MTYPE_AS_STR, 1);
      str_buf[0] = '\0';
      return str_buf;
    }
  
  seg = as->segments;
  
  /* ASN takes 5 chars at least, plus seperator, see below.
   * If there is one differing segment type, we need an additional
   * 2 chars for segment delimiters, and the final '\0'.
   * Hopefully this is large enough to avoid hitting the realloc
   * code below for most common sequences.
   */
  /* What will be common in the future? As soon as a 32 Bit AS number shows
   * up, such an ASN takes again 5+1 chars - if the whole number range will
   * be used.  But usage of AS4 will be the exception for some time now.
   * ah, what the heck, we put this into this now.
   */
#define ASN_STR_LEN (5 + 1)
  str_size = MAX (assegment_count_asns (seg, 0) * ASN_STR_LEN + 2 + 1
		  + aspath_count_num32as( as) * ASN_STR_LEN,
                  ASPATH_STR_DEFAULT_LEN);
  str_buf = XMALLOC (MTYPE_AS_STR, str_size);

  while (seg)
    {
      int i;
      char seperator;
      
      /* Check AS type validity. Set seperator for segment */
      switch (seg->type)
        {
          case AS_SET:
          case AS_CONFED_SET:
            seperator = ',';
            break;
          case AS_SEQUENCE:
          case AS_CONFED_SEQUENCE:
            seperator = ' ';
            break;
          default:
            XFREE (MTYPE_AS_STR, str_buf);
            return NULL;
        }
      
      /* We might need to increase str_buf, particularly if path has
       * differing segments types, our initial guesstimate above will
       * have been wrong.  need 5 chars for ASN, a seperator each and
       * potentially two segment delimiters, plus a space between each
       * segment and trailing zero.
       * ... and we need twice ASN_STR_LEN because all ASNUMs may be 32bit
       */
#define SEGMENT_STR_LEN(X) (((X)->length * ASN_STR_LEN)*2 + 2 + 1 + 1)
      if ( (len + SEGMENT_STR_LEN(seg)) > str_size)
        {
          str_size = len + SEGMENT_STR_LEN(seg);
          str_buf = XREALLOC (MTYPE_AS_STR, str_buf, str_size);
        }
#undef ASN_STR_LEN
#undef SEGMENT_STR_LEN
      
      if (seg->type != AS_SEQUENCE)
        len += snprintf (str_buf + len, str_size - len, 
			 "%c", 
                         aspath_delimiter_char (seg->type, AS_SEG_START));
      
      /* write out the ASNs, with their seperators, bar the last one*/
      for (i = 0; i < seg->length; i++)
        {
	  len += snprintf( str_buf + len, str_size - len, "%s", as2str(seg->as[i]));
          if (i < (seg->length - 1))
            len += snprintf (str_buf + len, str_size - len, "%c", seperator);
        }
      
      if (seg->type != AS_SEQUENCE)
        len += snprintf (str_buf + len, str_size - len, "%c", 
                        aspath_delimiter_char (seg->type, AS_SEG_END));
      if (seg->next)
        len += snprintf (str_buf + len, str_size - len, " ");
      
      seg = seg->next;
    }
  
  assert (len < str_size);
  
  str_buf[len] = '\0';

  return str_buf;
}

static void
aspath_str_update (struct aspath *as)
{
  if (as->str)
    XFREE (MTYPE_AS_STR, as->str);
  as->str = aspath_make_str_count (as);
}

/* Intern allocated AS path. */
struct aspath *
aspath_intern (struct aspath *aspath)
{
  struct aspath *find;
  
  /* Assert this AS path structure is not interned. */
  assert (aspath->refcnt == 0);

  /* Check AS path hash. */
  find = hash_get (ashash, aspath, hash_alloc_intern);

  if (find != aspath)
    aspath_free (aspath);

  find->refcnt++;

  if (! find->str)
    find->str = aspath_make_str_count (find);

  return find;
}

/* Duplicate aspath structure.  Created same aspath structure but
   reference count and AS path string is cleared. */
struct aspath *
aspath_dup (struct aspath *aspath)
{
  struct aspath *new;

  new = XCALLOC (MTYPE_AS_PATH, sizeof (struct aspath));

  if (aspath->segments)
    new->segments = assegment_dup_all (aspath->segments);
  else
    new->segments = NULL;

  new->str = aspath_make_str_count (aspath);

  return new;
}

static void *
aspath_hash_alloc (void *arg)
{
  struct aspath *aspath;

  /* New aspath structure is needed. */
  aspath = aspath_dup (arg);
  
  /* Malformed AS path value. */
  if (! aspath->str)
    {
      aspath_free (aspath);
      return NULL;
    }

  return aspath;
}

/* parse as-segment byte stream in struct assegment */
static struct assegment *
assegments_parse (struct stream *s, size_t length, int use32bit)
{
  struct assegment_header segh;
  struct assegment *seg, *prev = NULL, *head = NULL;
  size_t bytes = 0;
  
  /* empty aspath (ie iBGP or somesuch) */
  if (length == 0)
    return NULL;
  
  if (BGP_DEBUG (as4, AS4_SEGMENT))
    zlog_debug ("[AS4SEG] Parse aspath segment: got total byte length %d",length);
  /* basic checks */
  if ( (STREAM_READABLE(s) < length)
      || (STREAM_READABLE(s) < AS_HEADER_SIZE) 
      || (length % AS16_VALUE_SIZE ))
    return NULL;
  
  while ( (STREAM_READABLE(s) > AS_HEADER_SIZE)
         && (bytes < length))
    {
      int i;
      
      /* softly softly, get the header first on its own */
      segh.type = stream_getc (s);
      segh.length = stream_getc (s);

      if (BGP_DEBUG (as4, AS4_SEGMENT))
	zlog_debug ("[AS4SEG] Parse aspath segment: got type %d, length %d",segh.type, segh.length);
      
      /* check it.. */
      /*
       * If we get 16 Bit values, lengths are accordingly smaller
       * but we will always use 32 Bit for putting segments into memory
       */
      if ( ((bytes + ( (use32bit) ? ASSEGMENT_SIZE(segh.length) : ASSEGMENT16_SIZE(segh.length) )) > length)
          /* 1771bis 4.3b: seg length contains one or more */
          || (segh.length == 0) 
          /* Paranoia in case someone changes type of segment length */
          || ((sizeof segh.length > 1) && segh.length > AS_SEGMENT_MAX))
        {
          if (head)
            assegment_free_all (head);
          return NULL;
        }
      
      /* now its safe to trust lengths */
      seg = assegment_new (segh.type, segh.length);
      
      if (head)
        prev->next = seg;
      else /* it's the first segment */
        head = prev = seg;
      
      for (i = 0; i < segh.length; i++)
        {
	  seg->as[i] = (use32bit)? (stream_getl(s)) : (stream_getw (s));
          if (BGP_DEBUG (as4, AS4_SEGMENT))
	    zlog_debug ("[AS4SEG] Parse aspath segment: Read %s into this segment",as2str(seg->as[i]));
	}
      
      bytes += (use32bit) ? ASSEGMENT_SIZE(segh.length) : ASSEGMENT16_SIZE(segh.length);
      if (BGP_DEBUG (as4, AS4_SEGMENT))
	zlog_debug ("[AS4SEG] Parse aspath segment: Bytes now: %d",bytes);
      
      prev = seg;
    }
 
  return assegment_normalise (head);
}

/* AS path parse function.  pnt is a pointer to byte stream and length
   is length of byte stream.  If there is same AS path in the the AS
   path hash then return it else make new AS path structure. */
struct aspath *
aspath_parse (struct stream *s, size_t length, int use32bit)
{
  struct aspath as;
  struct aspath *find;

  /* If length is odd it's malformed AS path. */
  /* Nit-picking: if (use32bit == 0) it is malformed if odd,
   * otherwise its malformed when length is larger than 2 and (length-2) 
   * is not dividable by 4.
   * But... this time we're lazy
   */
  if (length % AS16_VALUE_SIZE )
    return NULL;

  as.segments = assegments_parse (s, length, use32bit);
  
  /* If already same aspath exist then return it. */
  find = hash_get (ashash, &as, aspath_hash_alloc);
  
  /* aspath_hash_alloc dupes segments too. that probably could be
   * optimised out.
   */
  assegment_free_all (as.segments);
  
  if (! find)
    return NULL;
  find->refcnt++;

  return find;
}

static inline void
assegment_data_put (struct stream *s, as_t *as, int num, int use32bit)
{
  int i;
  assert (num <= AS_SEGMENT_MAX);
  
  for (i = 0; i < num; i++)
    if ( use32bit )
      stream_putl (s, as[i]);
    else
      {
        if ( as[i] <= BGP_AS_MAX )
	  stream_putw(s, (unsigned short) as[i]);
	else
	  stream_putw(s, (unsigned short) BGP_AS_TRANS);
      }
}

static inline size_t
assegment_header_put (struct stream *s, u_char type, int length)
{
  size_t lenp;
  assert (length <= AS_SEGMENT_MAX);
  stream_putc (s, type);
  lenp = stream_get_endp (s);
  stream_putc (s, length);
  return lenp;
}

/* write aspath data to stream */
void
aspath_put (struct stream *s, struct aspath *as, int use32bit )
{
  struct assegment *seg = as->segments;
  
  if (!seg || seg->length == 0)
    return;
  
  if (seg)
    {
      /*
       * Hey, what do we do when we have > STREAM_WRITABLE(s) here?
       * At the moment, we would write out a partial aspath, and our peer
       * will complain and drop the session :-/
       * The general assumption here is that many things tested will
       * never happen.  And, in real live, up to now, they have not.
       */
      while (seg && (((use32bit)?ASSEGMENT_LEN (seg):ASSEGMENT16_LEN(seg)) <= STREAM_WRITEABLE(s)))
        {
          int written = 0;
          size_t lenp;
          
          /* Overlength segments have to be split up */
          while ( (seg->length - written) > AS_SEGMENT_MAX)
            {
              assegment_header_put (s, seg->type, AS_SEGMENT_MAX);
              assegment_data_put (s, seg->as, AS_SEGMENT_MAX, use32bit);
              written += AS_SEGMENT_MAX;
	      /* This is buggy, analogous to further down! */
	      zlog_err ("aspath_put: BUGGY splitting of an assegment! The peer will complain!");
            }
          
          /* write the final segment, probably is also the first */
          lenp = assegment_header_put (s, seg->type, seg->length - written);
          assegment_data_put (s, (seg->as + written), seg->length - written, use32bit);
          
          /* Sequence-type segments can be 'packed' together
           * Case of a segment which was overlength and split up
           * will be missed here, but that doesn't matter.
           */
          if (seg->next && ASSEGMENTS_PACKABLE (seg, seg->next))
            {
              /* NB: We should never normally get here given we
               * normalise aspath data when parse them. However, better
               * safe than sorry. We potentially could call
               * assegment_normalise here instead, but it's cheaper and
               * easier to do it on the fly here rather than go through
               * the segment list twice every time we write out
               * aspath's.
               */
              
              /* Next segment's data can fit in this one */
              assegment_data_put (s, seg->next->as, seg->next->length, use32bit);
              
              /* update the length of the segment header */
	      stream_putc_at (s, lenp, 
	                      seg->length - written + seg->next->length);
	      seg = seg->next->next; /* skip to past next */
	      /** ERROR HERE!
	       * We had set the size of the whole thing as a char or a
	       * short in front of the aspath in the stream.
	       * the aspath_size is calculated by adding up aspath_segments
	       * lengths, each with header.
	       * now we have joined two segments, thus have one aspath_
	       * segment header *less*, so we do *not* have put the right
	       * length into the stream.
	       */
	      zlog_err ("aspath_put: BUGGY merging of two assegments! The peer will complain!");
	    }
          else
            seg = seg->next;
        }
    }
}

/* This is for SNMP BGP4PATHATTRASPATHSEGMENT
 * We have no way to manage the storage, so we use a static stream
 * wrapper around aspath_put.
 */
u_char *
aspath_snmp_pathseg (struct aspath *as, size_t *varlen)
{
#define SNMP_PATHSEG_MAX 1024

  if (!snmp_stream)
    snmp_stream = stream_new (SNMP_PATHSEG_MAX);
  else
    stream_reset (snmp_stream);
  
  if (!as)
    {
      *varlen = 0;
      return NULL;
    }
  aspath_put (snmp_stream, as, 0); /* use 16 bit for now here */
  
  *varlen = stream_get_endp (snmp_stream);
  return stream_pnt(snmp_stream);
}
      
#define min(A,B) ((A) < (B) ? (A) : (B))

static struct assegment *
aspath_aggregate_as_set_add (struct aspath *aspath, struct assegment *asset,
			     as_t as)
{
  int i;

  /* If this is first AS set member, create new as-set segment. */
  if (asset == NULL)
    {
      asset = assegment_new (AS_SET, 1);
      if (! aspath->segments)
	aspath->segments = asset;
      else
        {
          struct assegment *seg = aspath->segments;
          while (seg->next)
            seg = seg->next;
          seg->next = asset;
        }
      asset->type = AS_SET;
      asset->length = 1;
      asset->as[0] = as;
    }
  else
    {
      /* Check this AS value already exists or not. */
      for (i = 0; i < asset->length; i++)
	if (asset->as[i] == as)
	  return asset;
      
      asset->length++;
      asset->as = XREALLOC (MTYPE_AS_SEG_DATA, asset->as, 
                            asset->length * AS_VALUE_SIZE);
      asset->as[asset->length - 1] = as;
    }
  

  return asset;
}

/* Modify as1 using as2 for aggregation. */
struct aspath *
aspath_aggregate (struct aspath *as1, struct aspath *as2)
{
  int i;
  int minlen;
  int match;
  int from;
  struct assegment *seg1 = as1->segments;
  struct assegment *seg2 = as2->segments;
  struct aspath *aspath;
  struct assegment *asset;

  match = 0;
  minlen = 0;
  aspath = NULL;
  asset = NULL;

  /* First of all check common leading sequence. */
  while (seg1 && seg2)
    {
      /* Check segment type. */
      if (seg1->type != seg2->type)
	break;

      /* Minimum segment length. */
      minlen = min (seg1->length, seg2->length);

      for (match = 0; match < minlen; match++)
	if (seg1->as[match] != seg2->as[match])
	  break;

      if (match)
	{
	  if (! aspath)
	    aspath = aspath_new ();
	  aspath->segments = assegment_new (seg1->type, 0);
	  aspath->segments = assegment_append_asns (aspath->segments, 
	                                            seg1->as, match);
	}

      if (match != minlen || match != seg1->length 
	  || seg1->length != seg2->length)
	break;
      
      seg1 = seg1->next;
      seg2 = seg2->next;
    }

  if (! aspath)
    aspath = aspath_new();

  /* Make as-set using rest of all information. */
  from = match;
  while (seg1)
    {
      for (i = from; i < seg1->length; i++)
	asset = aspath_aggregate_as_set_add (aspath, asset, seg1->as[i]);
      
      from = 0;
      seg1 = seg1->next;
    }

  from = match;
  while (seg2)
    {
      for (i = from; i < seg2->length; i++)
	asset = aspath_aggregate_as_set_add (aspath, asset, seg2->as[i]);

      from = 0;
      seg2 = seg2->next;
    }
  
  assegment_normalise (aspath->segments);
  aspath_str_update (aspath);
  return aspath;
}

/* When a BGP router receives an UPDATE with an MP_REACH_NLRI
   attribute, check the leftmost AS number in the AS_PATH attribute is
   or not the peer's AS number. */ 
int
aspath_firstas_check (struct aspath *aspath, as_t asno)
{
  if ( (aspath == NULL) || (aspath->segments == NULL) )
    return 0;
  
  if (aspath->segments
      && (aspath->segments->type == AS_SEQUENCE)
      && (aspath->segments->as[0] == asno ))
    return 1;

  return 0;
}

/* AS path loop check.  If aspath contains asno then return >= 1. */
int
aspath_loop_check (struct aspath *aspath, as_t asno)
{
  struct assegment *seg;
  int count = 0;

  if ( (aspath == NULL) || (aspath->segments == NULL) )
    return 0;
  
  seg = aspath->segments;
  
  while (seg)
    {
      int i;
      
      for (i = 0; i < seg->length; i++)
	if (seg->as[i] == asno)
	  count++;
      
      seg = seg->next;
    }
  return count;
}

/* When all of AS path is private AS return 1.  */
int
aspath_private_as_check (struct aspath *aspath)
{
  struct assegment *seg;
  
  if ( !(aspath && aspath->segments) )
    return 0;
    
  seg = aspath->segments;

  while (seg)
    {
      int i;
      
      for (i = 0; i < seg->length; i++)
	{
	  if ( (seg->as[i] < BGP_PRIVATE_AS_MIN)
	      || (seg->as[i] > BGP_PRIVATE_AS_MAX) )
	    return 0;
	}
      seg = seg->next;
    }
  return 1;
}

/* Merge as1 to as2.  as2 should be uninterned aspath. */
static struct aspath *
aspath_merge (struct aspath *as1, struct aspath *as2)
{
  struct assegment *last, *new;

  if (! as1 || ! as2)
    return NULL;

  last = new = assegment_dup_all (as1->segments);
  
  /* find the last valid segment */
  while (last && last->next)
    last = last->next;
  
  last->next = as2->segments;
  as2->segments = new;
  aspath_str_update (as2);
  return as2;
}

/* Prepend as1 to as2.  as2 should be uninterned aspath. */
struct aspath *
aspath_prepend (struct aspath *as1, struct aspath *as2)
{
  struct assegment *seg1;
  struct assegment *seg2;

  if (! as1 || ! as2)
    return NULL;
  
  seg1 = as1->segments;
  seg2 = as2->segments;
  
  /* If as2 is empty, only need to dupe as1's chain onto as2 */
  if (seg2 == NULL)
    {
      as2->segments = assegment_dup_all (as1->segments);
      aspath_str_update (as2);
      return as2;
    }
  
  /* If as1 is empty AS, no prepending to do. */
  if (seg1 == NULL)
    return as2;
  
  /* find the tail as1's segment chain. */
  while (seg1 && seg1->next)
    seg1 = seg1->next;

  /* Compare last segment type of as1 and first segment type of as2. */
  if (seg1->type != seg2->type)
    return aspath_merge (as1, as2);

  if (seg1->type == AS_SEQUENCE)
    {
      /* We have two chains of segments, as1->segments and seg2, 
       * and we have to attach them together, merging the attaching
       * segments together into one.
       * 
       * 1. dupe as1->segments onto head of as2
       * 2. merge seg2's asns onto last segment of this new chain
       * 3. attach chain after seg2
       */
      
      /* dupe as1 onto as2's head */
      seg1 = as2->segments = assegment_dup_all (as1->segments);
      
      /* refind the tail of as2, reusing seg1 */
      while (seg1 && seg1->next)
        seg1 = seg1->next;
      
      /* merge the old head, seg2, into tail, seg1 */
      seg1 = assegment_append_asns (seg1, seg2->as, seg2->length);
      
      /* bypass the merged seg2, and attach any chain after it to
       * chain descending from as2's head
       */
      seg1->next = seg2->next;
      
      /* seg2 is now referenceless and useless*/
      assegment_free (seg2);
      
      /* we've now prepended as1's segment chain to as2, merging
       * the inbetween AS_SEQUENCE of seg2 in the process 
       */
      aspath_str_update (as2);
      return as2;
    }
  else
    {
      /* AS_SET merge code is needed at here. */
      return aspath_merge (as1, as2);
    }
  /* XXX: Ermmm, what if as1 has multiple segments?? */
  
  /* Not reached */
}

/* Add specified AS to the leftmost of aspath. */
static struct aspath *
aspath_add_one_as (struct aspath *aspath, as_t asno, u_char type)
{
  struct assegment *assegment = aspath->segments;

  /* In case of empty aspath. */
  if (assegment == NULL || assegment->length == 0)
    {
      aspath->segments = assegment_new (type, 1);
      aspath->segments->as[0] = asno;
      
      if (assegment)
	assegment_free (assegment);

      return aspath;
    }

  if (assegment->type == type)
    aspath->segments = assegment_prepend_asns (aspath->segments, asno, 1);
  else 
    {
      /* create new segment
       * push it onto head of aspath's segment chain 
       */
      struct assegment *newsegment;
      
      newsegment = assegment_new (type, 1);
      newsegment->as[0] = asno;
      
      newsegment->next = assegment;
      aspath->segments = newsegment;
    }

  return aspath;
}

/* Add specified AS to the leftmost of aspath. */
struct aspath *
aspath_add_seq (struct aspath *aspath, as_t asno)
{
  return aspath_add_one_as (aspath, asno, AS_SEQUENCE);
}

/* Compare leftmost AS value for MED check.  If as1's leftmost AS and
   as2's leftmost AS is same return 1. */
int
aspath_cmp_left (struct aspath *aspath1, struct aspath *aspath2)
{
  struct assegment *seg1 = NULL;
  struct assegment *seg2 = NULL;

  if (!(aspath1 && aspath2))
    return 0;

  seg1 = aspath1->segments;
  seg2 = aspath2->segments;

  /* find first non-confed segments for each */
  while (seg1 && ((seg1->type == AS_CONFED_SEQUENCE)
		  || (seg1->type == AS_CONFED_SET)))
    seg1 = seg1->next;

  while (seg2 && ((seg2->type == AS_CONFED_SEQUENCE)
		  || (seg2->type == AS_CONFED_SET)))
    seg2 = seg2->next;

  /* Check as1's */
  if (!(seg1 && seg2
	&& (seg1->type == AS_SEQUENCE) && (seg2->type == AS_SEQUENCE)))
    return 0;
  
  if (seg1->as[0] == seg2->as[0])
    return 1;

  return 0;
}

/* Truncate an aspath after a number of hops, and put the hops remaining
 * at the front of another aspath.  Needed for AS4 compat. */
void
aspath_truncateathopsandjoin( struct aspath **aspath, struct aspath **new_aspath,int hops )
{
  struct assegment *startseg, *seg;
  struct assegment *prevseg = NULL;
  struct aspath *newfront, *newlybuild;

  if (!*aspath)
    return;

  startseg = seg = assegment_dup_all( (*aspath)->segments );
  
  while (seg && hops > 0)
    {
      if (seg->type == AS_SEQUENCE || seg->type == AS_CONFED_SEQUENCE )
	{
	  if ( hops < seg->length )
	    {
	      if ( seg->type == AS_CONFED_SEQUENCE )
	        {
		  /* Now, this can not be legal, can it?
		   * We are supposed to take only *some* ases out of
		   * a CONFED_SEQUENCE. and the path then continues
		   * with the contents of AS4_PATH?  you are
		   * kidding, this can not be right.
		   *
		   * Now, what is the fallback in this case?
		   * General fallback is always "use AS_PATH" iff we
		   * can not join aspath and newaspath in a
		   * reasonable way.  Tugging only a part of an
		   * AS_CONFED_SEQUENCE at the front of a constructed
		   * path is not reasonable, we loose information.
		   * So, we do nothing at all and take AS_PATH here.
		   *
		   * But burp out a warning, this is suspicious!
		   * Guess someone runs an AS confederation with only
		   * some routers as4 capable... something which is
		   * not supposed to be done...
		   */
		  assegment_free( seg );
		  if (BGP_DEBUG (as4, AS4))
		    zlog_debug ("[AS4] ASPATH32mangle: AS_CONFED_SEQUENCE would have been cut in two, taking AS_PATH instead of mangling");
		  aspath_unintern(*new_aspath);
		  *new_aspath = NULL;
		  return;
		}
	      /* take only hops */
	      seg->length = hops;
	      /* We have dangling ASes in memory here,
	       * but nobody uses them.
	       * When we free the thing (some lines down), they'll go away too
	       */
	      hops = 0;
	    }
	  else
	  {
	    hops -= seg->length; /* take the sequence and continue */
	  }
	}
      else if (seg->type == AS_SET || seg->type == AS_CONFED_SET)
	{
	  hops--; /* take this, it is one hop */
	}
      
      prevseg = seg;
      seg = seg->next;
    }
  /* we should now have hops == 0 and either seg or !seg */
  /* and prevseg points to the last thing which contains data we need */
  if ( prevseg )
    prevseg->next = NULL;

  assegment_free( seg ); /* that one is NULL-resistent */

  /* We have the startseg, and have to put that in front of
   * new_aspath->seg
   * Maybe this could be done easier, but this
   * way it works without leaving holes
   */
  newfront = aspath_new();
  newfront->segments = startseg;

  newlybuild = aspath_dup(*new_aspath);

  aspath_merge( newfront, newlybuild );
  aspath_free(newfront);

  aspath_unintern(*aspath);
  aspath_unintern(*new_aspath);
  *new_aspath = NULL;
  /* We may be able to join some segments here, and we must
   * do this because... we want normalised aspaths in out hash
   * and we do not want to stumble in aspath_put.
   */
  newlybuild->segments = assegment_normalise(newlybuild->segments);

  *aspath = aspath_intern(newlybuild);
}

/* Compare leftmost AS value for MED check.  If as1's leftmost AS and
   as2's leftmost AS is same return 1. (confederation as-path
   only).  */
int
aspath_cmp_left_confed (struct aspath *aspath1, struct aspath *aspath2)
{
  if (! (aspath1 && aspath2) )
    return 0;
  
  if ( !(aspath1->segments && aspath2->segments) )
    return 0;
  
  if ( (aspath1->segments->type != AS_CONFED_SEQUENCE)
      || (aspath2->segments->type != AS_CONFED_SEQUENCE) )
    return 0;
  
  if (aspath1->segments->as[0] == aspath2->segments->as[0])
    return 1;

  return 0;
}

/* Delete all leading AS_CONFED_SEQUENCE/SET segments from aspath.
 * See RFC3065, 6.1 c1 */
struct aspath *
aspath_delete_confed_seq (struct aspath *aspath)
{
  struct assegment *seg;

  if (!(aspath && aspath->segments))
    return aspath;

  seg = aspath->segments;
  
  /* "if the first path segment of the AS_PATH is 
   *  of type AS_CONFED_SEQUENCE,"
   */
  if (aspath->segments->type != AS_CONFED_SEQUENCE)
    return aspath;

  /* "... that segment and any immediately following segments 
   *  of the type AS_CONFED_SET or AS_CONFED_SEQUENCE are removed 
   *  from the AS_PATH attribute,"
   */
  while (seg && 
         (seg->type == AS_CONFED_SEQUENCE || seg->type == AS_CONFED_SET))
    {
      aspath->segments = seg->next;
      assegment_free (seg);
      seg = aspath->segments;
    }
  aspath_str_update (aspath);
  return aspath;
}

/* get rid of all AS_CONFED_SEQUENCE and AS_CONFED_SET path segments
 * in an aspath
 */
void
aspath_cleanoutall_asconfeds( struct aspath **aspath,
		unsigned int *aspath_data_size )
{
  struct assegment *seg, **ptto;
  struct aspath *cleanedout;
  int didsomething = 0;

  if (!(*aspath && (*aspath)->segments))
    return;

  cleanedout = aspath_dup( *aspath );
  ptto = &cleanedout->segments;
  seg = *ptto;

  while (seg)
    {
      if (seg->type == AS_CONFED_SEQUENCE || seg->type == AS_CONFED_SET)
	{
	  /* ignore this path segment */
	  struct assegment *ignorethis;
	  ignorethis = seg;

	  seg = ignorethis->next;
	  *ptto = seg;

          assegment_free (ignorethis);
	  didsomething = 1;
	}
      else
	{
	  ptto = &seg->next;
          seg = *ptto;
	}
    }
  if ( didsomething )
    {
      /* update necessary things */
      /* sigh, have to normalise - otherwise we can not advertise
       * the result due to aspath_put merging of segments
       */
      cleanedout->segments = assegment_normalise(cleanedout->segments);
      aspath_str_update (cleanedout);
      *aspath = cleanedout;
      *aspath_data_size = aspath_size(*aspath);
    }
  else
    {
      aspath_free( cleanedout );
    }
}

/* Add new AS number to the leftmost part of the aspath as
   AS_CONFED_SEQUENCE.  */
struct aspath*
aspath_add_confed_seq (struct aspath *aspath, as_t asno)
{
  return aspath_add_one_as (aspath, asno, AS_CONFED_SEQUENCE);
}

/* Add new as value to as path structure. */
static void
aspath_as_add (struct aspath *as, as_t asno)
{
  struct assegment *seg = as->segments;

  /* Last segment search procedure. */
  while (seg && seg->next)
    seg = seg->next;
  
  if (!seg)
    return;
  
  assegment_append_asns (seg, &asno, 1);
}

/* Add new as segment to the as path. */
static void
aspath_segment_add (struct aspath *as, int type)
{
  struct assegment *seg = as->segments;
  struct assegment *new = assegment_new (type, 0);

  while (seg && seg->next)
    seg = seg->next;
  
  if (seg == NULL)
    as->segments = new;
  else
    seg->next = new;
}

struct aspath *
aspath_empty (void)
{
  return aspath_parse (NULL, 0, 1); /* 32Bit ;-) */
}

struct aspath *
aspath_empty_get (void)
{
  struct aspath *aspath;

  aspath = aspath_new ();
  aspath->str = aspath_make_str_count (aspath);
  return aspath;
}

unsigned long
aspath_count (void)
{
  return ashash->count;
}     

/* 
   Theoretically, one as path can have:

   One BGP packet size should be less than 4096.
   One BGP attribute size should be less than 4096 - BGP header size.
   One BGP aspath size should be less than 4096 - BGP header size -
       BGP mandantry attribute size.
*/

/* AS path string lexical token enum. */
enum as_token
{
  as_token_asval,
  as_token_set_start,
  as_token_set_end,
  as_token_confed_seq_start,
  as_token_confed_seq_end,
  as_token_confed_set_start,
  as_token_confed_set_end,
  as_token_unknown
};

/* Return next token and point for string parse. */
static const char *
aspath_gettoken (const char *buf, enum as_token *token, u_long *asno)
{
  const char *p = buf;

  /* Skip seperators (space for sequences, ',' for sets). */
  while (isspace ((int) *p) || *p == ',')
    p++;

  /* Check the end of the string and type specify characters
     (e.g. {}()). */
  switch (*p)
    {
    case '\0':
      return NULL;
    case '{':
      *token = as_token_set_start;
      p++;
      return p;
    case '}':
      *token = as_token_set_end;
      p++;
      return p;
    case '(':
      *token = as_token_confed_seq_start;
      p++;
      return p;
    case ')':
      *token = as_token_confed_seq_end;
      p++;
      return p;
    case '[':
      *token = as_token_confed_set_start;
      p++;
      return p;
    case ']':
      *token = as_token_confed_set_end;
      p++;
      return p;
    }

  /* Check actual AS value. */
  /* Is now [<16bitnumber>.]<16bitnumber> for asdot(+),
   * <32bitnumber> for asplain,
   * or even <ipaddress> */

  if (isdigit ((int) *p)) 
    {
      /* consolidate, have only one place to interpret strings as asnums */
      const char *q;
      *asno = str2asnum( p, &q );
      *token = (*asno) ? as_token_asval : as_token_unknown;
      return q;
    }
  
  /* There is no match then return unknown token. */
  *token = as_token_unknown;
  return  p++;
}

struct aspath *
aspath_str2aspath (const char *str)
{
  enum as_token token = as_token_unknown;
  u_short as_type;
  u_long asno = 0;
  struct aspath *aspath;
  int needtype;

  aspath = aspath_new ();

  /* We start default type as AS_SEQUENCE. */
  as_type = AS_SEQUENCE;
  needtype = 1;

  while ((str = aspath_gettoken (str, &token, &asno)) != NULL)
    {
      switch (token)
	{
	case as_token_asval:
	  if (needtype)
	    {
	      aspath_segment_add (aspath, as_type);
	      needtype = 0;
	    }
	  aspath_as_add (aspath, asno);
	  break;
	case as_token_set_start:
	  as_type = AS_SET;
	  aspath_segment_add (aspath, as_type);
	  needtype = 0;
	  break;
	case as_token_set_end:
	  as_type = AS_SEQUENCE;
	  needtype = 1;
	  break;
	case as_token_confed_seq_start:
	  as_type = AS_CONFED_SEQUENCE;
	  aspath_segment_add (aspath, as_type);
	  needtype = 0;
	  break;
	case as_token_confed_seq_end:
	  as_type = AS_SEQUENCE;
	  needtype = 1;
	  break;
	case as_token_confed_set_start:
	  as_type = AS_CONFED_SET;
	  aspath_segment_add (aspath, as_type);
	  needtype = 0;
	  break;
	case as_token_confed_set_end:
	  as_type = AS_SEQUENCE;
	  needtype = 1;
	  break;
	case as_token_unknown:
	default:
	  aspath_free (aspath);
	  return NULL;
	}
    }

  aspath->str = aspath_make_str_count (aspath);

  return aspath;
}

/* Make hash value by raw aspath data. */
unsigned int
aspath_key_make (struct aspath *aspath)
{
  unsigned int key = 0;
  unsigned int i;
  struct assegment *seg = aspath->segments;
  struct assegment *prev = NULL;

  while (seg)
    {
      /* segment types should be part of the hash
       * otherwise seq(1) and set(1) will hash to same value
       */
      if (!(prev && seg->type == AS_SEQUENCE && seg->type == prev->type))
        key += seg->type;
      
      for (i = 0; i < seg->length; i++)
	key += seg->as[i];
      
      prev = seg;
      seg = seg->next;
    }

  return key;
}

/* If two aspath have same value then return 1 else return 0 */
static int
aspath_cmp (void *arg1, void *arg2)
{
  struct assegment *seg1 = ((struct aspath *)arg1)->segments;
  struct assegment *seg2 = ((struct aspath *)arg2)->segments;
  
  while (seg1 || seg2)
    {
      int i;
      if ((!seg1 && seg2) || (seg1 && !seg2))
	return 0;
      if (seg1->type != seg2->type)
        return 0;      
      if (seg1->length != seg2->length)
        return 0;
      for (i = 0; i < seg1->length; i++)
        if (seg1->as[i] != seg2->as[i])
          return 0;
      seg1 = seg1->next;
      seg2 = seg2->next;
    }
  return 1;
}

/* AS path hash initialize. */
void
aspath_init (void)
{
  ashash = hash_create_size (32767, aspath_key_make, aspath_cmp);
}

void
aspath_finish (void)
{
  hash_free (ashash);
  
  if (snmp_stream)
    stream_free (snmp_stream);
}

/* return and as path value */
const char *
aspath_print (struct aspath *as)
{
  return (as ? as->str : NULL);
}

/* Printing functions */
void
aspath_print_vty (struct vty *vty, const char *format, struct aspath *as)
{
  assert (format);
  vty_out (vty, format, as->str);
}

static void
aspath_show_all_iterator (struct hash_backet *backet, struct vty *vty)
{
  struct aspath *as;

  as = (struct aspath *) backet->data;

  vty_out (vty, "[%p:%u] (%ld) ", backet, backet->key, as->refcnt);
  vty_out (vty, "%s%s", as->str, VTY_NEWLINE);
}

/* Print all aspath and hash information.  This function is used from
   `show ip bgp paths' command. */
void
aspath_print_all_vty (struct vty *vty)
{
  hash_iterate (ashash, 
		(void (*) (struct hash_backet *, void *))
		aspath_show_all_iterator,
		vty);
}
