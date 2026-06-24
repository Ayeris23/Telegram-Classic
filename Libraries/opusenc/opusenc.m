#import "opusenc.h"
#import "TGOggOpusWriter.h"

#include "opus.h"
#include "opus_multistream.h"

#include <ogg/ogg.h>

#include "opus_header.h"

bool comment_init(char **comments, int* length, const char *vendor_string);
bool comment_add(char **comments, int* length, char *tag, char *val);
bool comment_pad(char **comments, int* length, int amount);

int writeOggPage(ogg_page *page, NSFileHandle *fileHandle)
{
    int written = page->header_len + page->body_len;
    
    [fileHandle writeData:[[NSData alloc] initWithBytesNoCopy:page->header length:page->header_len freeWhenDone:false]];
    [fileHandle writeData:[[NSData alloc] initWithBytesNoCopy:page->body length:page->body_len freeWhenDone:false]];
    
    return MAX(0, written);
}



/*
 Comments will be stored in the Vorbis style.
 It is describled in the "Structure" section of
    http://www.xiph.org/ogg/vorbis/doc/v-comment.html

 However, Opus and other non-vorbis formats omit the "framing_bit".

The comment header is decoded as follows:
  1) [vendor_length] = read an unsigned integer of 32 bits
  2) [vendor_string] = read a UTF-8 vector as [vendor_length] octets
  3) [user_comment_list_length] = read an unsigned integer of 32 bits
  4) iterate [user_comment_list_length] times {
     5) [length] = read an unsigned integer of 32 bits
     6) this iteration's user comment = read a UTF-8 vector as [length] octets
     }
  7) done.
*/

#define readint(buf, base) (((buf[base+3]<<24)&0xff000000)| \
                           ((buf[base+2]<<16)&0xff0000)| \
                           ((buf[base+1]<<8)&0xff00)| \
                           (buf[base]&0xff))
#define writeint(buf, base, val) do{ buf[base+3]=((val)>>24)&0xff; \
                                     buf[base+2]=((val)>>16)&0xff; \
                                     buf[base+1]=((val)>>8)&0xff; \
                                     buf[base]=(val)&0xff; \
                                 }while(0)

bool comment_init(char **comments, int *length, const char *vendor_string)
{
    // The 'vendor' field should be the actual encoding library used
    int vendor_length = strlen(vendor_string);
    int user_comment_list_length = 0;
    int len = 8 + 4 + vendor_length + 4;
    char *p = (char *)malloc(len);
    memcpy(p, "OpusTags", 8);
    writeint(p, 8, vendor_length);
    memcpy(p + 12, vendor_string, vendor_length);
    writeint(p, 12 + vendor_length, user_comment_list_length);
    *length = len;
    *comments = p;
    
    return true;
}

bool comment_add(char **comments, int* length, char *tag, char *val)
{
    char *p = *comments;
    int vendor_length = readint(p, 8);
    int user_comment_list_length = readint(p, 8 + 4 + vendor_length);
    int tag_len = (tag ? strlen(tag) + 1 : 0);
    int val_len = strlen(val);
    int len = (*length) + 4 + tag_len + val_len;
    
    p = (char *)realloc(p, len);
    
    writeint(p, *length, tag_len+val_len);      /* length of comment */
    if (tag)
    {
        memcpy(p + *length + 4, tag, tag_len);        /* comment tag */
        (p+*length+4)[tag_len-1] = '=';           /* separator */
    }
    memcpy(p + *length + 4 + tag_len, val, val_len);  /* comment */
    writeint(p, 8 + 4 + vendor_length, user_comment_list_length + 1);
    *comments = p;
    *length = len;
    
    return true;
}

bool comment_pad(char **comments, int* length, int amount)
{
    if (amount > 0)
    {
        char *p = *comments;
        // Make sure there is at least amount worth of padding free, and round up to the maximum that fits in the current ogg segments
        int newlen = (*length + amount + 255) / 255 * 255 - 1;
        p = realloc(p, newlen);
        for (int i = *length; i < newlen; i++)
        {
            p[i] = 0;
        }
        *comments = p;
        *length = newlen;
    }
    
    return true;
}

#undef readint
#undef writeint
