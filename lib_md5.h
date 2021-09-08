#ifndef MD5_H
#define MD5_H

#include <sys/types.h>
#include <stdint.h>

typedef unsigned int WORD32;
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef int BOOL;


#ifdef __cplusplus
extern "C"
{
#endif
    struct OI_MD5Context
    {
        u_int32_t buf[4];
        u_int32_t bits[2];
        unsigned char in[64];
    };

#define MD5InitA(context) OI_MD5InitA(context)
#define MD5UpdateA(context,buf,len) OI_MD5UpdateA(context,buf,len)
#define MD5FinalA(digest,context) OI_MD5FinalA(digest,context)
#define MD5TransformA(buf,in) OI_MD5TransformA(buf,in)
#define Md5HashBuffer(outBuffer,inBuffer,length) OI_Md5HashBuffer(outBuffer,inBuffer,length)

    void OI_MD5InitA(struct OI_MD5Context *context);
    void OI_MD5UpdateA(struct OI_MD5Context *context, unsigned char const *buf, unsigned len);
    void OI_MD5FinalA(unsigned char digest[16], struct OI_MD5Context *context);
    void OI_MD5TransformA(u_int32_t buf[4], u_int32_t const in[16]);

/*
 * This is needed to make RSAREF happy on some MS-DOS compilers.
 */
    typedef struct OI_MD5Context MD5_CTX_A;
    typedef struct OI_MD5Context MD5_CTX;


    void OI_Md5HashBuffer(BYTE * outBuffer, const BYTE * inBuffer, int length);

#ifdef __cplusplus
}
#endif

#endif                          /* !MD5_H */
