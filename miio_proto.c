#include <arpa/inet.h>
#include "miio_proto.h"
#include "lib_md5.h"
#include "lib_aes.h"

void md5(const char* message,int messageLen,char* v) {
    OI_Md5HashBuffer((BYTE *)v, (BYTE *) message , messageLen );
}

void init_msg_head(int version, char *msg_head,uint64_t indid,
        uint32_t stamp, const char* md5_sign, uint16_t length) {
    int n = 0;
    msg_head[n++] = '!';
    if(version == 1)
        msg_head[n++] = '1';
    else if(version == 2)
        msg_head[n++] = '2';
    else
        return;

    uint32_t did_h = (uint32_t)(indid >> 32);
    uint32_t did_l = (uint32_t)(indid);

    uint16_t nlength = htons(length);
    memcpy(msg_head + n, (void *)&nlength , 2);
    n += 2;

    uint32_t ndid_h = htonl(did_h);
    memcpy(msg_head + n, (void *)&ndid_h,4);
    n += 4;

    uint32_t ndid_l = htonl(did_l);
    memcpy(msg_head + n, (void *)&ndid_l,4);
    n += 4;

    uint32_t nstamp = htonl(stamp);
    memcpy(msg_head + n, (void *)&nstamp,4);
    n += 4;

    memcpy(msg_head + n, (void *)md5_sign, md5_size);

}
void parse_msg_head(const char* result_head, uint16_t* length,
        uint64_t* did, uint32_t* stamp, char* token) {
    if (result_head[0] != '!') {
        return;
    }
    if (result_head[1] != '1' && result_head[1] != '2') {
        return;
    }
    uint16_t _len = ntohs(*(uint16_t*)(result_head + 2 ));
    if (_len < 32) {
        return;
    }

    *length = _len;
    memcpy(token, result_head + 16,md5_size);

    uint32_t did_h = ntohl(
            *(uint32_t*)(result_head + 4));
    uint32_t did_l = ntohl(
            *(uint32_t*)(result_head + 8));
    uint64_t did_ = ((uint64_t)(did_h) << 32) | did_l;
    *did = did_;

    uint32_t _st = ntohl(
            *(uint32_t*)(result_head + 12));
    *stamp = _st;
}

void encrypt(int version, const uint64_t indid, const char* token,const uint32_t stamp,const char* body, const int bodylen,char* msg,int msglength) {

    //const size_t encslength = (( bodylen + AES_BLOCK_SIZE) / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;

    //std::vector<char> encjson(encslength, 0);
    //char* encjson = malloc(encslength);

    int outlen = 0;
    if(bodylen > 0)
    {
        AES_cbc_encrypt((const unsigned char*)body, bodylen, (unsigned char*)msg + header_size, &outlen, (const unsigned char*)token, AES_ENCRYPT,16);
    }
    init_msg_head(version, msg,indid,stamp,token,outlen + header_size);

    char sign[md5_size] = {0};
    md5(msg,msglength,sign);
    memcpy(msg + 16,sign,md5_size);

}

int decrypt(char* msg, const int msglen, const char* token,char* json,int jsonlen) {
    if (msglen <= 32) {
        return 0;
    }

    if (msg[0] != '!') {
        return 0;
    }

    if (msg[1] != '1' && msg[1] != '2') {
        return 0;
    }

    char insign[md5_size] = {0};
    memcpy(insign,msg+16,md5_size);

    memcpy(msg + 16, token , md5_size);
    char mysign[md5_size] = {0};
    md5(msg,msglen,mysign);

    if (memcmp(insign,mysign,md5_size) !=0) {
        return 0;
    }

    int outlen = 0;
    AES_cbc_encrypt((const unsigned char*)msg + header_size, msglen - header_size ,(unsigned char*)json, &outlen, (const unsigned char*)token, AES_DECRYPT,16);


    char* ptrpad = json + jsonlen - 1 ;
    uint8_t pad = *ptrpad;
    if (pad <= 16) {
        for (; pad > 0; pad--) {
            *ptrpad-- = '\0';
        }

        return outlen;
    }

    return 0;
}
