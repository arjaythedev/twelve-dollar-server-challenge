/* String utilities for HolyC */
#ifndef STRINGS_HC
#define STRINGS_HC

#include <string.h>
#include <stdlib.h>

extern I64 StrLen(U8 *s);
extern U8 *StrCopy(U8 *dest, U8 *src);
extern I64 StrCmp(U8 *s1, U8 *s2);
extern I64 StrNCmp(U8 *s1, U8 *s2, I64 n);
extern U8 *StrChr(U8 *s, I32 c);
extern U8 *StrStr(U8 *haystack, U8 *needle);
extern U8 *StrCat(U8 *dest, U8 *src);

#define my_strlen(s) strlen((char*)s)
#define my_strcpy(dest, src, n) (memcpy((void*)dest, (void*)src, n), (dest)[n] = '\0')
#define my_strcmp(s1,s2) strcmp((char*)s1,(char*)s2)
#define my_strncmp(s1,s2,n) strncmp((char*)s1,(char*)s2,n)
#define my_strchr(s,c) (U8*)strchr((char*)s,c)
#define my_strstr(hay,needle) (U8*)strstr((char*)hay,(char*)needle)
#define my_strcat(dest,src) (U8*)strcat((char*)dest,(char*)src)
#define my_sprintf sprintf
#define atol(x) atoi((char*)x)

#endif
