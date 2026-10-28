/* String utilities for HolyC */
#ifndef STRINGS_HC
#define STRINGS_HC

/* Declare external C string functions that hcc links */
extern I32 strlen(U8 *s);
extern I32 strcmp(U8 *s1, U8 *s2);
extern I32 strncmp(U8 *s1, U8 *s2, I64 n);
extern U8 *strchr(U8 *s, I32 c);
extern U8 *strstr(U8 *haystack, U8 *needle);
extern U8 *strcpy(U8 *dest, U8 *src);
extern U8 *strcat(U8 *dest, U8 *src);
extern U8 *memcpy(U8 *dest, U8 *src, I64 n);

/* Helper macros */
#define my_strlen(s) strlen((U8*)s)
#define my_strcmp(s1,s2) strcmp((U8*)s1,(U8*)s2)
#define my_strncmp(s1,s2,n) strncmp((U8*)s1,(U8*)s2,(I32)n)
#define my_strchr(s,c) strchr((U8*)s,(I32)c)
#define my_strstr(hay,needle) strstr((U8*)hay,(U8*)needle)
#define my_strcpy(dest, src) strcpy((U8*)dest, (U8*)src)
#define my_strcat(dest,src) strcat((U8*)dest,(U8*)src)
#define my_sprintf sprintf
#define atol(x) atoi((U8*)x)

#endif
