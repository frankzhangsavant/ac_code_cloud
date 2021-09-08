#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <mqueue.h>
#include <semaphore.h>
#include <netdb.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
//#include <unistd.h>

#include "common.h"
#include "libcommon.h"
#include "cJSON_Utils.h"
#include "cJSON.h"
#include "miio_proto.h"
#ifdef ENABLE_4G
#include "sim_card.h"
#endif

#define __PRINT_MACRO(x) #x
#define DEVICE_NAME(name, suffix) #name"-"__PRINT_MACRO(suffix)
#define IMAGE_SUFFIX(name, suffix) #name"_"__PRINT_MACRO(suffix)"m"
#define IMAGE_NAME  IMAGE_SUFFIX(home, DEVICE_SUFFIX)

#define MAX_HANDLE_NUM              8

#define KEEPALIVE_INTERVAL          300
#define OTC_INFO_INTERVAL           18000
#define SD_REPORT_INTERVAL          (24*3600*10)

#define MOTION_PIC              "/tmp/motion.jpg"
#define MOTION_VIDEO            "/tmp/motion.mp4"
#define SNAPSHOT_PIC            "/tmp/snapshot.jpg"
#define SNAPSHOT_VIDEO          "/tmp/snapshot.mp4"
#define BABYCRY_PIC              "/tmp/babycry.jpg"
#define BABYCRY_VIDEO            "/tmp/babycry.mp4"
#define ABNORMAL_SOUND_PIC      "/tmp/abnormal_sound.jpg"
#define ABNORMAL_SOUND_VIDEO    "/tmp/abnormal_sound.mp4"
#define PANORAMA_CAPTURE		"/tmp/panorama_capture"

#define TRUE    1
#define FALSE   0

typedef enum
{
    SD_OK,
    SD_SPEED_LOW,
    SD_NEED_FORMAT,
    SD_ABNORMAL_AFTER_FORMAT,
    SD_NOT_SUPPORT,
    SD_NO_CARD
}SD_STATUS;

typedef enum
{
    BIND_ERRNO_SUCCESS = 0,

    /* reserved for CURLcode */

    BIND_ERRNO_KEY_MISMATCH = 100,
    BIND_ERRNO_DEVICE_NOT_ALLOWED = 101,
    BIND_ERRNO_KEY_INVALID = 102,
    BIND_ERRNO_SYN_TIME_FAIL = 103,
    BIND_ERRNO_RESET_FAIL = 104,
    BIND_ERRNO_RESET_ERROR = 105,
    BIND_ERRNO_ONLINE_FAIL = 106,
    BIND_ERRNO_ONLINE_ERROR = 107,
    BIND_ERRNO_DO_BIND_FAIL = 108,
}bind_errno;

typedef enum
{
    MIIO_HANDLE_UNINIT = 0,
    MIIO_HANDLE_INIT,
    MIIO_HANDLE_UDP_SEND,
    MIIO_HANDLE_UDP_RECV,
    MIIO_HANDLE_TCP_SEND,
    MIIO_HANDLE_TCP_RECV,
    MIIO_HANDLE_PARSE_OK,
    MIIO_HANDLE_FINISH,
    MIIO_HANDLE_TIME_OUT,
    MIIO_HANDLE_DESTROYED
}miio_handle_state;

typedef enum
{
    MIIO_UDP = 1,
    MIIO_TCP
}miio_handle_protocol;

typedef enum
{
    MIIO_METHOD_SYNC_REGISTER,
    MIIO_METHOD_SYNC_ALERT_SETTING,
    MIIO_METHOD_PROPS,
    MIIO_METHOD_VERIFY_CODE,
    MIIO_METHOD_OTC_INFO,
    MIIO_METHOD_MIIO_INFO,
    MIIO_METHOD_KEEPALIVE,
    MIIO_METHOD_PROP_SD_SIZE,
    MIIO_METHOD_PROP_SD_AVAIL,
    MIIO_METHOD_PROP_VISITORS,
    MIIO_METHOD_GET_UPLOAD_URL,
    MIIO_METHOD_EVENT_MOTION,
    MIIO_METHOD_EVENT_SANPSHOT,
}miio_method;

typedef enum
{
    E_NORMAL_TYPE = 0,
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
    E_VOICE_TYPE,    // 声音报警
#endif
    E_FACE_TYPE,
    E_HUMAN_TYPE,
    E_BUTT
}e_short_video_type;
#if 0
typedef struct
{
    char version[2];
    uint16_t len;
    uint32_t did_low;
    uint32_t did_high;
    uint32_t stamp;
    char sign[16];
}miio_pkg_header;
#endif

typedef struct
{
    unsigned int part1;
    unsigned int part2;
    unsigned int part3;
    unsigned int stamp;
    unsigned int part4[4];
}miio_pkg_header;

typedef struct
{
    miio_handle_state state;
    miio_handle_protocol protocol;
    int time_out;
    int try_cnt;
    int wait_ret;
    int id;
    char send_plain_text[1024];
    char send_cipher_text[1024];
    char recv_plain_text[512];
    char recv_cipher_text[512];
    int send_plain_len;
    int send_cipher_len;
    int recv_plain_len;
    int recv_cipher_len;
}miio_handle;

miio_handle miio_handle_table[MAX_HANDLE_NUM];

typedef struct
{
    mqd_t mqfd_dispatch;
    mqd_t mqfd_cloud;

    mmap_info_s *g_mmap_info_ptr;

    unsigned int report_tick;
    unsigned int heartbeat_tick;
    unsigned int info_tick;

    int alarm_enable;
    unsigned int event_timeout;
    unsigned int miio_send_cnt;
    unsigned int miio_recv_cnt;
    int do_loging;
    int online_use_tmppwd;
#ifdef ENABLE_4G
    int need_recon;
#endif
    int do_4g_binding;
#ifdef PRODUCT_H31BG
    int event_tm;
#endif
}api_s;

static sem_t motion_sem;
static sem_t snap_sem;
static sem_t panorama_capture_sem;

static int global_id = 0;
static int sync_time_over = 0;
static int sync_register_over = 0;
static int first_otc_info_over = 0;
static int sync_props_over = 0;

static int video_upload_cnt = 0;
static int pic_upload_cnt = 0;
static int gen_url_fail = 0;
static int gen_url = 0;
static unsigned int error_time = 0;
static char sync_packet[512] = {0};
static int sync_packet_len = 0;
static int do_motion_push = 0;
static int do_snap_push = 0;
static int motion_apply_url = 0;
static int snap_apply_url = 0;
static char motion_upload_url[4096] = {0};
static char snap_upload_url[4096] = {0};
static int alert_max_retry = -1;
static int alert_retry_interval = -1;
static int alert_interval = -1;
static int panorama_capture_push_abort = 0;
static int report_baby_cry = 0;
static int report_bigger_move = 0;
static int report_abnormal_sound = 0;
static int report_rcd = 0;
static char rcd_file_path[512] = {};
static int g_bind_errno = BIND_ERRNO_SUCCESS;
static int g_debug_enable[DEBUG_INFO_TYPE_MAX_E] = { 0 };
static power_mode_e g_power_mode = POWER_MODE_OFF_E;
static int udp_sockfd = -1;
static int tcp_sockfd = -1;
static struct sockaddr_in servaddr;
static int g_updatestat = 0;
#ifdef PRODUCT_H31BG
//int g_timestring = 0;
#define IPC_SET_TIME 60;
#endif
api_s g_cloud_info;

int debug_mod = 0;
#if ENABLE_4G && ENABLE_SCANE_4G_DEVICE
int bind_4g_successed = 0;
#endif
int miio_send(char *send_buf, int send_len, int wait_ret);
int webapi_do_event_update(char *jpg_upload_url, char *mp4_upload_url, char *jpg_pwd, char *mp4_pwd, int type, int sub_type, time_t event_time, int face_count, int is_upload_video, char *event_uuid);
int webapi_do_log_upload();
void miio_info(int id);
void get_upload_url(int need_jpg, int need_mp4, int no_alert_check);
void event_motion(char *jpg_obj_name, char *mp4_ojb_name, char *jpg_pwd, char *mp4_pwd);


static int send_alarm_didi_msg(mqd_t mqfd)
{
    char send_buf[1024] = {0};
    COM_MSGHD_t msg = {0};
    int fsMsgRet = 0;
    int send_len = 0;

    //send msg to rmm
    msg.dstBitmap= MID_RMM;
    msg.mainOperation = RMM_SPEAK_ALARM;
    msg.msgLength = 0;
    msg.srcMid = MID_CLOUD;
    msg.subOperation = RMM_SPEAK_ALARM;

    memcpy(send_buf, &msg, sizeof(COM_MSGHD_t));
    send_len = sizeof(COM_MSGHD_t);

    if((fsMsgRet=mqueue_send(mqfd, send_buf, send_len))<0)
    {
        dump_string(_F_, _FU_, _L_, "msg snd err, err no is %d", fsMsgRet);
    }
    else
    {
        dump_string(_F_, _FU_, _L_, "msg snd success");
    }

    return fsMsgRet;
}

long long str2longlong(char *s)
{
    long long n = 0;

    while(*s)
    {
        n *= 10;
        n += *s - '0';
        ++s;
    }

    return n;
}

int write_to_file(char *filename, char *mode, char *write_buf, int write_len)
{
    FILE *fp = NULL;
    int ret = 0;

    if(strcmp("w", mode) != 0 && strcmp("w+", mode) != 0 && strcmp("a", mode) != 0 && strcmp("a+", mode) != 0)
    {
        return -1;
    }

    fp = fopen(filename, mode);
    if(fp == NULL)
    {
        return -1;
    }

    ret = fwrite(write_buf, 1, write_len, fp);
    if(ret != write_len)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    return 0;
}

int convert_shell_char(char *str, char *dst, int dst_size)
{
    int i = 0;
    int j = 0;
    char special = '`';

    if(!str || !dst)
    {
        return -1;
    }

    for(i=0,j=0; i<strlen(str) && j<(dst_size-1); i++, j++)
    {
        if(str[i] == special)
        {
            dst[j] = '\\';
            dst[++j] = str[i];
        }
        else
        {
            dst[j] = str[i];
        }
    }

    dst[j+1] = 0;

    return 0;
}

int cloud_send_msg(mqd_t mqfd, MSG_TYPE main_oper, char *payload, int payload_len)
{
    COM_MSGHD_t MsgHead;
    char send_buf[1024] = {0};
    int send_len = 0;
    int fsMsgRet = 0;

    memset(&MsgHead, 0, sizeof(MsgHead));
    MsgHead.srcMid = MID_CLOUD;
    MsgHead.mainOperation = main_oper;
    MsgHead.subOperation = 1;
    MsgHead.msgLength = payload_len;

    switch(main_oper)
    {
    case RCD_START_SHORT_VIDEO:
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
    case RCD_START_SHORT_VOICE_VIDEO:
#endif
    case RCD_START_SHORT_VIDEO_10S:
    case RCD_START_VOICECMD_VIDEO:
#ifdef HAVE_FEATURE_FACE
    case RCD_START_SHORT_FACE_VIDEO:
    case RCD_START_SHORT_HUMAN_VIDEO:
#endif
        MsgHead.dstBitmap = MID_RCD;
        break;
    case RMM_START_CAPTURE:
    case RMM_SPEAK_BAN_DEVICE:
    case RMM_START_PANORAMA_CAPTURE:
    case RMM_ABORT_PANORAMA_CAPTURE:
    case RMM_SET_ALARM:
        MsgHead.dstBitmap = MID_RMM;
        break;
    case CLOUD_DEBUG_ALARM:
        MsgHead.dstBitmap = MID_CLOUD;
        break;
#if defined(ETH_NET_BIND) || defined(ENABLE_4G)
    case DISPATCH_SET_BIND_SUCCESS:
    case DISPATCH_SET_BIND_FAIL:
    case DISPATCH_SET_BIND_TIMEOUT:
        MsgHead.dstBitmap = MID_DISPATCH | MID_MDNS;
        break;
#endif
    case  DISPATCH_SET_EVENT_UUID:
        MsgHead.dstBitmap = MID_DISPATCH;
        break;
    default:
        MsgHead.dstBitmap = MID_DISPATCH;
        break;
    }


    memcpy(send_buf, &MsgHead, sizeof(MsgHead));
    if(NULL!=payload && payload_len>0)
    {
        memcpy(send_buf + sizeof(MsgHead), payload, payload_len);
    }
    send_len = sizeof(MsgHead) + payload_len;

    fsMsgRet = mqueue_send(mqfd, send_buf, send_len);

    return fsMsgRet;
}

int cloud_get_sd_state(void)
{

    return 0;
}

int cloud_get_motion_state()
{
    return g_cloud_info.g_mmap_info_ptr->motion_stat;
}

#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
int cloud_get_face_state()
{
    return g_cloud_info.g_mmap_info_ptr->face_stat;
}

// 是否有一分钟的间隔
int cloud_get_is_interval()
{
    return g_cloud_info.g_mmap_info_ptr->is_interval;
}

int clould_get_motion_time()
{
    return g_cloud_info.g_mmap_info_ptr->motion_time;
}
#endif

void cloud_set_miio_send()
{
    g_cloud_info.miio_send_cnt ++;
    return;
}

void cloud_set_miio_recv()
{
    g_cloud_info.miio_recv_cnt ++;
    return;
}

int cloud_set_time(time_t systime)
{
    set_systime_t msg;
    int fsMsgRet = 0;

    memset_s(&msg, sizeof(msg), 0, sizeof(msg));
    //send msg to rmm
    msg.head.dstBitmap= MID_DISPATCH;
    msg.head.mainOperation = DISPATCH_SET_TIME;
    msg.head.msgLength = 0;
    msg.head.srcMid = MID_CLOUD;
    msg.head.subOperation = DISPATCH_SET_TIME;

    msg.systime = systime;

    if((fsMsgRet=mqueue_send(g_cloud_info.mqfd_dispatch, (char*)&msg, sizeof(msg)))<0)
    {
        dump_string(_F_, _FU_, _L_, "msg snd err, err no is %d", fsMsgRet);
    }
    else
    {
        sync_time_over = 1;
        dump_string(_F_, _FU_, _L_, "msg snd success");
    }

    return fsMsgRet;

}

int cloud_set_region(mqd_t mqfd, REGION_ID region_id, LANG_TYPE language, char *api_server, char *sname, char *dlproto)
{
    set_region_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.region_id = region_id;
    msg.language = language;
    snprintf(msg.api_server, sizeof(msg.api_server), "%s", api_server);
    snprintf(msg.sname, sizeof(msg.sname), "%s", sname);
    snprintf(msg.dlproto, sizeof(msg.dlproto), "%s", dlproto);
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_REGION, (char *)&msg, sizeof(msg)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_region send_msg fail!\n");
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_region send_msg ok!\n");
    }
    return 0;
}

int cloud_set_server_connect_ok()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_CONNECT_SUCCESS, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_server_connect_ok send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_server_connect_ok send_msg ok!\n");
        return 0;
    }
}

int cloud_set_bind_result(MSG_TYPE main_oper)
{
    #if defined(PRODUCT_B091QP)&&defined(ENABLE_4G)&&defined(ENABLE_SCANE_4G_DEVICE)
    if((g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0')&&(main_oper == DISPATCH_SET_BIND_SUCCESS))
    {
        bind_4g_successed = 1;
    }
    #elif defined(ENABLE_4G)&&defined(ENABLE_SCANE_4G_DEVICE)
	if(main_oper == DISPATCH_SET_BIND_SUCCESS)
	{
		bind_4g_successed = 1;
	}
	#endif

    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, main_oper, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_bind_result send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_bind_result send_msg ok!\n");
        return 0;
    }
}

int cloud_set_power(MSG_TYPE main_oper)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, main_oper, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_power send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_power send_msg ok!\n");
        return 0;
    }
}

int cloud_set_light(MSG_TYPE main_oper)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, main_oper, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_light send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_light send_msg ok!\n");
        return 0;
    }
}

int cloud_set_motion_record(MSG_TYPE main_oper)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, main_oper, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_motion_record_state send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_motion_record_state send_msg ok!\n");
        return 0;
    }
}

int cloud_set_mirror_flip(MSG_TYPE main_oper)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, main_oper, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_mirror_flip_state send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_mirror_flip_state send_msg ok!\n");
        return 0;
    }
}

int cloud_set_infrared_lamp(char *state)
{
    int value = -1;
    if(strcmp(state, "on") == 0)
    {
        value = 1;
    }
    else if(strcmp(state, "off") == 0)
    {
        value = 2;
    }
    else
    {
        return -1;
    }
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, RMM_SET_DAY_NIGHT_MODE, (char *)&value, sizeof(value)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_infrared_lamp_state %s send_msg fail!\n", state);
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_infrared_lamp_state %s send_msg ok!\n", state);
        return 0;
    }
}
int cloud_set_tnp(char *tnp_did, char *tnp_license, char *tnp_init_string)
{
    set_tnp_t msg;

    memset_s(&msg, sizeof(msg), 0, sizeof(msg));
    msg.head.dstBitmap= MID_DISPATCH;
    msg.head.mainOperation = DISPATCH_SET_TNP;
    msg.head.msgLength = sizeof(msg);
    msg.head.srcMid = MID_CLOUD;
    msg.head.subOperation = DISPATCH_SET_TNP;

    strncpy(msg.tnp_did, tnp_did, strlen(tnp_did));
    strncpy(msg.tnp_license, tnp_license, strlen(tnp_license));
    strncpy(msg.tnp_init_string, tnp_init_string, strlen(tnp_init_string));

    if(g_cloud_info.do_4g_binding == 1)
    {
        msg.is_4g_cmd = 1;
    }
    if(mqueue_send(g_cloud_info.mqfd_dispatch, (char*)&msg, sizeof(msg)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_tnp send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_tnp send_msg ok!\n");
        return 0;
    }
}

int cloud_finish_sync_info_from_server()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_FINISH_SYNC_INFO_FROM_SERVER, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_finish_sync_info_from_server send_msg DISPATCH_FINISH_SYNC_INFO_FROM_SERVER fail!\n");
    }

    return 0;
}
int cloud_check_ethernet()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_CHECK_ETHERNET, NULL, 0) < 0) {
        dump_string(_F_, _FU_, _L_,  "cloud_save_api_server send_msg DISPATCH_CHECK_ETHERNET fail!\n");
        return -1;
    }
    return 0;
}

int cloud_save_api_server()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SAVE_API_SERVER, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_save_api_server send_msg DISPATCH_SAVE_API_SERVER fail!\n");
    }

    return 0;
}

int cloud_clear_api_server()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_CLEAR_API_SERVER, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_clear_api_server send_msg DISPATCH_CLEAR_API_SERVER fail!\n");
    }

    return 0;
}

int cloud_set_tz_offset(int tz_offset)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_TZ_OFFSET, (char *)&tz_offset, sizeof(tz_offset)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_tz_offset %d send_msg msg fail!\n", tz_offset);
    }
    return 0;
}
int cloud_set_cloud_storage_state(MSG_TYPE msg)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, msg, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_cloud_storage_state %02x send_msg msg fail!\n", msg);
    }

    return 0;
}

int cloud_set_cloud_storage_mode(MSG_TYPE msg)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, msg, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_cloud_storage_mode %02x send_msg msg fail!\n", msg);
    }

    return 0;
}

int cloud_set_cloud_image_state(MSG_TYPE msg)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, msg, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_cloud_image_state %02x send_msg msg fail!\n", msg);
    }

    return 0;
}

int cloud_set_restart_type(MSG_TYPE msg,char *payload,int payload_len)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, msg, payload, payload_len) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_cloud_image_state %02x send_msg msg fail!\n", msg);
    }

    return 0;
}

int cloud_set_video_backup_state(video_backup_state_set *backup_state)
{
    int count_down = 0;

    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_VIDEO_BACKUP_STATE, (char *)backup_state, sizeof(video_backup_state_set_resp)) < 0)
    {
        dump_string(_F_, _FU_, _L_, "p2p_set_video_backup_state fail!\n");
        return -1;
    }

    count_down = 10;
    while(count_down)
    {
        count_down--;
        if(g_cloud_info.g_mmap_info_ptr->video_backup_info.enable == backup_state->enable &&
           g_cloud_info.g_mmap_info_ptr->video_backup_info.resolution == backup_state->resolution &&
           g_cloud_info.g_mmap_info_ptr->video_backup_info.backup_period == backup_state->backup_period &&
           g_cloud_info.g_mmap_info_ptr->video_backup_info.user_path == backup_state->user_path)
        {
            break ;
        }
        usleep(100*1000);
    }

    return 0;
}

int cloud_set_babycry_occur()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_BABYCRY_OCCUR, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_babycry_occur send_msg fail!\n");
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_babycry_occur send_msg ok!\n");
    }

    return 0;
}

int cloud_set_abnormal_sound_occur()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_ABNORMAL_SOUND_OCCUR, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_abnormal_sound_occur send_msg fail!\n");
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_abnormal_sound_occur send_msg ok!\n");
    }

    return 0;
}

int cloud_set_debug_mode()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_DEBUG_MODE, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_debug_mode send_msg fail!\n");
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_debug_mode send_msg ok!\n");
    }

    return 0;
}

int cloud_cap_pic(char *pic_name)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, RMM_START_CAPTURE, pic_name, strlen(pic_name)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_cap_pic send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_cap_pic send_msg ok!\n");
        return 0;
    }
}

int cloud_pic_index()
{    
    int pic_index_type  = 1;
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_CLOUD_PIC_INDEX_OCCUR, &pic_index_type, sizeof(int)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_pic_index send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_pic_index send_msg ok!\n");
        return 0;
    }
}


int cloud_make_video(char *video_name, int second, e_short_video_type short_video_type, time_t eventTime)
{
    MSG_TYPE msg_type;
    rcd_short_video_info_t rcdInfo;
    if(second == 6)
    {
        if(E_NORMAL_TYPE == short_video_type)
        {
            msg_type = RCD_START_SHORT_VIDEO;
        }
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
        else if(E_VOICE_TYPE == short_video_type)
        {
            msg_type = RCD_START_SHORT_VOICE_VIDEO;
        }
#endif
        else if(E_FACE_TYPE == short_video_type)
        {
            msg_type = RCD_START_SHORT_FACE_VIDEO;
        }
        else
        {
            msg_type = RCD_START_SHORT_HUMAN_VIDEO;
        }
    }
    else if( second == VC_RECORD_DURATION )
    {
        msg_type = RCD_START_VOICECMD_VIDEO;
    }
    else
    {
        msg_type = RCD_START_SHORT_VIDEO_10S;
    }

    strncpy(rcdInfo.fileName, video_name, sizeof(rcdInfo.fileName));
    rcdInfo.fileName[63] = 0;
    rcdInfo.eventTime = eventTime;

    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, msg_type, (char *)&rcdInfo, sizeof(rcdInfo)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_make_video %d(s) send_msg fail!\n", second);
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_make_video %d(s)send_msg ok!\n", second);
        return 0;
    }
}

int cloud_start_panorama_capture()
{
    int ret  = 0;

    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, RMM_START_PANORAMA_CAPTURE, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_, "cloud_start_panorama_capture send_msg to rmm fail!\n");
        ret = -1;
    }

    return ret;
}

int cloud_abort_panorama_capture()
{
    int ret = 0;

    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, RMM_ABORT_PANORAMA_CAPTURE, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_, "cloud_abort_panorama_capture send_msg to rmm fail!\n");
        ret = -1;
    }

    return ret;
}

int cloud_set_panorama_capture_state(PANORAMA_CAPTURE_STATE state)
{
    int ret = 0;

    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_PANORAMA_CAPTURE_STATE, (char *)&state, sizeof(state)) < 0)
    {
        dump_string(_F_, _FU_, _L_, "cloud_set_panorama_capture_state send_msg to dispatch fail!\n");
        ret = -1;
    }

    return ret;
}

int cloud_set_debug_info()
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, CLOUD_DEBUG_ALARM, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_debug_info send_msg fail!\n");
        return -1;
    }
    else
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_debug_info send_msg ok!\n");
        return 0;
    }
}

int cloud_set_event_uuid(char* uuid)
{
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_EVENT_UUID, uuid,  strlen(uuid) + 1) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "cloud_set_event_uuid send_msg fail!\n");
        return -1;
    }
    dump_string(_F_, _FU_, _L_,  "cloud_set_event_uuid send_msg ok!\n");
    return 0;
}



#ifdef HAVE_FEATURE_FACE
int cloud_set_human_face(int face_enable)
{
    int count_down = 0;
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, RMM_SET_HUMAN_FACE, (char *)&face_enable, sizeof(face_enable)) < 0)
    {
        dump_string(_F_, _FU_, _L_, "cloud_set_human_face %d send_msg fail!\n", face_enable);
        return -1;
    }
    return 0;
}
#endif

void miio_encrypt(char *in_buf, int in_len, char *out_buf, int *out_len)
{
    int version = 0;
    uint64_t did = 0;
    uint32_t stamp = 0;
    int payload_len = 0;
    int totoal_len = 0;
    char sn[32] = {0};

    if(in_len == 0)
    {
        did = -1;
        version = 1;
        payload_len = 0;
    }
    else
    {
        snprintf(sn, sizeof(sn), g_cloud_info.g_mmap_info_ptr->key_mi);
        version = 2;
        payload_len = ((in_len + 16) / 16) * 16;
        did = str2longlong(g_cloud_info.g_mmap_info_ptr->did_mi);
    }

    totoal_len = header_size + payload_len;
    stamp = time(NULL);
    encrypt(version, did, sn, stamp, in_buf, in_len, out_buf, totoal_len);
    *out_len = totoal_len;

    return;
}

void miio_decrypt(char *in_buf, int in_len, char *out_buf, int *out_len)
{
    int payload_len = 0;

    payload_len = in_len - header_size;
    decrypt(in_buf, in_len, g_cloud_info.g_mmap_info_ptr->key_mi, out_buf, payload_len);
    *out_len  = payload_len;

    return;
}

int miio_create_udp_socket()
{
    char domain_name[32] = "ot.io.mi.com";
    short dest_port = 8053;
    char dest_ip[20] = {0};
    struct hostent *he = NULL;
    char **pptr = NULL;

    udp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(udp_sockfd < 0)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_udp_socket socket fail!\n");
        return -1;
    }

    if(fcntl(udp_sockfd, F_SETFL, O_NONBLOCK) == -1)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_udp_socket fcntl fail!\n");
        close(udp_sockfd);
        udp_sockfd = -1;
        return -1;
    }

    he = gethostbyname(domain_name);
    if(he == NULL)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_udp_socket gethostbyname fail!\n");
        close(udp_sockfd);
        udp_sockfd = -1;
        return -1;
    }

    pptr = he->h_addr_list;
    inet_ntop(he->h_addrtype, (void*)*pptr, dest_ip, sizeof(dest_ip));

    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(dest_ip);
    servaddr.sin_port = htons(dest_port);

    return 0;
}

int miio_create_tcp_socket()
{
    char domain_name[32] = "ott.io.mi.com";
    short dest_port = 80;
    char dest_ip[20] = {0};
    struct hostent *he = NULL;
    struct sockaddr_in server = {0};
    char **pptr = NULL;
    struct timeval tv;

    tcp_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(tcp_sockfd < 0)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_tcp_socket socket fail!\n");
        return -1;
    }

    he = gethostbyname(domain_name);
    if(he == NULL)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_tcp_socket gethostbyname fail!\n");
        close(tcp_sockfd);
        tcp_sockfd = -1;
        return -1;
    }

    pptr = he->h_addr_list;
    inet_ntop(he->h_addrtype, (void*)*pptr, dest_ip, sizeof(dest_ip));

    bzero(&server,sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(dest_ip);
    server.sin_port= htons(dest_port);

    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if(setsockopt(tcp_sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_tcp_socket setsockopt fail!\n");
        close(tcp_sockfd);
        tcp_sockfd = -1;
        return -1;
    }

    if(connect(tcp_sockfd, (struct sockaddr *)&server, sizeof(struct sockaddr)) < 0)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_tcp_socket connect fail!\n");
        close(tcp_sockfd);
        tcp_sockfd = -1;
        return -1;
    }

    if(fcntl(tcp_sockfd, F_SETFL, O_NONBLOCK) == -1)
    {
        dump_string(_F_, _FU_, _L_,  "miio_create_tcp_socket fcntl fail!\n");
        close(tcp_sockfd);
        tcp_sockfd = -1;
        return -1;
    }

    return 0;
}

int miio_udp_send(char *send_buf, int send_len)
{
    int ret = 0;
    int addr_len = 0;

    if(udp_sockfd < 0)
    {
        ret = miio_create_udp_socket();
        if(ret < 0)
            return -1;
    }

    if(udp_sockfd > 0)
    {
        addr_len = sizeof(servaddr);
        ret = sendto(udp_sockfd, send_buf, send_len, 0, (struct sockaddr *)&servaddr, addr_len);
    }

    return ret;
}

int miio_tcp_send(char *send_buf, int send_len)
{
    int ret = 0;

    if(tcp_sockfd < 0)
    {
        ret = miio_create_tcp_socket();
        if(ret < 0)
            return -1;
    }

    if(tcp_sockfd > 0)
    {
        ret = send(tcp_sockfd, send_buf, send_len, 0);
    }

    return ret;
}

int miio_udp_recv(char *recv_buf, int *recv_len)
{
    int addr_len = 0;
    int ret = 0;

    if(udp_sockfd < 0)
    {
        ret = miio_create_udp_socket();
        if(ret < 0)
            return -1;
    }

    if(udp_sockfd > 0)
    {
        addr_len = sizeof(servaddr);
        ret = recvfrom(udp_sockfd, recv_buf, 4096, 0, (struct sockaddr *)&servaddr, (socklen_t *)&addr_len);
        *recv_len = ret;
    }

    return ret;
}

int miio_tcp_recv(char *recv_buf, int *recv_len)
{
    int ret = 0;

    if(tcp_sockfd < 0)
    {
        ret = miio_create_tcp_socket();
        if(ret < 0)
            return -1;
    }

    if(tcp_sockfd > 0)
    {
        ret = recv(tcp_sockfd, recv_buf, 4096, 0);
        *recv_len = ret;
    }

    return ret;
}

int addto_handle_table(miio_handle *handle)
{
    int i = 0;

    for(i = 0; i < MAX_HANDLE_NUM; i++)
    {
        if(miio_handle_table[i].state == MIIO_HANDLE_UNINIT)
        {
            memcpy(&miio_handle_table[i], handle, sizeof(miio_handle));
            return 0;
        }
    }

    return -1;
}

int rmfrom_handle_table(miio_handle *handle)
{
    int i = 0;

    for(i = 0; i < MAX_HANDLE_NUM; i++)
    {
        if(miio_handle_table[i].id == handle->id)
        {
            memset(&miio_handle_table[i], 0, sizeof(miio_handle));
            miio_handle_table[i].state = MIIO_HANDLE_UNINIT;
            return 0;
        }
    }

    return -1;
}

void check_gen_url_fail(miio_handle *handle)
{
    char method[64] = {0};
    int type = 0, sub_type = 0;

    if(trans_json_ex_s(method, sizeof(method), "\"method\"", handle->send_plain_text) == FALSE)
    {
        return;
    }

    type = 0;
    if(g_cloud_info.g_mmap_info_ptr->motion_type == 0)
    {
        sub_type = 1;			//�ƶ�
    }
    else
    {
        sub_type = 4;			//�ƶ�����
    }

    if(strcmp(method, "_sync.gen_presigned_url") == 0)
    {
        event_motion("", "", "", "");
        webapi_do_event_update("", "", "", "", type, sub_type, g_cloud_info.g_mmap_info_ptr->motion_time, 0, 1, "");
        gen_url_fail++;

        if(snap_apply_url == 1)
        {
            snap_apply_url = 0;
        }
        else if(motion_apply_url == 1)
        {
            motion_apply_url = 0;
        }
    }

    return;
}

void do_update(char *url)
{
    char cmd[1024] = {0};
    char buf[1024] = {0};
    char md5[64] = {0};
    char code[64] = {0};
    int success = 0;
    int trycnt = 0;

    trycnt = 5;

    while(trycnt>0)
    {
        trycnt-- ;
        memset(cmd, 0, sizeof(cmd));
        memset(buf, 0, sizeof(buf));
        snprintf(cmd, sizeof(cmd), "%s -c 301 -url \"%s/vmanager/upgrade/get_md5\"", CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server);
        system_cmd_withret_timeout(cmd, buf, sizeof(buf), 10);
        if(trans_json_ex_s(code, sizeof(code), "\"code\"", buf) == TRUE && atoi(code) == 20000 && trans_json_ex_s(md5, sizeof(md5), "\"md5\"", buf) == TRUE)
        {
            dump_string(_F_, _FU_, _L_, "cloudAPI get_md5 success %s\n", md5);
            success = 1;
            break;
        }

        ms_sleep(1000*3);
    }

    if(success == 1)
    {
        memset(cmd, 0, sizeof(cmd));
        snprintf(cmd, sizeof(cmd), "killall upgrade_firmware;rm -f /home/%s;rm -f /tmp/sd/%s;rm -f /tmp/update/%s;/backup/tools/upgrade_firmware \"%s\" \"%s\" &",
                 IMAGE_NAME, IMAGE_NAME, IMAGE_NAME,
                 url, md5);
        system(cmd);
    }

    g_updatestat = 1;

    return;
}

void do_update_ex(char *url, char *md5)
{
    char cmd[512] = {0};

    snprintf(cmd, sizeof(cmd), "killall upgrade_firmware;rm -f /home/%s;rm -f /tmp/sd/%s;rm -f /tmp/update/%s;/backup/tools/upgrade_firmware \"%s\" \"%s\" &",
             IMAGE_NAME, IMAGE_NAME, IMAGE_NAME,
             url, md5);

    system(cmd);

    g_updatestat = 1;

    return;
}

void do_get_progress(int *update_percent)
{
    char buf[128] = {0};
    char cmd[128] = {0};
    int getsize = 0;

    snprintf(cmd, sizeof(cmd), "ls /tmp/update/%s -l|awk '{print $5}'", IMAGE_NAME);

    if(1==g_updatestat)
    {
        system_cmd_withret_timeout(cmd, buf, sizeof(buf), 10);
        if(atoi(buf) > 0)
        {
            getsize = atoi(buf);
            *update_percent = MIN(100, getsize*100/(2*1000*1000));
            if(*update_percent >= 100)
                g_updatestat = 2;
        }
        else
        {
            *update_percent = 0;
        }
    }

    return;
}

int do_instruction(char *plain_text, int plain_len)
{
    char id_str[16] = {0};
    char method[64] = {0};
    char param[64] = {0};
    char app_url[256] = {0};
    char md5[64] = {0};
    char alarm_enable_str[4] = {0};
    int id = 0;
    int update_percent = 0;

    int need_jpg = 0, need_mp4 = 0;
    int no_alert_check = 0;
    char send_buf[4096] = {0};
    int send_len = 0;
    char cmd[1024] = {0};
    char cmdbuf[1024] = {0};

    if(trans_json_ex_s(id_str, sizeof(id_str), "\"id\"", plain_text) != TRUE || trans_json_ex_s(method, sizeof(method), "\"method\"", plain_text) != TRUE)
        return -1;

    id = atoi(id_str);

    if(strcmp(method, "miIO.ota") == 0)
    {
        if(trans_json_ex_s(app_url, sizeof(app_url), "\"app_url\"", plain_text) == TRUE)
        {
            dump_string(_F_, _FU_, _L_,  "app_url=%s\n", app_url);

            sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "miIO.ota success", "ok");
            send_len = strlen(send_buf);
            miio_send(send_buf, send_len, 0);

            if(trans_json_ex_s(md5, sizeof(md5), "\"file_md5\"", plain_text) == TRUE)
            {
                dump_string(_F_, _FU_, _L_,  "md5=%s\n", md5);

                do_update_ex(app_url, md5);
            }
            else
            {
                do_update(app_url);
            }
        }
    }
    else if(strcmp(method, "miIO.get_ota_progress") == 0)
    {
        do_get_progress(&update_percent);

        sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":%d}", id, 0, "miIO.get_ota_progress success", update_percent);
        send_len = strlen(send_buf);
        miio_send(send_buf, send_len, 0);
    }
    else if(strcmp(method, "miIO.info") == 0)
    {
        miio_info(id);
    }
    else if(strcmp(method, "bind_key") == 0)
    {
        if(g_cloud_info.g_mmap_info_ptr->bind_success == 0)
        {
            trans_json_ex_s(param, sizeof(param), "\"params\"", plain_text);

            if(strstr(param, "ok") != NULL)
            {
                cloud_set_bind_result(DISPATCH_SET_BIND_SUCCESS);
            }
            else
            {
                cloud_set_bind_result(DISPATCH_SET_BIND_FAIL);
            }

            sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "bind_key success", "ok");
            send_len = strlen(send_buf);
            miio_send(send_buf, send_len, 0);
        }
    }
    else if(strcmp(method, "miIO.settingchange") == 0)
    {
        if(trans_json_ex_s(alarm_enable_str, sizeof(alarm_enable_str), "\"enable_alarm\"", plain_text) == TRUE)
        {
            g_cloud_info.alarm_enable = atoi(alarm_enable_str);

            sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "miIO.settingchange success", "ok");
            send_len = strlen(send_buf);
            miio_send(send_buf, send_len, 0);
        }
    }
    else if(strcmp(method, "set_power") == 0)
    {
        trans_json_ex_s(param, sizeof(param), "\"params\"", plain_text);
        if(strcmp(param, "\"on\"") == 0)
        {
            cloud_set_power(DISPATCH_SET_POWER_ON);
        }
        else if(strcmp(param, "\"off\"") == 0)
        {
            cloud_set_power(DISPATCH_SET_POWER_OFF);
        }

        sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "set_power success", "ok");
        send_len = strlen(send_buf);
        miio_send(send_buf, send_len, 0);
    }
    else if(strcmp(method, "set_light") == 0)
    {
        trans_json_ex_s(param, sizeof(param), "\"params\"", plain_text);
        if(strcmp(param, "\"on\"") == 0)
        {
            cloud_set_light(DISPATCH_SET_LIGHT_ON);
        }
        else if(strcmp(param, "\"off\"") == 0)
        {
            cloud_set_light(DISPATCH_SET_LIGHT_OFF);
        }

        sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "set_light success", "ok");
        send_len = strlen(send_buf);
        miio_send(send_buf, send_len, 0);
    }
    else if(strcmp(method, "set_motion_record") == 0)
    {
        trans_json_ex_s(param, sizeof(param), "\"params\"", plain_text);
        if(strcmp(param, "\"on\"") == 0)
        {
            cloud_set_motion_record(DISPATCH_SET_MOTION_RCD);
        }
        else if(strcmp(param, "\"off\"") == 0)
        {
            cloud_set_motion_record(DISPATCH_SET_ALWAYS_RCD);
        }

        sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "set_motion_record success", "ok");
        send_len = strlen(send_buf);
        miio_send(send_buf, send_len, 0);
    }
    else if(strcmp(method, "set_infrared_lamp") == 0)
    {
        trans_json_ex_s(param, sizeof(param), "\"params\"", plain_text);
        if(atoi(param) == 1)
        {
            cloud_set_infrared_lamp("on");
        }
        else if(atoi(param) == 2)
        {
            cloud_set_infrared_lamp("off");
        }
        send_len = sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "set_infrared_lamp success", "ok");
        miio_send(send_buf, send_len, 0);
    }
    else if(strcmp(method, "set_mirror_flip") == 0)
    {
        trans_json_ex_s(param, sizeof(param), "\"params\"", plain_text);
        if(strcmp(param, "\"on\"") == 0)
        {
            cloud_set_mirror_flip(DISPATCH_SET_MIRROR_ON);
        }
        else if(strcmp(param, "\"off\"") == 0)
        {
            cloud_set_mirror_flip(DISPATCH_SET_MIRROR_OFF);
        }

        sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "set_mirror_flip success", "ok");
        send_len = strlen(send_buf);
        miio_send(send_buf, send_len, 0);
    }
    else if(strcmp(method, "snapshot") == 0)
    {
        trans_json_ex_s(param, sizeof(param), "\"params\"", plain_text);
        if(strstr(param, "\"picture\"") != NULL)
        {
            need_jpg = 1;
        }
        if(strstr(param, "\"video\"") != NULL)
        {
            need_mp4 = 1;
        }

        sprintf(send_buf, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":\"%s\"}", id, 0, "snapshot success", "ok");
        send_len = strlen(send_buf);
        miio_send(send_buf, send_len, 0);

        gen_url++;
        snap_apply_url = 1;
        sprintf(cmd, "rm -f %s* %s*", SNAPSHOT_PIC, SNAPSHOT_VIDEO);
        system_cmd_withret_timeout(cmd, cmdbuf, sizeof(cmdbuf), 10);
        cloud_cap_pic(SNAPSHOT_PIC);
        cloud_make_video(SNAPSHOT_VIDEO, 6, E_NORMAL_TYPE, time(NULL));
        no_alert_check = 1;
        get_upload_url(need_jpg, need_mp4, no_alert_check);
    }
    else
    {
        return -1;
    }

    return 0;
}

int parse_packet(char *cipher_text, int cipher_len)
{
    char plain_text[4096] = {0};
    int plain_len = 0;
    char id_str[16] = {0};
    int id = 0;
    int i = 0;

    if(cipher_len < 32)
        return -1;

    if(cipher_len == 32)
    {
        id = -1;
    }
    else
    {
        miio_decrypt(cipher_text, cipher_len, plain_text, &plain_len);
        if(trans_json_ex_s(id_str, sizeof(id_str), "\"id\"", plain_text) != TRUE)
            return -1;
        id = atoi(id_str);
    }

    dump_string(_F_, _FU_, _L_,  "parse_packet plain_text: %s\n", plain_text);

    for(i = 0; i < MAX_HANDLE_NUM; i++)
    {
        if(miio_handle_table[i].state == MIIO_HANDLE_UNINIT)
            continue;

        if(miio_handle_table[i].id == id)
        {
            memcpy(miio_handle_table[i].recv_cipher_text, cipher_text, cipher_len);
            memcpy(miio_handle_table[i].recv_plain_text, plain_text, plain_len);
            miio_handle_table[i].recv_cipher_len = cipher_len;
            miio_handle_table[i].recv_plain_len = plain_len;
            miio_handle_table[i].state = MIIO_HANDLE_PARSE_OK;
            if(miio_handle_table[i].wait_ret == 1)
            {
                cloud_set_miio_recv();
            }
            return 0;
        }
    }

    do_instruction(plain_text, plain_len);

    return 0;
}

int process_packet(miio_handle *handle)
{
    miio_pkg_header *head = NULL;
    time_t stamp = 0;
    char method[64] = {0};
    char alarm_enable_str[4] = {0};

    if(handle->recv_cipher_len == 32)
    {
        head = (miio_pkg_header *)handle->recv_cipher_text;
        stamp = ntohl(head->stamp);
        if(stamp <= 1435680000)
        {
            error_time = stamp;
            sync_packet_len = handle->recv_cipher_len;
            memset(sync_packet, 0, sizeof(sync_packet));
            memcpy(sync_packet, handle->recv_cipher_text, MIN(sizeof(sync_packet), sync_packet_len));
        }
        return cloud_set_time(stamp);
    }

    if(trans_json_ex_s(method, sizeof(method), "\"method\"", handle->send_plain_text) == FALSE)
    {
        return -1;
    }

    if(strcmp(method, "_sync.finish_register") == 0)
    {
        dump_string(_F_, _FU_, _L_,  "sync_register ok!\n");
        sync_register_over = 1;
    }
    else if(strcmp(method, "_sync.get_alert_setting") == 0)
    {
        if(trans_json_ex_s(alarm_enable_str, sizeof(alarm_enable_str), "\"enable_alarm\"", handle->recv_plain_text) == TRUE)
        {
            g_cloud_info.alarm_enable = atoi(alarm_enable_str);
            dump_string(_F_, _FU_, _L_,  "sync_alert_setting ok!\n");
        }
    }
    else if(strcmp(method, "props") == 0)
    {
        if(sync_props_over == 0)
        {
            sync_props_over = 1;
        }
        dump_string(_F_, _FU_, _L_, "bind_key send success!\n");
    }
    else if(strcmp(method, "_otc.info") == 0)
    {
        if(first_otc_info_over == 0)
        {
            first_otc_info_over = 1;
        }
    }
    else if(strcmp(method, "_sync.gen_presigned_url") == 0)
    {
        if(snap_apply_url == 1)
        {
            memset(snap_upload_url, 0, sizeof(snap_upload_url));
            memcpy(snap_upload_url, handle->recv_plain_text, handle->recv_plain_len);
            snap_apply_url = 0;
            sem_post(&snap_sem);
        }
        else if(motion_apply_url == 1)
        {
            memset(motion_upload_url, 0, sizeof(motion_upload_url));
            memcpy(motion_upload_url, handle->recv_plain_text, handle->recv_plain_len);
            motion_apply_url = 0;
            sem_post(&motion_sem);
        }
    }
    else if(strcmp(method, "event.motion") == 0)
    {
        dump_string(_F_, _FU_, _L_,  "miio event_motion ok!\n");
    }
    else
    {
        return -1;
    }

    return 0;
}

void handle_set_time_out(miio_handle *handle, int time_out)
{
    handle->time_out = g_cloud_info.g_mmap_info_ptr->systick + time_out*10;

    return;
}

int handle_time_out(miio_handle *handle)
{
    return (g_cloud_info.g_mmap_info_ptr->systick >= handle->time_out)?1:0;
}

int init_handle(miio_handle *handle, char *plain_text, int plain_len, int id, int wait_ret)
{
    char cipher_text[4096] = {0};
    int cipher_len = 0;

    if(handle == NULL)
        return -1;

    memset(handle, 0, sizeof(miio_handle));

    handle->state = MIIO_HANDLE_INIT;
    if(tcp_sockfd > 0)
    {
        handle->protocol = MIIO_TCP;
        handle->try_cnt = 1;
        handle_set_time_out(handle, 5);
    }
    else
    {
        handle->protocol = MIIO_UDP;
        handle->try_cnt = 3;
        handle_set_time_out(handle, 5);
    }

    miio_encrypt(plain_text, plain_len, cipher_text, &cipher_len);

    memcpy(handle->send_plain_text, plain_text, plain_len);
    memcpy(handle->send_cipher_text, cipher_text, cipher_len);
    handle->send_plain_len = plain_len;
    handle->send_cipher_len = cipher_len;
    handle->id = id;
    handle->wait_ret = wait_ret;

    return 0;
}

void service_handle()
{
    miio_handle *handle = NULL;
    struct timeval tv;
    fd_set rfds;
    char cipher_text[4096] = {0};
    int cipher_len = 0;
    int recv_cnt = 0;
    int i = 0;
    int ret = 0;

    for(i = 0; i < MAX_HANDLE_NUM; i++)
    {
        handle = &miio_handle_table[i];

        switch(handle->state)
        {
        case MIIO_HANDLE_UNINIT:
            break;

        case MIIO_HANDLE_INIT:
            if(handle->protocol == MIIO_UDP)
            {
                handle->state = MIIO_HANDLE_UDP_SEND;
            }
            else if(handle->protocol == MIIO_TCP)
            {
                handle->state = MIIO_HANDLE_TCP_SEND;
            }
            break;

        case MIIO_HANDLE_UDP_SEND:
            if(handle_time_out(handle))
            {
                dump_string(_F_, _FU_, _L_, "miio_udp_send fail!\n");
                handle->try_cnt--;
                if(handle->try_cnt > 0)
                {
                    handle->state = MIIO_HANDLE_UDP_SEND;
                    handle_set_time_out(handle, 5);
                }
                else
                {
                    close(udp_sockfd);
                    udp_sockfd = -1;
                    handle->protocol = MIIO_TCP;
                    handle->state = MIIO_HANDLE_TCP_SEND;
                    handle_set_time_out(handle, 5);
                }
            }
            else
            {
                ret = miio_udp_send(handle->send_cipher_text, handle->send_cipher_len);
                if(ret > 0)
                {
                    dump_string(_F_, _FU_, _L_,  "miio_udp_send ok!\n");
                    if(handle->wait_ret == 1)
                    {
                        cloud_set_miio_send();
                    }
                    if(handle->wait_ret == 0)
                    {
                        handle->state = MIIO_HANDLE_FINISH;
                    }
                    else
                    {
                        handle->state = MIIO_HANDLE_UDP_RECV;
                        handle_set_time_out(handle, 5);
                    }
                }
            }
            break;

        case MIIO_HANDLE_UDP_RECV:
            if(handle_time_out(handle))
            {
                dump_string(_F_, _FU_, _L_,  "miio_udp_recv fail!\n");
                handle->try_cnt--;
                if(handle->try_cnt > 0)
                {
                    handle->state = MIIO_HANDLE_UDP_SEND;
                    handle_set_time_out(handle, 5);
                }
                else
                {
                    close(udp_sockfd);
                    udp_sockfd = -1;
                    handle->protocol = MIIO_TCP;
                    handle->state = MIIO_HANDLE_TCP_SEND;
                    handle_set_time_out(handle, 5);
                }
            }
            else
            {
                memset_s(cipher_text, sizeof(cipher_text), 0, sizeof(cipher_text));
                ret = miio_udp_recv(cipher_text, &cipher_len);
                if(ret > 0)
                {
                    recv_cnt++;
                    dump_string(_F_, _FU_, _L_,  "miio_udp_recv ok!\n");
                    parse_packet(cipher_text, cipher_len);
                }
            }
            break;

        case MIIO_HANDLE_TCP_SEND:
            if(handle_time_out(handle))
            {
                dump_string(_F_, _FU_, _L_,  "miio_tcp_send fail!\n");
                handle->try_cnt--;
                if(handle->try_cnt > 0)
                {
                    handle->state = MIIO_HANDLE_TCP_SEND;
                    handle_set_time_out(handle, 5);
                }
                else
                {
                    handle->state = MIIO_HANDLE_TIME_OUT;
                    close(tcp_sockfd);
                    tcp_sockfd = -1;
                }
            }
            else
            {
                ret = miio_tcp_send(handle->send_cipher_text, handle->send_cipher_len);
                if(ret > 0)
                {
                    dump_string(_F_, _FU_, _L_,  "miio_tcp_send ok!\n");
                    if(handle->wait_ret == 1)
                    {
                        cloud_set_miio_send();
                    }
                    if(handle->wait_ret == 0)
                    {
                        handle->state = MIIO_HANDLE_FINISH;
                    }
                    else
                    {
                        handle->state = MIIO_HANDLE_TCP_RECV;
                        handle_set_time_out(handle, 5);
                    }
                }
            }
            break;

        case MIIO_HANDLE_TCP_RECV:
            if(handle_time_out(handle))
            {
                dump_string(_F_, _FU_, _L_,  "miio_tcp_recv fail!\n");
                handle->try_cnt--;
                if(handle->try_cnt > 0)
                {
                    handle->state = MIIO_HANDLE_TCP_SEND;
                    handle_set_time_out(handle, 5);
                }
                else
                {
                    handle->state = MIIO_HANDLE_TIME_OUT;
                    close(tcp_sockfd);
                    tcp_sockfd = -1;
                }
            }
            else
            {
                memset_s(cipher_text, sizeof(cipher_text), 0, sizeof(cipher_text));
                ret = miio_tcp_recv(cipher_text, &cipher_len);
                if(ret > 0)
                {
                    recv_cnt++;
                    dump_string(_F_, _FU_, _L_,  "miio_tcp_recv ok!\n");
                    parse_packet(cipher_text, cipher_len);
                }
            }
            break;

        case MIIO_HANDLE_PARSE_OK:
            process_packet(handle);
            handle->state = MIIO_HANDLE_FINISH;
            break;

        case MIIO_HANDLE_TIME_OUT:
            check_gen_url_fail(handle);
        case MIIO_HANDLE_FINISH:
            rmfrom_handle_table(handle);
            break;

        case MIIO_HANDLE_DESTROYED:
            break;

        default:
            break;
        }
    }

    if(recv_cnt == 0)
    {
        FD_ZERO(&rfds);
        memset_s(&tv, sizeof(tv), 0, sizeof(tv));
        tv.tv_sec = 0;
        tv.tv_usec = 100*1000;

        if(udp_sockfd > 0)
        {
            FD_SET(udp_sockfd, &rfds);

            if(select(udp_sockfd+1, &rfds, NULL, NULL, &tv) > 0)
            {
                if(FD_ISSET(udp_sockfd, &rfds))
                {
                    ret = miio_udp_recv(cipher_text, &cipher_len);
                    if(ret > 0)
                    {
                        dump_string(_F_, _FU_, _L_,  "miio_udp_recv ok!\n");
                        parse_packet(cipher_text, cipher_len);
                    }
                }
            }
        }
        else if(tcp_sockfd > 0)
        {
            FD_SET(tcp_sockfd, &rfds);
            if(select(tcp_sockfd+1, &rfds, NULL, NULL, &tv) > 0)
            {
                if(FD_ISSET(tcp_sockfd, &rfds))
                {
                    ret = miio_tcp_recv(cipher_text, &cipher_len);
                    if(ret > 0)
                    {
                        dump_string(_F_, _FU_, _L_,  "miio_tcp_recv ok!\n");
                        parse_packet(cipher_text, cipher_len);
                    }
                }
            }
        }
    }

    return;
}


int miio_send(char *send_buf, int send_len, int wait_ret)
{
    miio_handle handle;
    char id_str[16] = {0};
    int id = 0;

    if(send_len <= 0)
    {
        id = -1;
    }
    else
    {
        if(trans_json_ex_s(id_str, sizeof(id_str), "\"id\"", send_buf) != TRUE)
        {
            return -1;
        }

        id = atoi(id_str);
    }

    if(init_handle(&handle, send_buf, send_len, id, wait_ret) != 0)
    {
        dump_string(_F_, _FU_, _L_,  "prepare_hanlde init_handle fail!\n");
        return -1;
    }

    if(addto_handle_table(&handle) != 0)
    {
        dump_string(_F_, _FU_, _L_,  "prepare_hanlde addto_handle_table fail!\n");
        return -1;
    }

    dump_string(_F_, _FU_, _L_,  "miio_send send_buf: %s\n", send_buf);

    return 0;
}

void construct_packet(miio_method method, int assign_id, char *plain_text, int *plain_len)
{
    int id = 0;

    if(assign_id != -1)
        id = assign_id;
    else
        id = global_id++;

    switch(method)
    {
    case MIIO_METHOD_SYNC_REGISTER:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[]}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, "_sync.finish_register");
        break;

    case MIIO_METHOD_SYNC_ALERT_SETTING:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[]}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, "_sync.get_alert_setting");
        break;

    case MIIO_METHOD_PROPS:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":{\"bind_key\":\"%s\"}}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, "props", g_cloud_info.g_mmap_info_ptr->bind_key);
        break;

    case MIIO_METHOD_OTC_INFO:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"uid\":%d,\"method\":\"%s\",\"params\":"
                             "{\"ap\":{\"rssi\":%d,\"ssid\":\"%s\",\"bssid\":\"%s\"},\"netif\":"
                             "{\"localIp\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"gw_mac\":\"%s\"},"
                             "\"tick\":%d,\"token\":\"%s\",\"passwd\":\"%s\",\"mac\":\"%s\",\"model\":\"yunyi.camera.v1\",\"sdcard_status\":%d,\"fw_ver\":\"%s\","
                             "\"life\":%d,\"passwd_valid_time\":%d,\"netcheck\":{\"in\":%d,\"out\":%d}}}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->miuid, "_otc.info",
                             g_cloud_info.g_mmap_info_ptr->signal_quality, g_cloud_info.g_mmap_info_ptr->ssid, g_cloud_info.g_mmap_info_ptr->bssid,
                             g_cloud_info.g_mmap_info_ptr->ip, g_cloud_info.g_mmap_info_ptr->mask, g_cloud_info.g_mmap_info_ptr->gw, g_cloud_info.g_mmap_info_ptr->gwmac,
                             g_cloud_info.g_mmap_info_ptr->systick/10, g_cloud_info.g_mmap_info_ptr->sn, g_cloud_info.g_mmap_info_ptr->pwd, g_cloud_info.g_mmap_info_ptr->mac,
                             cloud_get_sd_state(), g_cloud_info.g_mmap_info_ptr->version,
                             g_cloud_info.g_mmap_info_ptr->systick/10, g_cloud_info.g_mmap_info_ptr->pwd_valid_time, g_cloud_info.g_mmap_info_ptr->in_packet_loss, g_cloud_info.g_mmap_info_ptr->out_packet_loss);
        break;

    case MIIO_METHOD_MIIO_INFO:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"code\":%d,\"message\":\"%s\",\"result\":"
                             "{\"ap\":{\"rssi\":%d,\"ssid\":\"%s\",\"bssid\":\"%s\"},\"netif\":"
                             "{\"localIp\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"gw_mac\":\"%s\"},"
                             "\"tick\":%d,\"token\":\"%s\",\"passwd\":\"%s\",\"mac\":\"%s\",\"model\":\"yunyi.camera.v1\",\"sdcard_status\":%d,\"fw_ver\":\"%s\","
                             "\"life\":%d,\"passwd_valid_time\":%d,\"netcheck\":{\"in\":%d,\"out\":%d}}}",
                             id, 0, "miIO.info success",
                             g_cloud_info.g_mmap_info_ptr->signal_quality, g_cloud_info.g_mmap_info_ptr->ssid, g_cloud_info.g_mmap_info_ptr->bssid,
                             g_cloud_info.g_mmap_info_ptr->ip, g_cloud_info.g_mmap_info_ptr->mask, g_cloud_info.g_mmap_info_ptr->gw, g_cloud_info.g_mmap_info_ptr->gwmac,
                             g_cloud_info.g_mmap_info_ptr->systick/10, g_cloud_info.g_mmap_info_ptr->sn, g_cloud_info.g_mmap_info_ptr->pwd, g_cloud_info.g_mmap_info_ptr->mac,
                             cloud_get_sd_state(), g_cloud_info.g_mmap_info_ptr->version,
                             g_cloud_info.g_mmap_info_ptr->systick/10, g_cloud_info.g_mmap_info_ptr->pwd_valid_time, g_cloud_info.g_mmap_info_ptr->in_packet_loss, g_cloud_info.g_mmap_info_ptr->out_packet_loss);
        break;

    case MIIO_METHOD_KEEPALIVE:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[]}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, "_otc.keepalive");
        break;

    case MIIO_METHOD_PROP_SD_SIZE:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[%d]}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, "prop.sdcard_size", (int)g_cloud_info.g_mmap_info_ptr->sd_size);
        break;

    case MIIO_METHOD_PROP_SD_AVAIL:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[%d]}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, "prop.sdcard_avail", (int)g_cloud_info.g_mmap_info_ptr->sd_leftsize);
        break;

    case MIIO_METHOD_PROP_VISITORS:
        *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[%d]}",
                             id, g_cloud_info.g_mmap_info_ptr->p2pid, "prop.visitors", g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt);
        break;

    default:
        break;
    }

    return;
}

void construct_packet_get_upload_url(int assign_id, int need_jpg, int need_mp4, int no_alert_check, char *plain_text, int *plain_len)
{
    int id = 0;
    char suffix[64] = {0};

    if(assign_id != -1)
        id = assign_id;
    else
        id = global_id++;

    if(need_jpg)
    {
        strcat(suffix, "\"jpg\"");
    }

    if(need_mp4)
    {
        if(need_jpg)
        {
            strcat(suffix, ",\"mp4\"");
        }
        else
        {
            strcat(suffix, "\"mp4\"");
        }
    }

    *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":{\"suffix\":[%s],\"noAlertChecking\":%d}}",
                         id, g_cloud_info.g_mmap_info_ptr->p2pid, "_sync.gen_presigned_url", suffix, no_alert_check);
}

void construct_packet_event_motion(int assign_id, char *jpg_obj_name, char *mp4_ojb_name, char *jpg_pwd, char *mp4_pwd, char *plain_text, int *plain_len)
{
    int id = 0;

    if(assign_id != -1)
    {
        id = assign_id;
    }
    else
    {
        id = global_id++;
    }

    *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%u%ld\"]}",
                         id, g_cloud_info.g_mmap_info_ptr->p2pid, "event.motion", mp4_ojb_name, jpg_obj_name, mp4_pwd, jpg_pwd, g_cloud_info.g_mmap_info_ptr->systick/10, time(NULL));
}

void construct_packet_event_snapshot(int assign_id, char *jpg_obj_name, char *mp4_ojb_name, char *jpg_pwd, char *mp4_pwd, char *plain_text, int *plain_len)
{
    int id = 0;

    if(assign_id != -1)
    {
        id = assign_id;
    }
    else
    {
        id = global_id++;
    }

    *plain_len = sprintf(plain_text, "{\"id\":%d,\"sid\":\"yunyi.%s\",\"method\":\"%s\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%u%ld\"]}",
                         id, g_cloud_info.g_mmap_info_ptr->p2pid, "event.snapshot", mp4_ojb_name, jpg_obj_name, mp4_pwd, jpg_pwd, g_cloud_info.g_mmap_info_ptr->systick/10, time(NULL));
}

int need_update(char *url, char *md5)
{
    char cmd[512] = {0};
    char ret_string[2048] = {0};
    char buf[128] = {0};
    char need_update[64] = {0};
    char force_update[64] = {0};
    char tmp_url[256] = {0};
    char tmp_md5[64] = {0};
    int trycnt = 5;

    sprintf(cmd, "%s -c 140 -url \"%s/vmanager/upgrade\" "
            "-uid %s "
            "-sname %s "
            "-protocol %s "
            "-version %s ",
            CLOUDAPI_PATH,
            g_cloud_info.g_mmap_info_ptr->api_server,
            g_cloud_info.g_mmap_info_ptr->p2pid,
            g_cloud_info.g_mmap_info_ptr->sname,
            g_cloud_info.g_mmap_info_ptr->dlproto,
            g_cloud_info.g_mmap_info_ptr->version);

    dump_string(_F_, _FU_, _L_, "cmd = %s\n", cmd);

    while(trycnt > 0)
    {
        trycnt--;

        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "cmd = %s\n", cmd);

        memset(buf, 0, sizeof(buf));
        if(TRUE == trans_json_ex_s(buf, sizeof(buf), "code", ret_string) && atoi(buf) == 20000)
        {
            memset(need_update, 0, sizeof(need_update));
            memset(force_update, 0, sizeof(force_update));
            if((FALSE == trans_json_ex_s(need_update, sizeof(need_update), "needUpdate", ret_string) || strcmp(need_update, "true") != 0)
               && (FALSE == trans_json_ex_s(force_update, sizeof(force_update), "forceUpdate", ret_string) || strcmp(force_update, "true") != 0))
            {
                return 0;
            }

            memset(tmp_url, 0, sizeof(tmp_url));
            if(FALSE == trans_json_ex_s(tmp_url, sizeof(tmp_url), "downloadPath", ret_string) || strlen(tmp_url) == 0)
            {
                return 0;
            }

            memset(tmp_md5, 0, sizeof(tmp_md5));
            if(FALSE == trans_json_ex_s(tmp_md5, sizeof(tmp_md5), "md5Code", ret_string) || strlen(tmp_md5) == 0)
            {
                return 0;
            }

            strcpy(url, tmp_url);
            strcpy(md5, tmp_md5);
            return 1;
        }

        sleep(10);
    }

    return 0;
}

int webapi_do_tnp_on_line()
{
    char cmd[1024] = {0};
    char ret_string[2048] = {0};
    char code[16] = {0};
    char tnp_did[32] = {0};
    char tnp_license[32] = {0};
    char tnp_init_string[128] = {0};
    int success = 0;
    int trycnt = 5;

    dump_string(_F_, _FU_, _L_, "now do webapi_do_tnp_on_line \n");

    sprintf(cmd, "%s -c 141 -url \"%s/v4/tnp/on_line\" "
            "-keySec %s "
            "-uid %s "
            "-version %s",
            CLOUDAPI_PATH,
            g_cloud_info.g_mmap_info_ptr->api_server,
            g_cloud_info.g_mmap_info_ptr->key,
            g_cloud_info.g_mmap_info_ptr->p2pid,
            g_cloud_info.g_mmap_info_ptr->version);
    dump_string(_F_, _FU_, _L_, "cmd = %s\n", cmd);
    memset(ret_string, 0, sizeof(ret_string));

    success = 0;
    trycnt = 5;
    while(trycnt > 0)
    {
        trycnt-- ;

        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        memset(tnp_did, 0, sizeof(tnp_did));
        memset(tnp_license, 0, sizeof(tnp_license));
        memset(tnp_init_string, 0, sizeof(tnp_init_string));

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            dump_string(_F_, _FU_, _L_, "webapi_do_tnp_on_line success %s\n", ret_string);
            if(trans_json_ex_s(tnp_did, sizeof(tnp_did), "\"DID\"", ret_string) == TRUE && \
               trans_json_ex_s(tnp_license, sizeof(tnp_license), "\"License\"", ret_string) == TRUE &&
               trans_json_ex_s(tnp_init_string, sizeof(tnp_init_string), "\"InitString\"", ret_string) == TRUE)
            {
                char * p = NULL;
                p = strstr(tnp_license, ":");
                if(NULL != p)
                {
                    *p = '\0';
                }
                cloud_set_tnp(tnp_did, tnp_license, tnp_init_string);
            }

            success = 1;
            break;
        }

        sleep(5);
    }

    return success;
}

int webapi_do_check_region()
{
    char cmd[512] = {0};
    char ret_string[2048] = {0};
    char endpoint[128] = {0};
    char api_server[128] = {0};
    char sname[64] = {0};
    char dlproto[64] = {0};
    REGION_ID region_id = REGION_PAD;
    LANG_TYPE language = LANGUAGE_ENGLISH;
    int success = -1;
    int trycnt = 5;

    dump_string(_F_, _FU_, _L_, "now do check region\n");

    sprintf(cmd, "%s -c 143 -url %s "
            "-uid %s ",
            CLOUDAPI_PATH,
            "http://yicamera-router.mi-ae.com.sg/v1/ipc/router",
            g_cloud_info.g_mmap_info_ptr->p2pid);

    dump_string(_F_, _FU_, _L_, "cmd = %s\n", cmd);

    while(trycnt > 0)
    {
        trycnt-- ;

        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(endpoint, 0, sizeof(endpoint));
        memset(api_server, 0, sizeof(api_server));
        memset(sname, 0, sizeof(sname));
        memset(dlproto, 0, sizeof(dlproto));

        if(trans_json_ex_s(endpoint, sizeof(endpoint), "\"endpoint\"", ret_string) == TRUE && trans_json_ex_s(dlproto, sizeof(dlproto), "\"dlProtocol\"", ret_string) == TRUE)
        {
            snprintf(api_server, sizeof(api_server), "https://%s", endpoint);

            if(strcmp(dlproto, "mius") == 0)
            {
                region_id = REGION_AMERICA;
                snprintf(sname, sizeof(sname), "%s", DEVICE_NAME(familymonitor, DEVICE_SUFFIX));
            }
            else if(strcmp(dlproto, "mieu") == 0)
            {
                region_id = REGION_EUROPE;
                snprintf(sname, sizeof(sname), "%s", DEVICE_NAME(familymonitor, DEVICE_SUFFIX));
            }
            else
            {
                region_id = REGION_SOUTHEAST_ASIA;
                snprintf(sname, sizeof(sname), "%s", DEVICE_NAME(familymonitor, DEVICE_SUFFIX));
            }

            dump_string(_F_, _FU_, _L_, "in webapi_do_check_region, region_id = %d, language = %d, api_server = %s, sname = %s, dlproto = %s\n", region_id, language, api_server, sname, dlproto);

            cloud_set_region(g_cloud_info.mqfd_dispatch, region_id, language, api_server, sname, dlproto);
            success = 1;
            break;
        }

        sleep(10);
    }

    if(1!=success)
    {
        return 0;
    }

    return 1;
}

int webapi_do_reset_by_specified_server(char *api_server, int time_count, int type)
{
    char ret_string[2048] = {0};
    char cmd[512] = {0};
    char code[16] = {0};
    int success = 0;
    int trycnt = time_count;

    char curl_code_string[16] = {0};
    int curl_code = 0;

    struct timeval tv;
    unsigned int seed;

    gettimeofday(&tv, NULL);
    seed += ((int)tv.tv_usec+tv.tv_sec);
    srand(seed);

    dump_string(_F_, _FU_, _L_, "now do reset \n");

    sprintf(cmd, "%s -c 139 -keySec %s -url %s/v4/ipc/reset "
            "-uid %s "
            "-version %s "
            "-mac %s ",
            CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->key,
            api_server,
            g_cloud_info.g_mmap_info_ptr->p2pid,
            g_cloud_info.g_mmap_info_ptr->version,
            g_cloud_info.g_mmap_info_ptr->mac);

    dump_string(_F_, _FU_, _L_, "cmd = %s\n", cmd);

    success = 0;

    while(trycnt > 0)
    {
        trycnt-- ;
        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE)
        {
            if(atoi(code) == 20000)
            {
                dump_string(_F_, _FU_, _L_, "webapi_do_reset success %s\n", ret_string);
                success = 1;
                break;
            }
            else if(atoi(code) == 20241)
            {
                if(1 == type)
                {
                    g_bind_errno = BIND_ERRNO_RESET_ERROR;
                }
                if(g_cloud_info.g_mmap_info_ptr->region_id != REGION_CHINA)
                {
                    if(webapi_do_check_region() == 1)
                    {
                        sleep(1);
                        return 0;
                    }
                }
                else
                {
                    dump_string(_F_, _FU_, _L_, "webapi_do_reset fail %s\n", ret_string);
                    //cloud_set_bind_result(DISPATCH_SET_BIND_FAIL);
                    return 0;
                }
            }
        }

        if(1 == type)
        {
            if(trans_json_ex_s(curl_code_string, sizeof(curl_code_string), "\"curl_code\"", ret_string) == TRUE)
            {
                curl_code = atoi(curl_code_string);
            }
        }

        sleep((rand()%10)+10);
    }

    if(1 != success)
    {
        if(1 == type)
        {
            if(curl_code > 0)
            {
                g_bind_errno = curl_code;
            }
            else
            {
                g_bind_errno = BIND_ERRNO_RESET_FAIL;
            }
        }
        return 0;
    }

    return 1;
}

int webapi_do_reset(void)
{
    return webapi_do_reset_by_specified_server(g_cloud_info.g_mmap_info_ptr->api_server, 3, 1);
}

int webapi_do_login()
{
    char cmd[2048] = {0};
    char ret_string[10*1024] = {0};
    char code[16] = {0};
    char upload_url[512]={0};
    char app_param[512] = {0};
    char router_backup[256] = {0};
    char router_backup_enable[16] = {0};
    char router_backup_resolution[16] = {0};
    char router_backup_user_path[16] = {0};
    char router_backup_period[16] = {0};
    char css_flag[16] = {0};
    char css_mode[16] = {0};
    char css_image[16] = {0};
    #if !defined(NOT_PLT_API)
    char restartType[16] = {0};
    char restartTime[16] = {0};
    char restartDay[16] = {0};
    restart_timer_t restartTimer;
    char ai_face[16] = {0};
    #endif
    video_backup_state_set backup_state;
    //char url[256] = {0};
    //char md5[64] = {0};
    char tmp_ssid[128] = {};
    int success = 0;
    int trycnt = 0;
    char p2p_pwd[32] = {0};

    char curl_code_string[16] = {0};
    int curl_code = 0;

    struct timeval tv;
    unsigned int seed;
#ifdef ENABLE_4G
    char *mobile_server = NULL;
#endif

    gettimeofday(&tv, NULL);
    seed += ((int)tv.tv_usec+tv.tv_sec);
    srand(seed);

    if(1 == g_cloud_info.g_mmap_info_ptr->tmp_pwd_have_gen)
    {
        strncpy(p2p_pwd, g_cloud_info.g_mmap_info_ptr->tmp_pwd, sizeof(p2p_pwd));
        g_cloud_info.online_use_tmppwd = 1;
        dump_string(_F_, _FU_, _L_, "online with new p2p temp password");
    }
    else
    {
        strncpy(p2p_pwd, g_cloud_info.g_mmap_info_ptr->pwd, sizeof(p2p_pwd));
        g_cloud_info.online_use_tmppwd = 0;
    }

    dump_string(_F_, _FU_, _L_, "now do login \n");
#if defined(PRODUCT_B091QP)&&defined(ENABLE_4G)
    if(g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0')
    {
        if (g_cloud_info.g_mmap_info_ptr->sim_card.type == SIM_CARD_TYPE_CT) {
            mobile_server = "China Telecom";
        } else if (g_cloud_info.g_mmap_info_ptr->sim_card.type == SIM_CARD_TYPE_WO) {
            mobile_server = "China Unicom";
        } else {
            mobile_server = "China Mobile";
        }
        printf("mobile_server: %s, rssi: %ddBm\n", mobile_server, g_cloud_info.g_mmap_info_ptr->sim_card.rssi);
    }
#elif defined(ENABLE_4G)
    if (g_cloud_info.g_mmap_info_ptr->sim_card.type == SIM_CARD_TYPE_CT) {
        mobile_server = "China Telecom";
    } else if (g_cloud_info.g_mmap_info_ptr->sim_card.type == SIM_CARD_TYPE_WO) {
        mobile_server = "China Unicom";
    } else {
        mobile_server = "China Mobile";
    }
    printf("mobile_server: %s, rssi: %ddBm\n", mobile_server, g_cloud_info.g_mmap_info_ptr->sim_card.rssi);
#endif

    g_power_mode = g_cloud_info.g_mmap_info_ptr->power_mode;
#if defined(PRODUCT_B091QP)&&defined(ENABLE_4G)
    int signal_quality;
    if(g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0')
    {
        memcpy(tmp_ssid, mobile_server, strlen(mobile_server));
        signal_quality = g_cloud_info.g_mmap_info_ptr->sim_card.rssi;
    }
    else
    {
        convert_shell_char(g_cloud_info.g_mmap_info_ptr->ssid, tmp_ssid, sizeof(tmp_ssid));
        signal_quality=g_cloud_info.g_mmap_info_ptr->signal_quality;
    }
    snprintf(cmd, sizeof(cmd), "%s -c 138 -url \"%s/v4/ipc/on_line\" -key %s -keySec %s "
     "-uid %s "
     "-version %s "
     "-ssid \"%s\" "
     "-mac %s "
     "-ip %s "
     "-signal_quality %d "
     "-packetloss %d "
     "-p2pconnect %d "
     "-p2pconnect_success %d "
     "-tfstat %d "
     "-powerstate %d ",
     CLOUDAPI_PATH,
     g_cloud_info.g_mmap_info_ptr->api_server,
     p2p_pwd, g_cloud_info.g_mmap_info_ptr->key,
     g_cloud_info.g_mmap_info_ptr->p2pid,
     g_cloud_info.g_mmap_info_ptr->version,
     tmp_ssid,
     g_cloud_info.g_mmap_info_ptr->mac,
     g_cloud_info.g_mmap_info_ptr->ip,
     signal_quality,
     g_cloud_info.g_mmap_info_ptr->in_packet_loss,
     0,
     0,
     g_cloud_info.g_mmap_info_ptr->tf_status.stat,
     g_power_mode);
#else
    #ifdef ENABLE_4G
    memcpy(tmp_ssid, mobile_server, strlen(mobile_server));
    #else
    convert_shell_char(g_cloud_info.g_mmap_info_ptr->ssid, tmp_ssid, sizeof(tmp_ssid));
    #endif
    snprintf(cmd, sizeof(cmd), "%s -c 138 -url \"%s/v4/ipc/on_line\" -key %s -keySec %s "
             "-uid %s "
             "-version %s "
             "-ssid \"%s\" "
             "-mac %s "
             "-ip %s "
             "-signal_quality %d "
             "-packetloss %d "
             "-p2pconnect %d "
             "-p2pconnect_success %d "
             "-tfstat %d "
             "-powerstate %d ",
             CLOUDAPI_PATH,
             g_cloud_info.g_mmap_info_ptr->api_server,
             p2p_pwd, g_cloud_info.g_mmap_info_ptr->key,
             g_cloud_info.g_mmap_info_ptr->p2pid,
             g_cloud_info.g_mmap_info_ptr->version,
             tmp_ssid,
             g_cloud_info.g_mmap_info_ptr->mac,
             g_cloud_info.g_mmap_info_ptr->ip,
            #ifdef ENABLE_4G
             g_cloud_info.g_mmap_info_ptr->sim_card.rssi,
            #else
             g_cloud_info.g_mmap_info_ptr->signal_quality,
            #endif
             g_cloud_info.g_mmap_info_ptr->in_packet_loss,
             0,
             0,
             g_cloud_info.g_mmap_info_ptr->tf_status.stat,
             g_power_mode);
#endif
    dump_string(_F_, _FU_, _L_, "cmd = %s\n", cmd);

    success = 0;
    trycnt = 3;
    while(trycnt > 0)
    {
        trycnt-- ;
        memset_s(ret_string, sizeof(ret_string), 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset_s(code, sizeof(code), 0, sizeof(code));
        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE)
        {
            if(atoi(code) == 20000)
            {
                dump_string(_F_, _FU_, _L_, "webapi_do_login success %s\n", ret_string);
                if(trans_json_ex_s(upload_url, sizeof(upload_url), "\"uploadUrl\"", ret_string) == TRUE)
                {
                    if(g_cloud_info.g_mmap_info_ptr->region_id == REGION_CHINA)
                    {
                        webapi_do_log_upload();
                    }
                    debug_mod = 1;
#if 0
                    debug_mod = 1;
                    cloud_set_debug_mode();

                    memset(url, 0, sizeof(url));
                    memset(md5, 0, sizeof(md5));
                    if(need_update(url, md5) == 1)
                    {
                        do_update_ex(url, md5);
                    }
#endif
                }
                else
                {
                    debug_mod = 0;
                }

                if(trans_json_ex_s(css_flag, sizeof(css_flag), "\"css_flag\"", ret_string) == TRUE)
                {
                    if(atoi(css_flag) == 0)
                    {
                        cloud_set_cloud_storage_state(DISPATCH_SET_CLOUD_STORAGE_OFF);
                    }
                    else
                    {
                        cloud_set_cloud_storage_state(DISPATCH_SET_CLOUD_STORAGE_ON);
                    }
                }

                if(trans_json_ex_s(css_mode, sizeof(css_mode), "\"css_mode\"", ret_string) == TRUE)
                {
                    if(atoi(css_mode) == 0)
                    {
                        cloud_set_cloud_storage_mode(DISPATCH_SET_CLOUD_STORAGE_MODE_MOTION_DETECT);
                    }
                    else
                    {
                        cloud_set_cloud_storage_mode(DISPATCH_SET_CLOUD_STORAGE_MODE_ALL_DAY);
                    }
                }
                #if !defined(NOT_PLT_API)
                #if 0//code for test
                //cloud_set_restart_type(DISPATCH_SET_RESTART_TYPE_AI,NULL,0);
                restartTimer.restartTime=1950;
                restartTimer.restartDayInWeek[0]=2;
                restartTimer.restartDayInWeek[1]=3;
                cloud_set_restart_type(DISPATCH_SET_RESTART_TYPE_TIMER,&restartTimer,sizeof(restartTimer)); 
                #endif                
                if(trans_json_ex_s(restartType, sizeof(restartType), "\"restartTpye\"", ret_string) == TRUE)
                {
                    int restart_type=atoi(restartType);
                    if(restart_type == 1)
                    {
                        //AI restart
                        cloud_set_restart_type(DISPATCH_SET_RESTART_TYPE_AI,NULL,0);
                    }
                    else if(restart_type == 2)
                    {
                        if(trans_json_ex_s(restartTime, sizeof(restartTime), "\"restartTime\"", ret_string) == TRUE)
                        {
                            int hours,mins;
                            sscanf(restartTime,"%d:%d",&hours,&mins);
                            restartTimer.restartTime=hours*100 + mins;
                            if(trans_json_ex_s(restartDay, sizeof(restartDay), "\"repeatCycle\"", ret_string) == TRUE)
                            {
                                char *p=restartDay;
                                int week_day=0;
                                memset(restartTimer.restartDayInWeek,0,sizeof(restartTimer.restartDayInWeek));
                                for(;*p;p++)
                                {
                                    if((*p>'0')&&(*p<'8'))
                                    {
                                        restartTimer.restartDayInWeek[week_day++] = *p - '0';//ex."1,2,5,7"==>1 2 5 7 0 0 0
                                    }
                                }
                                restartTimer.restartAiNextTime = 0;
                                //timer restart
                                cloud_set_restart_type(DISPATCH_SET_RESTART_TYPE_TIMER,&restartTimer,sizeof(restartTimer));  
                            }
                        }
                    }
                }
                    #ifdef HAVE_FEATURE_FACE
                    if(trans_json_ex_s(ai_face, sizeof(ai_face), "\"ai_face\"", ret_string) == TRUE)
                    {
                        int is_face_enable=(atoi(ai_face)==1)?1:0;
                        cloud_set_human_face (is_face_enable);
                    }
                    else
                    {
                        cloud_set_human_face (0);
                    }
                    #endif
                #endif
                if(trans_json_ex_s(css_image, sizeof(css_image), "\"css_image_flag\"", ret_string) == TRUE)
                {
                    if(atoi(css_image) == 1)
                    {
                        cloud_set_cloud_image_state(DISPATCH_SET_CLOUD_IMAGE_ON);
                    }
                    else
                    {
                        cloud_set_cloud_image_state(DISPATCH_SET_CLOUD_IMAGE_OFF);
                    }
                }

                if(trans_json_ex_s(app_param, sizeof(app_param), "\"appParam\"", ret_string) == TRUE)
                {
                    if(trans_json_ex_s(router_backup, sizeof(router_backup), "\"router_backup\"", app_param) == TRUE)
                    {
                        if(trans_json_ex_s(router_backup_enable, sizeof(router_backup_enable), "\"enable\"", router_backup) == TRUE &&
                           trans_json_ex_s(router_backup_resolution, sizeof(router_backup_resolution), "\"resolution\"", router_backup) == TRUE &&
                           trans_json_ex_s(router_backup_user_path, sizeof(router_backup_user_path), "\"user_path\"", router_backup) == TRUE &&
                           trans_json_ex_s(router_backup_period, sizeof(router_backup_period), "\"backup_period\"", router_backup) == TRUE)
                        {
                            backup_state.enable = atoi(router_backup_enable);
                            backup_state.resolution = atoi(router_backup_resolution);
                            backup_state.user_path = atoi(router_backup_user_path);
                            backup_state.backup_period = atoi(router_backup_period);
                            cloud_set_video_backup_state(&backup_state);
                        }
                    }
                }

                success = 1;
                break;
            }
            else if(atoi(code) == 20241)
            {
                if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
                {
                    g_bind_errno = BIND_ERRNO_ONLINE_ERROR;
                }

                if(g_cloud_info.g_mmap_info_ptr->region_id != REGION_CHINA)
                {
                    if(webapi_do_check_region() == 1)
                    {
                        sleep(1);
                        return 0;
                    }
                }
                else
                {
                    dump_string(_F_, _FU_, _L_, "webapi_do_reset fail %s\n", ret_string);
                    //cloud_set_bind_result(DISPATCH_SET_BIND_FAIL);
                    return 0;
                }
            }
        }

        if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
        {
            if(trans_json_ex_s(curl_code_string, sizeof(curl_code_string), "\"curl_code\"", ret_string) == TRUE)
            {
                curl_code = atoi(curl_code_string);
            }
            #if defined(PRODUCT_B091QP)&&defined(ENABLE_4G)&&defined(ENABLE_SCANE_4G_DEVICE)
            if(g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0')
            {
                return 1;
            }
            #elif defined(ENABLE_4G)&&defined(ENABLE_SCANE_4G_DEVICE)
                return 1;
            #endif
        }

        sleep((rand()%10)+10);
    }

    if((g_cloud_info.g_mmap_info_ptr->start_with_reset == 1) && (1 != success))
    {
        if(curl_code > 0)
        {
            g_bind_errno = curl_code;
        }
        else
        {
            g_bind_errno = BIND_ERRNO_ONLINE_FAIL;
        }
    }

    return success;
}

int webapi_do_bindkey(void)
{
    char ret_string[2048] = {0};
    char cmd[512] = {0};
    char code[16] = {0};
    int success = 0;
    int trycnt = 0;

    char curl_code_string[16] = {0};
    int curl_code = 0;

    struct timeval tv;
    unsigned int seed;

    gettimeofday(&tv, NULL);
    seed += ((int)tv.tv_usec+tv.tv_sec);
    srand(seed);

    dump_string(_F_, _FU_, _L_, "now do bindkey \n");
    #if defined(AP_MODE)&&defined(PRODUCT_B091QP)
    if(strlen(g_cloud_info.g_mmap_info_ptr->bind_key)>32)
    {
            sprintf(cmd, "%s -c 137 -key %s -keySec %s -url %s/v5/cut/bind "
            "-uid %s ",
            CLOUDAPI_PATH,
            g_cloud_info.g_mmap_info_ptr->bind_key, 
            g_cloud_info.g_mmap_info_ptr->key,
            g_cloud_info.g_mmap_info_ptr->api_server,
            g_cloud_info.g_mmap_info_ptr->p2pid);
    }else
    {
        sprintf(cmd, "%s -c 137 -key %s -keySec %s -url %s/v4/ipc/qr_bind "
                "-uid %s ",
                CLOUDAPI_PATH,
                g_cloud_info.g_mmap_info_ptr->bind_key, 
                g_cloud_info.g_mmap_info_ptr->key,
                g_cloud_info.g_mmap_info_ptr->api_server,
                g_cloud_info.g_mmap_info_ptr->p2pid);

    }
    #else
    sprintf(cmd, "%s -c 137 -key %s -keySec %s -url %s/v4/ipc/qr_bind "
            "-uid %s ",
            CLOUDAPI_PATH,
            g_cloud_info.g_mmap_info_ptr->bind_key, 
            g_cloud_info.g_mmap_info_ptr->key,
            g_cloud_info.g_mmap_info_ptr->api_server,
            g_cloud_info.g_mmap_info_ptr->p2pid);
    #endif
    dump_string(_F_, _FU_, _L_, "cmd = %s\n", cmd);

    success = 0;
    trycnt = 3;
    while(trycnt>0)
    {
        trycnt-- ;

        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE)
        {
            if(atoi(code) == 20000)
            {
                cloud_set_bind_result(DISPATCH_SET_BIND_SUCCESS);
                dump_string(_F_, _FU_, _L_, "webapi_do_bindkey success %s\n", ret_string);
                success = 1;
                break;
            }
            else if(atoi(code) == 20219)
            {
                g_bind_errno = BIND_ERRNO_KEY_INVALID;
                cloud_set_bind_result(DISPATCH_SET_BIND_FAIL);
                dump_string(_F_, _FU_, _L_, "webapi_do_bindkey fail %s\n", ret_string);
                return 2;
            }
        }

        if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
        {
            if(trans_json_ex_s(curl_code_string, sizeof(curl_code_string), "\"curl_code\"", ret_string) == TRUE)
            {
                curl_code = atoi(curl_code_string);
            }
        }

        sleep((rand()%10)+10);
    }

    if(1 != success)
    {
        if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
        {
            if(curl_code > 0)
            {
                g_bind_errno = curl_code;
            }
            else
            {
                g_bind_errno = BIND_ERRNO_DO_BIND_FAIL;
            }
        }
        return 0;
    }

    return 1;
}

int webapi_do_event_update(char *jpg_upload_url, char *mp4_upload_url, char *jpg_pwd, char *mp4_pwd, int type, int sub_type, time_t event_time, int face_count, int is_upload_video, char *event_uuid)
{
    char cmd[1024] = {0};
    char ret_string[2048] = {0};
    char code[16] = {0};
    char timestring[32] = {0};
    char event_stat[8] = {0};
    int retval = 0;
    int event_silent_interval = 600;
    int trycnt = 0;
    
#ifdef PRODUCT_H31BG
    #define VIDEO_SOURCE_DURATION_ENABLE    1
    #ifdef VIDEO_SOURCE_DURATION_ENABLE   
    int video_source = 0;
    int video_duration = 60;
    if(g_cloud_info.g_mmap_info_ptr->cloud_storage_enable == 1 && g_cloud_info.g_mmap_info_ptr->sdcard_record_enable != 1)
    {
        video_source = (video_source | 0x01); //cloud storage
    }

    if(g_cloud_info.g_mmap_info_ptr->is_sd_exist == 1)
    {
        if(g_cloud_info.g_mmap_info_ptr->cloud_storage_enable != 1 && g_cloud_info.g_mmap_info_ptr->sdcard_record_enable == 1)
        {
            video_source = (video_source | 0x02); // sd storage
        }
        if(g_cloud_info.g_mmap_info_ptr->cloud_storage_enable == 1 && g_cloud_info.g_mmap_info_ptr->sdcard_record_enable == 1)
        {
            video_source = (video_source | 0x03); // cloud storage + sd storage
        }
    }
    #endif //#ifdef VIDEO_SOURCE_DURATION_ENABLE    
#endif // #ifdef PRODUCT_H31BG

    trycnt = 3;
    while(trycnt > 0)
    {
        trycnt-- ;
        if(g_cloud_info.g_mmap_info_ptr->sd_leftsize> 0)
        {
            strncpy(event_stat, "true", sizeof(event_stat));
        }
        else
        {
            strncpy(event_stat, "false", sizeof(event_stat));
        }
#ifdef VIDEO_SOURCE_DURATION_ENABLE        
    #ifdef HAVE_FEATURE_FACE
        snprintf(cmd, sizeof(cmd), "%s -c 304 -url \"%s/v4/alert/event\" -uid %s -keySec %s -EventTime %lu -EventStat %s -pic_url \"%s\" -video_url \"%s\" -pic_pwd \"%s\" -video_pwd \"%s\" -type %d -sub_type %d -face_count %d -is_upload_video %d -video_source %d -event_uuid \"%s\" ",
                 CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->key, event_time, event_stat, jpg_upload_url, mp4_upload_url, jpg_pwd, mp4_pwd, type, sub_type, face_count, is_upload_video, video_source, event_uuid);
    #else
        snprintf(cmd, sizeof(cmd), "%s -c 304 -url \"%s/v4/alert/event\" -uid %s -keySec %s -EventTime %lu -EventStat %s -pic_url \"%s\" -video_url \"%s\" -pic_pwd \"%s\" -video_pwd \"%s\" -type %d -sub_type %d -video_source %d -event_uuid \"%s\"",
                 CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->key, event_time, event_stat, jpg_upload_url, mp4_upload_url, jpg_pwd, mp4_pwd, type, sub_type, video_source, event_uuid);
    #endif  //HAVE_FEATURE_FACE
#else
    #ifdef HAVE_FEATURE_FACE
        snprintf(cmd, sizeof(cmd), "%s -c 304 -url \"%s/v4/alert/event\" -uid %s -keySec %s -EventTime %lu -EventStat %s -pic_url \"%s\" -video_url \"%s\" -pic_pwd \"%s\" -video_pwd \"%s\" -type %d -sub_type %d -face_count %d -is_upload_video %d ",
                 CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->key, event_time, event_stat, jpg_upload_url, mp4_upload_url, jpg_pwd, mp4_pwd, type, sub_type, face_count, is_upload_video);
    #else
        snprintf(cmd, sizeof(cmd), "%s -c 304 -url \"%s/v4/alert/event\" -uid %s -keySec %s -EventTime %lu -EventStat %s -pic_url \"%s\" -video_url \"%s\" -pic_pwd \"%s\" -video_pwd \"%s\" -type %d -sub_type %d ",
                 CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->key, event_time, event_stat, jpg_upload_url, mp4_upload_url, jpg_pwd, mp4_pwd, type, sub_type);
    #endif  //HAVE_FEATURE_FACE
#endif  //#ifdef VIDEO_SOURCE_DURATION_ENABLE


        dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

        memset(ret_string, 0, sizeof(ret_string));
        int ret = 0;
        ret = system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "ret = %d\n", ret);
        //logw("cmd[%s]\nret[%s]",cmd,ret_string);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            dump_string(_F_, _FU_, _L_, "do_cloud_event_update success %s\n", ret_string);

            if((3!=sub_type) && (5!=sub_type))
            {
                if(trans_json_ex_s(timestring, sizeof(timestring), "\"ipcSetTime\"", ret_string) != TRUE)
                {
                    dump_string(_F_, _FU_, _L_, "got trans_json_ex_s ipcSetTime fail\n");
                    if(alert_retry_interval == -1)
                    {
                        g_cloud_info.event_timeout = g_cloud_info.g_mmap_info_ptr->systick + event_silent_interval*10;
                    }
                }
                else
                {
                    if(alert_retry_interval == -1)
                    {
                        #ifdef PRODUCT_H31BG
                        //g_timestring = atoi(timestring);
                        g_cloud_info.event_timeout	= g_cloud_info.event_tm + IPC_SET_TIME;
                        #else
                        g_cloud_info.event_timeout	= g_cloud_info.g_mmap_info_ptr->systick +  atoi(timestring)*10;
                        #endif
                    }
                }
            }

            retval = 1;
            break;
        }
        ms_sleep(1000*3);
    }

    return retval;
}

int webapi_do_event_upload(char *upload_url, char *local_file)
{
    char cmd[1024] = {0};
    char buf[1024] = {0};
    char access_key[128] = {0};
    int try_cnt = 0;
    int success = -1;
    struct timeval tv;
    unsigned int seed;

    gettimeofday(&tv, NULL);
    seed += ((int)tv.tv_usec+tv.tv_sec);
    srand(seed);

    if(upload_url == NULL || local_file == NULL)
    {
        return -1;
    }

    printf("upload_url = %s, local_file = %s\n", upload_url, local_file);

    snprintf(cmd, sizeof(cmd), "%s -c 411 -url \"%s\" -filename %s",
             CLOUDAPI_PATH, upload_url, local_file);
    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

    if(alert_max_retry != -1)
    {
        try_cnt = alert_max_retry;
    }
    else
    {
        try_cnt = 3;
    }

    while(try_cnt > 0)
    {
        try_cnt--;
        memset(buf, 0, sizeof(buf));
        system_cmd_withret_timeout(cmd, buf, sizeof(buf), 60);
        logw("cmd[%s]\nret[%s]",cmd,buf);

        if(trans_json_ex_s(access_key, sizeof(access_key), "\"accessKeyId\"", buf) == TRUE)
        {
            memset(cmd, 0, sizeof(cmd));
            snprintf(cmd, sizeof(cmd), "rm -f %s", local_file);
            dump_string(_F_, _FU_, _L_,  "cmd=%s\n", cmd);
            system(cmd);
            success = 1;
            break;
        }

        if(alert_retry_interval != -1)
        {
            usleep(alert_retry_interval*1000);
        }
        else
        {
            sleep(rand()%5);
        }
    }

    return success;
}

#ifdef ENABLE_4G
int webapi_do_report_iccid()
{
    char cmd[1024] = {0};
    char buf[1024] = {0};
    char code[16] = {0};
    char ret_string[10*1024] = {0};
    int success = 0;
    sprintf(cmd, "%s -c 416 -url %s/vas/v8/ipc/4g/card -keySec %s -iccid %s -uid %s",
            CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->key, g_cloud_info.g_mmap_info_ptr->sim_card.iccid, g_cloud_info.g_mmap_info_ptr->p2pid);
    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);
    int trycnt = 3;
    while(trycnt > 0)
    {
        trycnt-- ;
        memset_s(ret_string, sizeof(ret_string), 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset_s(code, sizeof(code), 0, sizeof(code));
        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE)
        {
            if(atoi(code) == 20000)
            {
                dump_string(_F_, _FU_, _L_, "webapi_do_report_iccid success %s\n", ret_string);
                success = 1;
                break;
            }
        }
        sleep((rand()%5)+2);
    }
    return success;
}
#endif
int webapi_do_log_upload()
{
    char cmd[1024] = {0};
    char buf[1024] = {0};
    int success = 0;
    snprintf(cmd, sizeof(cmd), "%s -c 413 -url \"%s\" -filename %s -device_id %s&",
             CLOUDAPI_PATH, "https://hch.xiaoyi.com:9443/upload/log", "/tmp/log.txt", g_cloud_info.g_mmap_info_ptr->p2pid);
    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);
    system_cmd_withret_timeout(cmd, buf, sizeof(buf), 60);
    if(strstr(buf, "UploadLog.Success") != NULL)
    {
        dump_string(_F_, _FU_, _L_, "Upload ok!\n");
        success = 1;
    }
    return success;
}
int webapi_do_check_did()
{
    char cmd[1024] = {0};
    char ret_string[2048] = {0};
    char allow[32] = {0};
    char code[16] = {0};
    int trycnt = 0;

    snprintf(cmd, sizeof(cmd), "%s -c 311 -url %s/v4/ipc/check_did -uid %s -keySec %s", CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->key);

    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

    trycnt = 2;
    while(trycnt>0)
    {
        trycnt-- ;
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE)
        {
            if(atoi(code) != 20000)
            {
                return 0;
            }

            memset(allow, 0, sizeof(allow));
            if(trans_json_ex_s(allow, sizeof(allow), "\"allow\"", ret_string) == TRUE)
            {
                if(strcmp(allow, "false") == 0)
                {
                    g_bind_errno = BIND_ERRNO_DEVICE_NOT_ALLOWED;
                    return -1;
                }
                else if(strcmp(allow, "true") == 0)
                {
                    return 0;
                }
            }
        }
        sleep(3);
    }

    return 0;
}

static int parse_schedule_power(char *str,schedule_power_t *info)
{
    char value[128]={0};

    if( !info )
        return -1;

    if( trans_json_ex_s(value, sizeof(value), "\"enable\"", str) != TRUE )
    {
        logw("get enable error");
        return -1;
    }

    info->enable = atoi(value);
    if( info->enable )
    {
        char repeater[128]={0},time[128]={0};
        if( trans_json_ex_s(repeater, sizeof(repeater), "\"repeater\"",str ) != TRUE || trans_json_ex_s(time, sizeof(time), "\"time\"", str) != TRUE )
            return -1;
        //repeater format "1,2,3"
        int n=0,rep[7] = {0};
        n = sscanf(repeater,"%d,%d,%d,%d,%d,%d,%d",&rep[0],&rep[1],&rep[2],&rep[3],&rep[4],&rep[5],&rep[6]);
        if( n>0 )
        {
            int i;
            for(i=0;i<n;i++)
            {
                if( rep[i]>=1&&rep[i]<=7 )
                {
                    info->repeat |= (1<<((rep[i]-1)%7));
                }
            }
        }

        //time format yymmddhhmm
        unsigned int t = atoi(time);
        struct tm tm={0};
        tm.tm_year = (t/100000000)+100;
        tm.tm_mon = ((t%100000000)/1000000)-1;
        tm.tm_mday = (t%1000000)/10000;
        tm.tm_hour = (t%10000)/100;
        tm.tm_min = t%100;

        logw("repeater:%s,t:%u,time:%02d-%02d-%02d %02d:%02d",repeater,t,tm.tm_year-100,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min);
        info->time = mktime(&tm);
    }
    logw("schedule_power info:enable=%d,repeater=%d,time=%d",info->enable,info->repeat,info->time);
    return 0;
}
#define ALERT_SCHEDULE_MAX_NUM 10
static int cloud_set_schedule_power(schedule_power_info_t *pwr_info)
{
    int count_down = 0;

    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_SCHEDULE_POWER, (char *)pwr_info, sizeof(schedule_power_info_t)) < 0)
    {
        dump_string(_F_, _FU_, _L_, "p2p_set_video_backup_state fail!\n");
        return -1;
    }

    count_down = 10;
    while(count_down)
    {
        count_down--;
        if( 0 == memcmp(&g_cloud_info.g_mmap_info_ptr->schedule_power,pwr_info,sizeof(schedule_power_info_t)) )
        {
            break ;
        }
        usleep(100*1000);
    }

    return 0;
}

static int parse_alert_schedule(char *str,alert_schedule_t *info)
{
    //char value[8]={0};
    cJSON           *timeperiod;
    cJSON           *timeperiods;
    cJSON           *timeperiod_list;
    cJSON           *enable,*onceday,*repeatday,*starttime,*endtime;
    int i = 0;

    timeperiod_list = cJSON_Parse(str);
    if(!cJSON_IsObject(timeperiod_list))
    {
        info->alert_schedule_num = 1;
        return -1;
    }


    timeperiod = cJSON_GetObjectItemCaseSensitive(timeperiod_list, "timeperiod");
    cJSON_ArrayForEach(timeperiods,timeperiod)
    {
        enable = cJSON_GetObjectItemCaseSensitive(timeperiods, "enable");
        if (!cJSON_IsNumber(enable))
        {
            printf("Checking enable \"%d\"\n", enable->valueint);
        }
        if(enable->valueint == 1)
        {
            info->alert_schedule_info[i].enable = enable->valueint;
            onceday = cJSON_GetObjectItemCaseSensitive(timeperiods, "onceday");
            //if (onceday->valuestring != NULL)
            {
                strcpy(info->alert_schedule_info[i].onceday,onceday->valuestring);
                dump_string(_F_, _FU_, _L_, "onceday=%s\n", info->alert_schedule_info[i].onceday);
            }
            repeatday = cJSON_GetObjectItemCaseSensitive(timeperiods, "repeatday");
            dump_string(_F_, _FU_, _L_, "repeatday=%s\n", cJSON_Print(repeatday));
            dump_string(_F_, _FU_, _L_, "repeatday=%s\n", repeatday->valuestring);
            //if(repeatday->valuestring != NULL)
            {
                int n=0,rep[7] = {0};
                n = sscanf(repeatday->valuestring,"%d,%d,%d,%d,%d,%d,%d",&rep[0],&rep[1],&rep[2],&rep[3],&rep[4],&rep[5],&rep[6]);
                if( n>0 )
                {
                    int j;
                    for(j=0;j<n;j++)
                    {
                        if( rep[j]>=1&&rep[j]<=7 )
                        {
                            info->alert_schedule_info[i].repeatday |= (1<<((rep[j]-1)%7));
                        }
                    }
                }
            }

            starttime = cJSON_GetObjectItemCaseSensitive(timeperiods, "starttime");
            endtime = cJSON_GetObjectItemCaseSensitive(timeperiods, "endtime");

            {
                //time format yymmddhhmm
                unsigned int start_time = atoi(starttime->valuestring);
                unsigned int end_time = atoi(endtime->valuestring);

                info->alert_schedule_info[i].starttime = start_time;
                info->alert_schedule_info[i].endtime = end_time;

                dump_string(_F_, _FU_, _L_, "starttime=%d\n", info->alert_schedule_info[i].starttime);
                dump_string(_F_, _FU_, _L_, "endtime=%d\n", info->alert_schedule_info[i].endtime);
            }
            i++;
        }
        info->alert_schedule_num = i;
        dump_string(_F_, _FU_, _L_, "alert_schedule_num=%d\n", info->alert_schedule_num);
    }
    cJSON_Delete(timeperiod_list);
    return 0;
}

static int cloud_set_alert_schedule(alert_schedule_t *alert_schedule)
{
    int count_down = 0;
    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_ALERT_SCHEDULE, (char *)alert_schedule, sizeof(alert_schedule_t)) < 0)
    //if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_ALERT_SCHEDULE, NULL, 0) < 0)
    {
        dump_string(_F_, _FU_, _L_, "cloud_set_alert_schedule fail!\n");
        return -1;
    }
    return 0;
}

int webapi_get_dev_info()
{
    char cmd[1024] = {0};
    char ret_string[2048] = {0};
    char code[16] = {0};
    char tz_offset[16] = {0};
    char data[1024] = {0};
    char app_param[512] = {0};
    char router_backup[256] = {0};
    char router_backup_enable[16] = {0};
    char router_backup_resolution[16] = {0};
    char router_backup_user_path[16] = {0};
    char router_backup_period[16] = {0};
    char css_flag[16] = {0};
    char css_mode[16] = {0};
    char css_image[16] = {0};
    video_backup_state_set backup_state;
    int trycnt = 0;
    int success = 0;

    snprintf(cmd, sizeof(cmd), "%s -c 142 -url %s/v4/ipc/deviceinfo -uid %s -keySec %s", CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->key);

    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

    trycnt = 3;
    while(trycnt>0)
    {
        trycnt-- ;
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            memset(data, 0, sizeof(data));
            if(trans_json_ex_s(data, sizeof(data), "\"data\"", ret_string) == TRUE)
            {
                if(trans_json_ex_s(tz_offset, sizeof(tz_offset), "\"tz_offset\"", data) == TRUE)
                {
                    cloud_set_tz_offset(atoi(tz_offset));
                }

                if(trans_json_ex_s(css_flag, sizeof(css_flag), "\"css_flag\"", data) == TRUE)
                {
                    if(atoi(css_flag) == 0)
                    {
                        cloud_set_cloud_storage_state(DISPATCH_SET_CLOUD_STORAGE_OFF);
                    }
                    else
                    {
                        cloud_set_cloud_storage_state(DISPATCH_SET_CLOUD_STORAGE_ON);
                    }
                }

                if(trans_json_ex_s(css_mode, sizeof(css_mode), "\"css_mode\"", data) == TRUE)
                {
                    if(atoi(css_mode) == 0)
                    {
                        cloud_set_cloud_storage_mode(DISPATCH_SET_CLOUD_STORAGE_MODE_MOTION_DETECT);
                    }
                    else
                    {
                        cloud_set_cloud_storage_mode(DISPATCH_SET_CLOUD_STORAGE_MODE_ALL_DAY);
                    }
                }

                if(trans_json_ex_s(css_image, sizeof(css_image), "\"css_image_flag\"", data) == TRUE)
                {
                    if(atoi(css_image) == 1)
                    {
                        cloud_set_cloud_image_state(DISPATCH_SET_CLOUD_IMAGE_ON);
                    }
                    else
                    {
                        cloud_set_cloud_image_state(DISPATCH_SET_CLOUD_IMAGE_OFF);
                    }
                }

                if(trans_json_ex_s(app_param, sizeof(app_param), "\"appParam\"", data) == TRUE)
                {
                    if(trans_json_ex_s(router_backup, sizeof(router_backup), "\"router_backup\"", app_param) == TRUE)
                    {
                        if(trans_json_ex_s(router_backup_enable, sizeof(router_backup_enable), "\"enable\"", router_backup) == TRUE &&
                           trans_json_ex_s(router_backup_resolution, sizeof(router_backup_resolution), "\"resolution\"", router_backup) == TRUE &&
                           trans_json_ex_s(router_backup_user_path, sizeof(router_backup_user_path), "\"user_path\"", router_backup) == TRUE &&
                           trans_json_ex_s(router_backup_period, sizeof(router_backup_period), "\"backup_period\"", router_backup) == TRUE)
                        {
                            backup_state.enable = atoi(router_backup_enable);
                            backup_state.resolution = atoi(router_backup_resolution);
                            backup_state.user_path = atoi(router_backup_user_path);
                            backup_state.backup_period = atoi(router_backup_period);
                            cloud_set_video_backup_state(&backup_state);
                        }
                    }
                    char schedule_power[1024]={0};
                    if(trans_json_ex_s(schedule_power, sizeof(schedule_power), "\"schedule_power\"", app_param) == TRUE)
                    {
                        char schedule_pwr_str[512]={0};
                        schedule_power_info_t pwr_info;
                        bzero(&pwr_info,sizeof(pwr_info));
                        logw("schedule_power:%s",schedule_power);
                        if(trans_json_ex_s(schedule_pwr_str, sizeof(schedule_pwr_str), "\"schedule_power_on\"", schedule_power) == TRUE)
                        {
                            logw("schedule_pwron_str:%s",schedule_pwr_str);
                            parse_schedule_power(schedule_pwr_str,&pwr_info.pwr_on);
                        }

                        bzero(schedule_pwr_str,sizeof(schedule_pwr_str));
                        if(trans_json_ex_s(schedule_pwr_str, sizeof(schedule_pwr_str), "\"schedule_power_off\"", schedule_power) == TRUE)
                        {
                            logw("schedule_pwroff_str:%s",schedule_pwr_str);
                            parse_schedule_power(schedule_pwr_str,&pwr_info.pwr_off);
                        }
                        cloud_set_schedule_power(&pwr_info);
                    }
                }

                char *timeperiod = NULL;
                char timeperiod_total[1024] = {0};
                alert_schedule_t alert_schedule;
                memset(&alert_schedule,0 , sizeof(alert_schedule_t));

                timeperiod = strstr(data,"timeperiod");
                dump_string(_F_, _FU_, _L_, "timeperiod=%s\n", timeperiod);
                sprintf(timeperiod_total,"{\"%s}",timeperiod);
                dump_string(_F_, _FU_, _L_, "timeperiod_total=%s\n", timeperiod_total);
                parse_alert_schedule(timeperiod_total,&alert_schedule);
                cloud_set_alert_schedule(&alert_schedule);
            }

            success = 1;
            break;
        }
        sleep(3);
    }

    return success;
}

void sync_alert_setting()
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_SYNC_ALERT_SETTING, -1, send_buf, &send_len);
    miio_send(send_buf, send_len, 1);

    return;
}

void props()
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_PROPS, -1, send_buf, &send_len);
    miio_send(send_buf, send_len, 1);

    return;
}

void verify_code()
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_VERIFY_CODE, -1, send_buf, &send_len);
    miio_send(send_buf, send_len, 1);

    return;
}

void otc_info(int id)
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_OTC_INFO, id, send_buf, &send_len);
    miio_send(send_buf, send_len, 1);

    return;
}

void miio_info(int id)
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_MIIO_INFO, id, send_buf, &send_len);
    miio_send(send_buf, send_len, 0);

    return;
}

void keep_alive()
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_KEEPALIVE, -1, send_buf, &send_len);
    miio_send(send_buf, send_len, 0);

    return;
}

void prop_sd_size()
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_PROP_SD_SIZE, -1, send_buf, &send_len);
    miio_send(send_buf, send_len, 0);

    return;
}

void prop_sd_avail()
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_PROP_SD_AVAIL, -1, send_buf, &send_len);
    miio_send(send_buf, send_len, 0);

    return;
}

void prop_visitors()
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet(MIIO_METHOD_PROP_VISITORS, -1, send_buf, &send_len);
    miio_send(send_buf, send_len, 0);

    return;
}

void get_upload_url(int need_jpg, int need_mp4, int no_alert_check)
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet_get_upload_url(-1, need_jpg, need_mp4, no_alert_check, send_buf, &send_len);
    miio_send(send_buf, send_len, 1);

    return;
}

void event_motion(char *jpg_obj_name, char *mp4_ojb_name, char *jpg_pwd, char *mp4_pwd)
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet_event_motion(-1, jpg_obj_name, mp4_ojb_name, jpg_pwd, mp4_pwd, send_buf, &send_len);
    miio_send(send_buf, send_len, 0);

    return;
}

void event_snapshot(char *jpg_obj_name, char *mp4_ojb_name, char *jpg_pwd, char *mp4_pwd)
{
    char send_buf[2048] = {0};
    int send_len = 0;

    construct_packet_event_snapshot(-1, jpg_obj_name, mp4_ojb_name, jpg_pwd, mp4_pwd, send_buf, &send_len);
    miio_send(send_buf, send_len, 0);

    return;
}

int first_otc_info()
{
    int cnt_down = 0;

    otc_info(-1);
    cnt_down = 150;
    while(cnt_down)
    {
        cnt_down--;
        service_handle();
        if(first_otc_info_over)
        {
            dump_string(_F_, _FU_, _L_,  "first_otc_info ok!\n");
            return 0;
        }
    }

    exit(0);
    dump_string(_F_, _FU_, _L_,  "first_otc_info fail!\n");
    return -1;
}

int sync_props()
{
    int cnt_down = 0;

    props();
    cnt_down = 150;
    while(cnt_down)
    {
        cnt_down--;
        service_handle();
        if(sync_props_over)
        {
            dump_string(_F_, _FU_, _L_,  "sync_props ok!\n");
            return 0;
        }
    }

    dump_string(_F_, _FU_, _L_,  "sync_props fail!\n");
    return -1;
}

void cloud_debug_log(int error, char *message)
{
    time_t cur_time = time(NULL);
    static time_t pre_debug_time = 0;
    static time_t error_message_time = 0;

    debug_string(DEBUG_INFO_TYPE_ALARM_E, "mark=alarmtrack,time=%ld,%s", cur_time, message);

    if(0 == error)
    {
        if((error_message_time > 0) && (cur_time > (error_message_time + 1800)))
        {
            if(0 == cloud_set_debug_info())
            {
                error_message_time = 0;
            }
        }
    }
    else
    {
        if(cur_time > (pre_debug_time + 1800))
        {
            if(0 == cloud_set_debug_info())
            {
                error_message_time = 0;
                pre_debug_time = cur_time;
            }
        }
        else
        {
            if(0 == error_message_time)
            {
                error_message_time = cur_time;
            }
        }
    }
}

void cloud_crypt_file(char *src_file, char *dst_file, char *key)
{
    char cmd[512] = {0};

    sprintf(cmd, "/home/app/crypt_file -e %s %s %s", src_file, dst_file, key);

    system(cmd);

    return;
}

void *motion_push_proc()
{
    char *recv_buf = motion_upload_url;
    char mp4_part[1024] = {0};
    char jpg_part[1024] = {0};
    char mp4_upload_url[512] = {0};
    char jpg_upload_url[512] = {0};
    char mp4_obj_name[256] = {0};
    char jpg_obj_name[256] = {0};
    char mp4_pwd[64] = {0};
    char jpg_pwd[64] = {0};
    char alert_max_retry_str[16] = {0};
    char alert_retry_interval_str[16] = {0};
    char alert_interval_str[16] = {0};
    char pic_name[64] = {0};
    char video_name[64] = {0};
    char pic_crypt_name[64] = {0};
    char video_crypt_name[64] = {0};
    char cmd[256] = {0};
    int cnt_down = 100;
    int pic_ok = -1, mp4_ok = -1;
    int type = 0, sub_type = 0;
    int ret = -1;

    pthread_detach(pthread_self());

    dump_string(_F_, _FU_, _L_, "motion_push_proc ok");

    type = 0;
    if(g_cloud_info.g_mmap_info_ptr->motion_type == 0)
    {
        sub_type = 1;			//�ƶ�
    }
    else
    {
        sub_type = 4;			//�ƶ�����
    }

    snprintf(pic_crypt_name, sizeof(pic_crypt_name), "%s.crypt", MOTION_PIC);
    snprintf(video_crypt_name, sizeof(video_crypt_name), "%s.crypt", MOTION_VIDEO);

    //snprintf(cmd, sizeof(cmd), "rm -f %s && rm -f %s && rm -f %s && rm -f %s", MOTION_PIC, MOTION_VIDEO, pic_crypt_name, video_crypt_name);
    snprintf(cmd, sizeof(cmd), "rm -f %s* %s*", MOTION_PIC, MOTION_VIDEO);

    while(1)
    {
        memset(mp4_part, 0, sizeof(mp4_part));
        memset(jpg_part, 0, sizeof(jpg_part));
        memset(mp4_upload_url, 0, sizeof(mp4_upload_url));
        memset(jpg_upload_url, 0, sizeof(jpg_upload_url));
        memset(mp4_obj_name, 0, sizeof(mp4_obj_name));
        memset(jpg_obj_name, 0, sizeof(jpg_obj_name));
        memset(alert_max_retry_str, 0, sizeof(alert_max_retry_str));
        memset(alert_retry_interval_str, 0, sizeof(alert_retry_interval_str));
        memset(alert_interval_str, 0, sizeof(alert_interval_str));
        memset(pic_name, 0, sizeof(pic_name));
        memset(video_name, 0, sizeof(video_name));
        memset(mp4_pwd, 0, sizeof(mp4_pwd));
        memset(jpg_pwd, 0, sizeof(jpg_pwd));
        cnt_down = 100;
        pic_ok = -1;
        mp4_ok = -1;

        sem_wait(&motion_sem);

        do_motion_push = 1;

        if(trans_json_ex_s(jpg_part, sizeof(jpg_part), "\"jpg\"", recv_buf) == TRUE)
        {
            trans_json_ex_s(jpg_upload_url, sizeof(jpg_upload_url), "\"url\"", jpg_part);
            trans_json_ex_s(jpg_obj_name, sizeof(jpg_obj_name), "\"obj_name\"", jpg_part);
            trans_json_ex_s(jpg_pwd, sizeof(jpg_pwd), "\"pwd\"", jpg_part);
        }

        if(trans_json_ex_s(mp4_part, sizeof(mp4_part), "\"mp4\"", recv_buf) == TRUE)
        {
            trans_json_ex_s(mp4_upload_url, sizeof(mp4_upload_url), "\"url\"", mp4_part);
            trans_json_ex_s(mp4_obj_name, sizeof(mp4_obj_name), "\"obj_name\"", mp4_part);
            trans_json_ex_s(mp4_pwd, sizeof(mp4_pwd), "\"pwd\"", mp4_part);
        }

        if(trans_json_ex_s(alert_max_retry_str, sizeof(alert_max_retry_str), "\"alertMaxRetry\"", recv_buf) == TRUE)
        {
            alert_max_retry = atoi(alert_max_retry_str);
        }
        else
        {
            alert_max_retry = -1;
        }

        if(trans_json_ex_s(alert_retry_interval_str, sizeof(alert_retry_interval_str), "\"alertRetryInterval\"", recv_buf) == TRUE)
        {
            alert_retry_interval = atoi(alert_retry_interval_str);
        }
        else
        {
            alert_retry_interval = -1;
        }

        if(trans_json_ex_s(alert_interval_str, sizeof(alert_interval_str), "\"alertInterval\"", recv_buf) == TRUE)
        {
            alert_interval = atoi(alert_interval_str);
            g_cloud_info.event_timeout = g_cloud_info.g_mmap_info_ptr->systick + alert_interval*10;
        }
        else
        {
            alert_interval = -1;
        }

        if(strlen(jpg_upload_url) != 0)
        {
            pic_ok = 0;
        }

        if(strlen(mp4_upload_url) != 0)
        {
            mp4_ok = 0;
        }

        event_motion(jpg_obj_name, mp4_obj_name, jpg_pwd, mp4_pwd);
        webapi_do_event_update(jpg_upload_url, mp4_upload_url, jpg_pwd, mp4_pwd, type, sub_type, g_cloud_info.g_mmap_info_ptr->motion_time, 0, 1, "");

        while(cnt_down)
        {
            if(g_cloud_info.event_timeout < g_cloud_info.g_mmap_info_ptr->systick + 1200)
            {
                break;
            }

            if(pic_ok == 0 && access(MOTION_PIC, F_OK) == 0)
            {
                if(strlen(jpg_pwd) > 0)
                {
                    cloud_crypt_file(MOTION_PIC, pic_crypt_name, jpg_pwd);
                    snprintf(pic_name, sizeof(pic_name), "%s", pic_crypt_name);
                }
                else
                {
                    snprintf(pic_name, sizeof(pic_name), "%s", MOTION_PIC);
                }

                ret = webapi_do_event_upload(jpg_upload_url, pic_name);
                if(ret < 0)
                {
                    pic_ok = -1;
                }
                else
                {
                    pic_ok = 1;
                    pic_upload_cnt++;
                }
            }

            if(mp4_ok == 0 && access(MOTION_VIDEO, F_OK) == 0)
            {
                if(strlen(mp4_pwd) > 0)
                {
                    cloud_crypt_file(MOTION_VIDEO, video_crypt_name, mp4_pwd);
                    snprintf(video_name, sizeof(video_name), "%s", video_crypt_name);
                }
                else
                {
                    snprintf(video_name, sizeof(video_name), "%s", MOTION_VIDEO);
                }

                ret = webapi_do_event_upload(mp4_upload_url, video_name);
                if(ret < 0)
                {
                    mp4_ok = -1;
                }
                else
                {
                    mp4_ok = 1;
                    video_upload_cnt++;
                }
            }
            else
            {
                //printf("[P>>>]mp4_ok = %d, is_vedio_exist = %d\n", mp4_ok, access(MOTION_VIDEO, F_OK));
            }

            if(pic_ok != 0 && mp4_ok != 0)
            {
                break;
            }

            usleep(100*1000);
            cnt_down--;
        }

        system(cmd);

        do_motion_push = 0;
    }

    sem_destroy(&motion_sem);
    pthread_exit(0);
}

void *snap_push_proc()
{
    char *recv_buf = snap_upload_url;
    char mp4_part[1024] = {0};
    char jpg_part[1024] = {0};
    char mp4_upload_url[512] = {0};
    char jpg_upload_url[512] = {0};
    char mp4_obj_name[256] = {0};
    char jpg_obj_name[256] = {0};
    char mp4_pwd[64] = {0};
    char jpg_pwd[64] = {0};
    char alert_max_retry_str[16] = {0};
    char alert_retry_interval_str[16] = {0};
    char pic_name[64] = {0};
    char video_name[64] = {0};
    char pic_crypt_name[64] = {0};
    char video_crypt_name[64] = {0};
    char cmd[256] = {0};
    int cnt_down = 100;
    int pic_ok = -1, mp4_ok = -1;
    int ret = -1;

    pthread_detach(pthread_self());
    dump_string(_F_, _FU_, _L_, "snap_push_proc ok");

    snprintf(pic_crypt_name, sizeof(pic_crypt_name), "%s.crypt", SNAPSHOT_PIC);
    snprintf(video_crypt_name, sizeof(video_crypt_name), "%s.crypt", SNAPSHOT_VIDEO);

    //snprintf(cmd, sizeof(cmd), "rm -f %s && rm -f %s && rm -f %s && rm -f %s", SNAPSHOT_PIC, SNAPSHOT_VIDEO, pic_crypt_name, video_crypt_name);
    snprintf(cmd, sizeof(cmd), "rm -f %s* %s*", MOTION_PIC, MOTION_VIDEO);

    while(1)
    {
        memset(mp4_part, 0, sizeof(mp4_part));
        memset(jpg_part, 0, sizeof(jpg_part));
        memset(mp4_upload_url, 0, sizeof(mp4_upload_url));
        memset(jpg_upload_url, 0, sizeof(jpg_upload_url));
        memset(mp4_obj_name, 0, sizeof(mp4_obj_name));
        memset(jpg_obj_name, 0, sizeof(jpg_obj_name));
        memset(alert_max_retry_str, 0, sizeof(alert_max_retry_str));
        memset(alert_retry_interval_str, 0, sizeof(alert_retry_interval_str));
        memset(pic_name, 0, sizeof(pic_name));
        memset(video_name, 0, sizeof(video_name));
        memset(mp4_pwd, 0, sizeof(mp4_pwd));
        memset(jpg_pwd, 0, sizeof(jpg_pwd));
        cnt_down = 100;
        pic_ok = -1;
        mp4_ok = -1;

        sem_wait(&snap_sem);

        do_snap_push = 1;

        if(trans_json_ex_s(jpg_part, sizeof(jpg_part), "\"jpg\"", recv_buf) == TRUE)
        {
            trans_json_ex_s(jpg_upload_url, sizeof(jpg_upload_url), "\"url\"", jpg_part);
            trans_json_ex_s(jpg_obj_name, sizeof(jpg_obj_name), "\"obj_name\"", jpg_part);
            trans_json_ex_s(jpg_pwd, sizeof(jpg_pwd), "\"pwd\"", jpg_part);
        }

        if(trans_json_ex_s(mp4_part, sizeof(mp4_part), "\"mp4\"", recv_buf) == TRUE)
        {
            trans_json_ex_s(mp4_upload_url, sizeof(mp4_upload_url), "\"url\"", mp4_part);
            trans_json_ex_s(mp4_obj_name, sizeof(mp4_obj_name), "\"obj_name\"", mp4_part);
            trans_json_ex_s(mp4_pwd, sizeof(mp4_pwd), "\"pwd\"", mp4_part);
        }

        if(trans_json_ex_s(alert_max_retry_str, sizeof(alert_max_retry_str), "\"alertMaxRetry\"", recv_buf) == TRUE)
        {
            alert_max_retry = atoi(alert_max_retry_str);
        }

        if(trans_json_ex_s(alert_retry_interval_str, sizeof(alert_retry_interval_str), "\"alertRetryInterval\"", recv_buf) == TRUE)
        {
            alert_retry_interval = atoi(alert_retry_interval_str);
        }

        if(strlen(jpg_upload_url) != 0)
        {
            pic_ok = 0;
        }

        if(strlen(mp4_upload_url) != 0)
        {
            mp4_ok = 0;
        }

        event_snapshot(jpg_obj_name, mp4_obj_name, jpg_pwd, mp4_pwd);

        while(cnt_down)
        {
            if(pic_ok == 0 && access(SNAPSHOT_PIC, F_OK) == 0)
            {
                if(strlen(jpg_pwd))
                {
                    cloud_crypt_file(SNAPSHOT_PIC, pic_crypt_name, jpg_pwd);
                    strcpy(pic_name, pic_crypt_name);
                }
                else
                    strcpy(pic_name, SNAPSHOT_PIC);

                ret = webapi_do_event_upload(jpg_upload_url, pic_name);
                if(ret < 0)
                {
                    pic_ok = -1;
                }
                else
                {
                    pic_ok = 1;
                    pic_upload_cnt++;
                }
            }

            if(mp4_ok == 0 && access(SNAPSHOT_VIDEO, F_OK) == 0)
            {
                if(strlen(mp4_pwd))
                {
                    cloud_crypt_file(SNAPSHOT_VIDEO, video_crypt_name, mp4_pwd);
                    strcpy(video_name, video_crypt_name);
                }
                else
                    strcpy(video_name, SNAPSHOT_VIDEO);

                ret = webapi_do_event_upload(mp4_upload_url, video_name);
                if(ret < 0)
                {
                    mp4_ok = -1;
                }
                else
                {
                    mp4_ok = 1;
                    video_upload_cnt++;
                }
            }

            if(pic_ok != 0 && mp4_ok != 0)
            {
                break;
            }

            usleep(100*1000);
            cnt_down--;
        }

        system(cmd);

        do_snap_push = 0;
    }

    sem_destroy(&snap_sem);
    pthread_exit(0);
}

void yi_push_rcd_file()
{
    char ret_string[2048] = {0};
    char cmd[512] = {0};
    char code[16] = {0};
    int trycnt = 3;
    int success = 0;
    char upload_url[512] = {0};
    int ret = 0;
    char *filename = strrchr(rcd_file_path, '/') + 1;

    if(!filename)
    {
        dump_string(_F_, _FU_, _L_, "rcd_file_path invalid = %s\n", rcd_file_path);
        return;
    }

    snprintf(cmd, sizeof(cmd), "%s -c 414 -url %s/v4/ipc/upload_data -uid %s -keySec %s -filename %s",
             CLOUDAPI_PATH,
             g_cloud_info.g_mmap_info_ptr->api_server,
             g_cloud_info.g_mmap_info_ptr->p2pid,
             g_cloud_info.g_mmap_info_ptr->key,
             filename);
    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

    while(trycnt > 0)
    {
        trycnt--;

        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        memset(upload_url, 0, sizeof(upload_url));

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            if(trans_json_ex_s(upload_url, sizeof(upload_url), "\"uploadUrl\"", ret_string) == TRUE)
            {
                success = 1;
                dump_string(_F_, _FU_, _L_, "gen_file_url success \n");
                break;
            }
        }

        sleep(5);
    }

    if(success)
    {
        int upload_ok = 0;

        trycnt = 3;
        while(trycnt > 0)
        {
            if(!upload_ok)
            {
                ret = webapi_do_event_upload(upload_url, rcd_file_path);
                logw("webapi_do_event_upload(%s) ret = %d", rcd_file_path, ret);
                if(ret >= 0)
                {
                    upload_ok = 1;
                    break;
                }
            }

            sleep(3);
            trycnt--;
        }
    }

    remove(rcd_file_path);
    memset(cmd, 0, sizeof(cmd));
    snprintf(cmd, sizeof(cmd), "rm -f /tmp/recorddata/rcd_*");
    dump_string(_F_, _FU_, _L_,  "cmd=%s\n", cmd);
    system(cmd);
}

void yi_motion_push(int alarm_type/*0:motion; 1:baby cry; 2:abnormal sound*/)
{
    char ret_string[2048] = {0};
    char clean_cmd[512] = {0};
    char cmd[512] = {0};
    char code[16] = {0};
    char data[2048] = {0};
    char ok_str[16] = {0};
    char jpg_part[1024] = {0};
    char mp4_part[1024] = {0};
    char jpg_upload_url[512] = {0};
    char mp4_upload_url[512] = {0};
    char jpg_pwd[64] = {0};
    char mp4_pwd[64] = {0};
    char pic_upload_name[64] = {0};
    char video_upload_name[64] = {0};
    char pic_original_name[64] = {0};
    char pic_crypt_name[64] = {0};
    char video_original_name[64] = {0};
    char video_crypt_name[64] = {0};
    char event_uuid[64] = {0};
    int trycnt = 3;
    int success = 0;
    int cnt_down = 100;
    int pic_ok = -1, mp4_ok = -1;
    int type = 0, sub_type = 0;
    int event_time = 0;
    int ret = 0;
    int gen_success = 0;
    int need_alert = 0;
    int need_jpg = 0;
    int need_mp4 = 0;
    char debug_log[256] = {};
    char suffix_mode[32] = {0};

#ifdef HAVE_FEATURE_FACE
    char tar_part[1024] = {0};
    char tar_upload_url[512] = {0};
    char tar_pwd[64] = {0};
    char tar_name[64] = {0};
    char tar_crypt_name[64] = {0};
    char tar_upload_name[64] = {0};

    int wait_face_count = 100;
    int tar_ok = -1;
    int change_human_alarm_num = 0;
    int face_count = 0;

    snprintf(tar_name, sizeof(tar_name), "%s", OSS_FACE_TAR_NAME);
    snprintf(tar_crypt_name, sizeof(tar_crypt_name), "%s.crypt", OSS_FACE_TAR_NAME);
#ifdef PRODUCT_H31BG
    //g_cloud_info.event_tm = g_cloud_info.g_mmap_info_ptr->motion_time;
    g_cloud_info.event_tm = g_cloud_info.g_mmap_info_ptr->event_tm;
    event_time = g_cloud_info.g_mmap_info_ptr->event_tm;
    g_cloud_info.event_timeout = event_time + IPC_SET_TIME;
    //send_set_event_time_msg();
    send_set_motion_stop_msg();
    dump_string(_F_, _FU_, _L_, " g_cloud_info.event_timeout is = %d, event_time:%d\n", g_cloud_info.event_timeout, event_time);
#endif
#endif

    if(1 == alarm_type)
    {
        sub_type = 3;				//baby cry
        event_time = time(NULL);
        snprintf(pic_original_name, sizeof(pic_original_name), "%s", BABYCRY_PIC);
        snprintf(video_original_name, sizeof(video_original_name), "%s", BABYCRY_VIDEO);
        snprintf(pic_crypt_name, sizeof(pic_crypt_name), "%s.crypt", BABYCRY_PIC);
        snprintf(video_crypt_name, sizeof(video_crypt_name), "%s.crypt", BABYCRY_VIDEO);
        snprintf(clean_cmd, sizeof(clean_cmd), "rm -f %s* %s*", BABYCRY_PIC, BABYCRY_VIDEO);
        system_cmd_withret_timeout(clean_cmd, ret_string, sizeof(ret_string), 10);
        cloud_cap_pic(pic_original_name);
    }
    else if(2 == alarm_type)
    {
        sub_type = 5;				//abnormal sound
        event_time = time(NULL);
        snprintf(pic_original_name, sizeof(pic_original_name), "%s", ABNORMAL_SOUND_PIC);
        snprintf(video_original_name, sizeof(video_original_name), "%s", ABNORMAL_SOUND_VIDEO);
        snprintf(pic_crypt_name, sizeof(pic_crypt_name), "%s.crypt", ABNORMAL_SOUND_PIC);
        snprintf(video_crypt_name, sizeof(video_crypt_name), "%s.crypt", ABNORMAL_SOUND_VIDEO);
        snprintf(clean_cmd, sizeof(clean_cmd), "rm -f %s* %s*", ABNORMAL_SOUND_PIC, ABNORMAL_SOUND_VIDEO);
        system_cmd_withret_timeout(clean_cmd, ret_string, sizeof(ret_string), 10);
        cloud_cap_pic(pic_original_name);
    }
    else
    {
        logw("motion_type(%d)", g_cloud_info.g_mmap_info_ptr->motion_type);

        snprintf(pic_original_name, sizeof(pic_original_name), "%s", MOTION_PIC);
        snprintf(video_original_name, sizeof(video_original_name), "%s", MOTION_VIDEO);
        snprintf(pic_crypt_name, sizeof(pic_crypt_name), "%s.crypt", MOTION_PIC);
        snprintf(video_crypt_name, sizeof(video_crypt_name), "%s.crypt", MOTION_VIDEO);

        #ifndef PRODUCT_H31BG
        snprintf(clean_cmd, sizeof(clean_cmd), "rm -f %s* %s*", MOTION_PIC, MOTION_VIDEO);
        system_cmd_withret_timeout(clean_cmd, ret_string, sizeof(ret_string), 10);
        #endif

        if(g_cloud_info.g_mmap_info_ptr->motion_type == 1)
        {
            #if defined(HUMAN_MOTION_TRACK)
            if(g_cloud_info.g_mmap_info_ptr->ptz_motion_track_switch == 1)
            {
                sub_type = 4;
            }
            else if(g_cloud_info.g_mmap_info_ptr->ptz_motion_track_switch == 2)//human motion track
            {
                sub_type = 12;
            }
            #else
            sub_type = 4;			//�ƶ�����
            #endif
            cloud_cap_pic(pic_original_name);
        }
        else if(g_cloud_info.g_mmap_info_ptr->motion_type == 7)
        {
            sub_type = 7;			//face
        }
        else if(g_cloud_info.g_mmap_info_ptr->motion_type == 2)
        {
#ifdef HAVE_FEATURE_FACE
            if(1 == g_cloud_info.g_mmap_info_ptr->human_face_enable)
            {
                while(1)
                {
                    if(change_human_alarm_num >= 60)
                    {
                        sub_type = 2;			//���μ��
                        cloud_cap_pic(pic_original_name);
                        break;
                    }

                    if(g_cloud_info.g_mmap_info_ptr->motion_type == 7)
                    {
                        sub_type = 7;			//face
                        break;
                    }

                    change_human_alarm_num++;
                    usleep(50 * 1000);
                }
            }
            else
#endif
            {
                sub_type = 2;			//���μ��
                #ifdef PRODUCT_H31BG
                if(access(pic_original_name, F_OK))
                #endif
                    cloud_cap_pic(pic_original_name);
            }
        }
        else
        {
            sub_type = 1;			//�ƶ�
            cloud_cap_pic(pic_original_name);
        }
#ifndef PRODUCT_H31BG
        event_time = g_cloud_info.g_mmap_info_ptr->motion_time;
#endif
        logw("sub_type(%d)", sub_type);
#ifdef HAVE_FEATURE_FACE
        if(sub_type == 7)
        {
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
            cloud_make_video(video_original_name, 6, E_NORMAL_TYPE, event_time);
#else
            cloud_make_video(video_original_name, 6, E_FACE_TYPE, event_time);
#endif

            int loopCnt = 0;
            while(access(tar_name, F_OK) != 0 || g_cloud_info.g_mmap_info_ptr->face_count == 0)
            {
                usleep(50*1000);
                loopCnt++;
                if(loopCnt > 200)
                {
                    dump_string(_F_, _FU_, _L_, " face detect failed! can not get face conut or tar file!! g_mmap_info_ptr->face_count = %d\n", g_cloud_info.g_mmap_info_ptr->face_count);
                    return;
                }
            }
        }
#endif
    }
    #if defined(HUMAN_MOTION_TRACK)
    if(sub_type == 12)				//human motion track
    {
        cloud_make_video(video_original_name, 10, E_NORMAL_TYPE, event_time);
    }
    else
    #endif
    if(sub_type == 4)				//�ƶ�����
    {
        cloud_make_video(video_original_name, 10, E_NORMAL_TYPE, event_time);
    }
    else if(sub_type == 3)			//baby cry
    {
        cloud_make_video(video_original_name, 6, E_NORMAL_TYPE, event_time);
    }
    else if(sub_type == 5)			//abnormal sound
    {
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
        cloud_make_video(video_original_name, 6, E_VOICE_TYPE, event_time);
#else
        cloud_make_video(video_original_name, 6, E_NORMAL_TYPE, event_time);
#endif
    }
    else if(sub_type == 2)			//���μ��
    {
#ifndef PRODUCT_H31BG   //H31BG push alert video to s3 , and is ts file
        if(1 != g_cloud_info.g_mmap_info_ptr->human_face_enable)
        {
            cloud_make_video(video_original_name, 6, E_NORMAL_TYPE, event_time);
        }
        else
        {
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
            cloud_make_video(video_original_name, 6, E_NORMAL_TYPE, event_time);
#else
            cloud_make_video(video_original_name, 6, E_HUMAN_TYPE, event_time);
#endif
        }
#endif
    }
    else if(sub_type == 7)			//face
    {
        //cloud_make_video(video_original_name, 6, E_FACE_TYPE, event_time);
    }
    else							//�ƶ�
    {
        cloud_make_video(video_original_name, 6, E_NORMAL_TYPE, event_time);
        sleep(3);
        if(g_cloud_info.g_mmap_info_ptr->motion_type == 1)
        {
            sub_type = 4;			//�ƶ�����
        }
#ifndef HAVE_FEATURE_FACE
        else if(g_cloud_info.g_mmap_info_ptr->motion_type == 2)
        {
            sub_type = 2;			//���μ��
        }
        else if(g_cloud_info.g_mmap_info_ptr->motion_type == 7)
        {
            sub_type = 7;			//face
        }
#endif
        else
        {
            sub_type = 1;			//�ƶ�
        }
    }

    if(7 != sub_type)
    {
        snprintf(suffix_mode, sizeof(suffix_mode), "%s", SUFFIX_MODE_NOT_FACE);
    }
    else
    {
        snprintf(suffix_mode, sizeof(suffix_mode), "%s", SUFFIX_MODE_FACE);
    }

    snprintf(cmd, sizeof(cmd), "%s -c 306 -url %s/v5/alert/gen_presigned_url "
             "-uid %s -keySec %s -type %d -sub_type %d -suffix %s -time %d",
             CLOUDAPI_PATH,
             g_cloud_info.g_mmap_info_ptr->api_server,
             g_cloud_info.g_mmap_info_ptr->p2pid,
             g_cloud_info.g_mmap_info_ptr->key,
             type, sub_type, suffix_mode,
             event_time);

    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

    while(trycnt > 0)
    {
        trycnt--;
        gen_url++;

        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        memset(data, 0, sizeof(data));
        memset(ok_str, 0, sizeof(ok_str));
        memset(jpg_part, 0, sizeof(jpg_part));
        memset(mp4_part, 0, sizeof(mp4_part));
        memset(jpg_upload_url, 0, sizeof(jpg_upload_url));
        memset(jpg_pwd, 0, sizeof(jpg_pwd));
        memset(mp4_upload_url, 0, sizeof(mp4_upload_url));
        memset(mp4_pwd, 0, sizeof(mp4_pwd));
        memset(event_uuid, 0,sizeof(event_uuid));
#ifdef HAVE_FEATURE_FACE
        memset(tar_part, 0, sizeof(tar_part));
        memset(tar_upload_url, 0, sizeof(tar_upload_url));
        memset(tar_pwd, 0, sizeof(tar_pwd));
#endif

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            if(trans_json_ex_s(data, sizeof(data), "\"data\"", ret_string) == TRUE)
            {
                dump_string(_F_, _FU_, _L_, "gen_presigned_url success \n");

                gen_success = 1;
                if(trans_json_ex_s(event_uuid, sizeof(event_uuid), "\"event_uuid\"", data) == TRUE)
                {
                    cloud_set_event_uuid(event_uuid);
                }
                if(trans_json_ex_s(ok_str, sizeof(ok_str), "\"ok\"", data) == TRUE && strcmp(ok_str, "true") == 0)
                {
#ifdef HAVE_FEATURE_FACE
                    if(7 == sub_type)
                    {
                        if(trans_json_ex_s(tar_part, sizeof(tar_part), "\"tar\"", data) == TRUE)
                        {
                            trans_json_ex_s(tar_upload_url, sizeof(tar_upload_url), "\"url\"", tar_part);
                            trans_json_ex_s(tar_pwd, sizeof(tar_pwd), "\"pwd\"", tar_part);
                        }
                        if(trans_json_ex_s(mp4_part, sizeof(mp4_part), "\"mp4\"", data) == TRUE)
                        {
                            need_mp4 = 1;
                            trans_json_ex_s(mp4_upload_url, sizeof(mp4_upload_url), "\"url\"", mp4_part);
                            trans_json_ex_s(mp4_pwd, sizeof(mp4_pwd), "\"pwd\"", mp4_part);
                        }

                    }
                    else
#endif
                    {
                        if(trans_json_ex_s(jpg_part, sizeof(jpg_part), "\"jpg\"", data) == TRUE)
                        {
                            need_jpg = 1;
                            trans_json_ex_s(jpg_upload_url, sizeof(jpg_upload_url), "\"url\"", jpg_part);
                            trans_json_ex_s(jpg_pwd, sizeof(jpg_pwd), "\"pwd\"", jpg_part);
                        }
                        if(trans_json_ex_s(mp4_part, sizeof(mp4_part), "\"mp4\"", data) == TRUE)
                        {
                            need_mp4 = 1;
                            trans_json_ex_s(mp4_upload_url, sizeof(mp4_upload_url), "\"url\"", mp4_part);
                            trans_json_ex_s(mp4_pwd, sizeof(mp4_pwd), "\"pwd\"", mp4_part);
                        }
                    }
                    need_alert = 1;
                }
                else
                {
                    int trycnt = 10;
                    while(trycnt > 0)
                    {
                        trycnt--;
                        if(access(video_original_name, F_OK) == 0)
                        {
                            break;
                        }
                        sleep(1);
                    }
                }
                break;
            }
        }

        gen_url_fail++;

        sleep(5);
    }
    
    if(1 == need_alert)
    {
#ifdef HAVE_FEATURE_FACE
        if(7 == sub_type)
        {
            if(strlen(tar_upload_url) != 0)
            {
                tar_ok = 0;
            }
            else
            {
                tar_ok = 2;
            }
            if(strlen(mp4_upload_url) != 0)
            {
                mp4_ok = 0;
            }
            else
            {
                mp4_ok = 2;
            }
            if((2 == tar_ok) && (2 == mp4_ok))			//not need push pic and video
            {
                cnt_down = 0;
                success = 1;
            }
        }
        else
#endif
        {
            if(strlen(jpg_upload_url) != 0)
            {
                pic_ok = 0;
            }
            else
            {
                pic_ok = 2;
            }

            if(strlen(mp4_upload_url) != 0)
            {
                mp4_ok = 0;
            }
            else
            {
                mp4_ok = 2;
            }
            if((2 == pic_ok) && (2 == mp4_ok))			//not need push pic and video
            {
                cnt_down = 0;
                success = 1;
            }
        }

        while(cnt_down)
        {
#if 0
            if((sub_type != 3) && (sub_type != 5))
            {
                if(g_cloud_info.event_timeout < g_cloud_info.g_mmap_info_ptr->systick + 1200)
                {
                    break;
                }
            }
#endif
#ifdef HAVE_FEATURE_FACE
            if(7 == sub_type)
            {
                if(tar_ok == 0 && access(tar_name, F_OK) == 0 && g_cloud_info.g_mmap_info_ptr->face_count != 0)
                {
                    face_count = g_cloud_info.g_mmap_info_ptr->face_count;
                    if(strlen(tar_pwd) != 0)
                    {
                        cloud_crypt_file(tar_name, tar_crypt_name, tar_pwd);
                        strcpy(tar_upload_name, tar_crypt_name);
                    }
                    else
                    {
                        strcpy(tar_upload_name, tar_name);
                    }

                    ret = webapi_do_event_upload(tar_upload_url, tar_upload_name);
                    if(ret < 0)
                    {
                        tar_ok = -1;
                    }
                    else
                    {
                        tar_ok = 1;
                    }
                }
                if(mp4_ok == 0 && access(video_original_name, F_OK) == 0)
                {
                    if(strlen(mp4_pwd) != 0)
                    {
                        cloud_crypt_file(video_original_name, video_crypt_name, mp4_pwd);
                        strcpy(video_upload_name, video_crypt_name);
                    }
                    else
                    {
                        strcpy(video_upload_name, video_original_name);
                    }

                    ret = webapi_do_event_upload(mp4_upload_url, video_upload_name);
                    if(ret < 0)
                    {
                        mp4_ok = -1;
                    }
                    else
                    {
                        mp4_ok = 1;
                        video_upload_cnt++;
                    }
                }
                if(1 ==  tar_ok && 1 == mp4_ok)
                {
                    success = 1;
                    break;
                }
            }
            else
#endif
            {
#ifndef PRODUCT_H31BG   //H31bg alert pic is uploaded to s3
                if(pic_ok == 0 && access(pic_original_name, F_OK) == 0)
                {
                    if(strlen(jpg_pwd) != 0)
                    {
                        cloud_crypt_file(pic_original_name, pic_crypt_name, jpg_pwd);
                        strcpy(pic_upload_name, pic_crypt_name);
                    }
                    else
                    {
                        strcpy(pic_upload_name, pic_original_name);
                    }

                    ret = webapi_do_event_upload(jpg_upload_url, pic_upload_name);
                    if(ret < 0)
                    {
                        pic_ok = -1;
                    }
                    else
                    {
                        pic_ok = 1;
                        pic_upload_cnt++;
                    }
                }

                if(mp4_ok == 0 && access(video_original_name, F_OK) == 0)
                {
                    if(strlen(mp4_pwd) != 0)
                    {
                        cloud_crypt_file(video_original_name, video_crypt_name, mp4_pwd);
                        strcpy(video_upload_name, video_crypt_name);
                    }
                    else
                    {
                        strcpy(video_upload_name, video_original_name);
                    }

                    ret = webapi_do_event_upload(mp4_upload_url, video_upload_name);
                    if(ret < 0)
                    {
                        mp4_ok = -1;
                    }
                    else
                    {
                        mp4_ok = 1;
                        video_upload_cnt++;
                    }
                }
#endif

#ifdef  PRODUCT_H31BG
                success = 1;
                break;
#else
                if(1 ==  pic_ok && 1 == mp4_ok)
                {
                    success = 1;
                    break;
                }
#endif
            }
            usleep(100*1000);
            cnt_down--;
        }
        if(success)
        {
#ifdef HAVE_FEATURE_FACE
            if(7 == sub_type)
            {
                if(2 == tar_ok)
                {
                    while(wait_face_count > 0)
                    {
                        if(access(tar_name, F_OK) == 0 && g_cloud_info.g_mmap_info_ptr->face_count != 0)
                        {
                            webapi_do_event_update(tar_upload_url, mp4_upload_url, tar_pwd, mp4_pwd, type, sub_type, event_time, g_cloud_info.g_mmap_info_ptr->face_count, 1, event_uuid);
                            break;
                        }
                        wait_face_count--;
                        usleep(100*1000);
                    }
                }
                else
                {
                    webapi_do_event_update(tar_upload_url, mp4_upload_url, tar_pwd, mp4_pwd, type, sub_type, event_time, face_count, 1, event_uuid);
                }
            }
            else
#endif
            {
                webapi_do_event_update(jpg_upload_url, mp4_upload_url, jpg_pwd, mp4_pwd, type, sub_type, event_time, 0, 1, event_uuid);
            }
        }

#if defined(PRODUCT_H30GA) || defined(PRODUCT_H31GA) || defined(PRODUCT_H32GA) || defined(PRODUCT_R40GA)
        #if defined(PRODUCT_H31GA)
        if(g_cloud_info.g_mmap_info_ptr->alarm && ((sub_type==1)||(sub_type==2)||(sub_type==7)))
        #elif defined(PRODUCT_R40GA)
        if(g_cloud_info.g_mmap_info_ptr->alarm && ((sub_type==1)||(sub_type==2)||(sub_type==12)))
        #else
        if(g_cloud_info.g_mmap_info_ptr->alarm && (sub_type ==1 || sub_type ==2))
        #endif
        {
            logw("alarm(%d), sub_type(%d)", g_cloud_info.g_mmap_info_ptr->alarm, sub_type);
            send_alarm_didi_msg(g_cloud_info.mqfd_dispatch);
        }
#endif

 #ifdef PRODUCT_H31BG
        int wait_count = 60;
        while(wait_count--)
        {
            if(0 == access(MOTION_PIC , F_OK))
            {
                cloud_pic_index();
                break;
            }
            usleep(50*1000);
        }
#endif
    }

    snprintf(debug_log, sizeof(debug_log), "event_time=%d,type=%d,sub_type=%d,gen_success=%d,need_alert=%d,alert_success=%d,need_jpg=%d,need_mp4=%d,pic_ok=%d,mp4_ok=%d",
             event_time, type, sub_type, gen_success, need_alert, success, need_jpg, need_mp4, pic_ok, mp4_ok);
    if((gen_success != 1) || ((need_alert == 1) && (success != 1)) || ((need_jpg == 1) && (pic_ok != 1)) || ((need_mp4 == 1) && (mp4_ok != 1)))
    {
        cloud_debug_log(1, debug_log);
    }
    else
    {
        cloud_debug_log(0, debug_log);
    }

    system_cmd_withret_timeout(clean_cmd, ret_string, sizeof(ret_string), 10);
}

#define VC_CAPTURE_FILE "/tmp/vc_capture.jpg"
#define VC_CAPTURE_CRYPT "/tmp/vc_capture.jpg.crypt"
#define VC_RECORD_FILE "/tmp/vc_record.mp4"
#define VC_RECORD_CRYPT "/tmp/vc_record.mp4.crypt"
void yi_push_voicecmd_file(int isVideo)
{
    char ret_string[2048] = {0};
    char cmd[512] = {0};
    char code[16] = {0};
    char data[2048] = {0};
    int trycnt = 3;
    int success = 0;
    int type = 2, sub_type = isVideo?4:3;
    char *orig_filename = isVideo?VC_RECORD_FILE:VC_CAPTURE_FILE;
    char *crypt_filename = isVideo?VC_RECORD_CRYPT:VC_CAPTURE_CRYPT;
    char jpg_upload_url[512] = {0},mp4_upload_url[512] = {0},*upload_url=isVideo?mp4_upload_url:jpg_upload_url;
    char jpg_pwd[64] = {0},mp4_pwd[64] = {0},*pwd=isVideo?mp4_pwd:jpg_pwd;

    int event_time = time(NULL);
    int ret = 0;

    logw("Voicecmd:%d",isVideo);
    remove(VC_CAPTURE_FILE);
    remove(VC_CAPTURE_CRYPT);
    remove(VC_RECORD_FILE);
    remove(VC_RECORD_CRYPT);

    cloud_cap_pic(VC_CAPTURE_FILE);
    if( isVideo )
    {
        cloud_make_video(orig_filename, VC_RECORD_DURATION, E_NORMAL_TYPE, time(NULL));
    }

    snprintf(cmd, sizeof(cmd), "%s -c 306 -url %s/v5/alert/gen_presigned_url "
             "-uid %s -keySec %s -type %d -sub_type %d -suffix %s -time %d",
             CLOUDAPI_PATH,
             g_cloud_info.g_mmap_info_ptr->api_server,
             g_cloud_info.g_mmap_info_ptr->p2pid,
             g_cloud_info.g_mmap_info_ptr->key,
             type, sub_type, isVideo?"jpg,mp4":"jpg",
             event_time);
    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

    while(trycnt > 0)
    {
        trycnt--;
        gen_url++;

        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        memset(data, 0, sizeof(data));
        memset(jpg_upload_url, 0, sizeof(jpg_upload_url));
        memset(mp4_upload_url, 0, sizeof(mp4_upload_url));
        memset(jpg_pwd, 0, sizeof(jpg_pwd));
        memset(mp4_pwd, 0, sizeof(mp4_pwd));

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            if(trans_json_ex_s(data, sizeof(data), "\"data\"", ret_string) == TRUE)
            {
                char ok_str[64]={0};
                dump_string(_F_, _FU_, _L_, "gen_presigned_url success \n");

                if(trans_json_ex_s(ok_str, sizeof(ok_str), "\"ok\"", data) == TRUE && strcmp(ok_str, "true") == 0)
                {
                    char part[512] = {0};
                    if(trans_json_ex_s(part, sizeof(part), "\"mp4\"", data) == TRUE)
                    {
                        trans_json_ex_s(mp4_upload_url, sizeof(mp4_upload_url), "\"url\"", part);
                        trans_json_ex_s(mp4_pwd, sizeof(mp4_pwd), "\"pwd\"", part);
                    }
                    bzero(part,sizeof(part));
                    if(trans_json_ex_s(part, sizeof(part), "\"jpg\"", data) == TRUE)
                    {
                        trans_json_ex_s(jpg_upload_url, sizeof(jpg_upload_url), "\"url\"", part);
                        trans_json_ex_s(jpg_pwd, sizeof(jpg_pwd), "\"pwd\"", part);
                    }
                    success = 1;
                    webapi_do_event_update(jpg_upload_url, mp4_upload_url, jpg_pwd, mp4_pwd, type, sub_type, event_time, 0, 1, "");
                }
                break;
            }
        }

        gen_url_fail++;

        sleep(5);
    }

    //wait record end
    trycnt = (isVideo?VC_RECORD_DURATION:0)+3;
    while(trycnt > 0)
    {
        if(access(orig_filename, F_OK) == 0)
        {
            break;
        }
        sleep(1);
        trycnt--;
    }
    if( trycnt <= 0 )
    {
        logw("Wait event end timeout!");
    }

    if(success)
    {
        int upload_ok = 0;
        int ext_ok=(isVideo?0:1);
        char *p_upd_file = orig_filename;
        char *p_upd_file_ext = VC_CAPTURE_FILE;

        logw("webapi_do_event_update() success");
        if( pwd[0] )
        {
            cloud_crypt_file(orig_filename, crypt_filename, pwd);
            p_upd_file = crypt_filename;
        }
        if( isVideo )
        {
            if( jpg_pwd[0] )
            {
                cloud_crypt_file(VC_CAPTURE_FILE, VC_CAPTURE_CRYPT, jpg_pwd);
                p_upd_file_ext = VC_CAPTURE_CRYPT;
            }
        }
        trycnt = 3;
        while(trycnt>0)
        {
            if(!upload_ok)
            {
                ret = webapi_do_event_upload(upload_url, p_upd_file);
                logw("webapi_do_event_upload(%s) ret= %d",p_upd_file,ret);
                if(ret >= 0)
                {
                    upload_ok = 1;
                }
            }
            if(isVideo && !ext_ok)
            {
                ret = webapi_do_event_upload(jpg_upload_url, p_upd_file_ext);
                logw("webapi_do_event_upload(%s) ret= %d",p_upd_file_ext,ret);
                if(ret >= 0)
                {
                    ext_ok = 1;
                }
            }

            if( upload_ok && ext_ok )
                break;
            sleep(3);
            trycnt--;
        }
    }
    remove(VC_CAPTURE_FILE);
    remove(VC_CAPTURE_CRYPT);
    remove(VC_RECORD_FILE);
    remove(VC_RECORD_CRYPT);
}

int send_vc_capture_end_msg()
{
    if( cloud_send_msg(g_cloud_info.mqfd_dispatch,DISPATCH_VC_STOP_CAPTURE,0,0)<0 )
    {
        dump_string(_F_, _FU_, _L_,  "send_vc_capture_end_msg send_msg fail!\n");
        return -1;
    }
    else
    {
        int count=50;
        while(g_cloud_info.g_mmap_info_ptr->vc_capure_stat != 0 && count-- >0 )
            ms_sleep(20);
        dump_string(_F_, _FU_, _L_,  "send_vc_capture_end_msg send_msg ok!\n");
        return 0;
    }
}

int send_vc_record_end_msg()
{
    if( cloud_send_msg(g_cloud_info.mqfd_dispatch,DISPATCH_VC_STOP_RECORD,0,0)<0 )
    {
        dump_string(_F_, _FU_, _L_,  "send_vc_record_end_msg send_msg fail!\n");
        return -1;
    }
    else
    {
        int count=50;
        while(g_cloud_info.g_mmap_info_ptr->vc_record_stat != 0 && count-- >0 )
            ms_sleep(20);
        dump_string(_F_, _FU_, _L_,  "send_vc_record_end_msg send_msg ok!\n");
        return 0;
    }
}
#ifdef PRODUCT_H31BG
int send_set_motion_stop_msg()
{
    if( cloud_send_msg(g_cloud_info.mqfd_dispatch,DISPATCH_SET_MOTION_STOP_STATE,0,0)<0 )
    {
        dump_string(_F_, _FU_, _L_,  "send_set_motion_stop_msg send_msg fail!\n");
        return -1;
    }
    else
    {
        int count=50;
        while(g_cloud_info.g_mmap_info_ptr->motion_stop != 0 && count-- >0 )
            ms_sleep(20);
        dump_string(_F_, _FU_, _L_,  "send_set_motion_stop_msg send_msg ok!\n");
        return 0;
    }
}

int send_set_event_time_msg()
{
    if( cloud_send_msg(g_cloud_info.mqfd_dispatch,DISPATCH_SET_EVENT_TIME,0,0)<0 )
    {
        dump_string(_F_, _FU_, _L_,  "send_set_event_time_msg send_msg fail!\n");
        return -1;
    }
    else
    {
        int count=50;
        while(g_cloud_info.g_mmap_info_ptr->event_tm != 0 && count-- >0 )
            ms_sleep(20);
        dump_string(_F_, _FU_, _L_,  "send_set_event_time_msg send_msg ok!\n");
        return 0;
    }
}
#endif
int send_re_online_success_msg()
{
    if( cloud_send_msg(g_cloud_info.mqfd_dispatch,DISPATCH_REONLINE_SUCCESS,0,0)<0 )
    {
        dump_string(_F_, _FU_, _L_,  "send_re_online_success_msg send_msg fail!\n");
        return -1;
    }
    else
    {
        int count=50;
        while(g_cloud_info.g_mmap_info_ptr->is_wifi_changed != 0 && count-- >0 )
            ms_sleep(20);
        dump_string(_F_, _FU_, _L_,  "send_re_online_success_msg send_msg ok!\n");
        return 0;
    }
}

void *yi_motion_push_proc(void *arg)
{
    int cur_motion_state = 0, prev_motion_state = 0;
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
    int cur_is_interval = 0;
    int cur_face_state = 0, prev_face_state = 0;
#endif
    char debug_log[256] = {};

    while(1)
    {
        if(g_cloud_info.g_mmap_info_ptr->power_mode == POWER_MODE_ON_E)
        {
            
            if(g_cloud_info.g_mmap_info_ptr->motion_time > 0)
            {
                cur_motion_state = cloud_get_motion_state();
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
                cur_is_interval = cloud_get_is_interval();

                if(cur_is_interval == 1)
                {
                    prev_motion_state = 0;
                }

                logw("cur_motion_state = %d, prev_motion_state = %d\n", cur_motion_state,prev_motion_state);
#endif
#ifdef PRODUCT_H31BG
                if((g_cloud_info.g_mmap_info_ptr->motion_stop == 1) && (g_cloud_info.g_mmap_info_ptr->motion_stat == 1))
#else
                if(cur_motion_state == 1 && prev_motion_state == 0)
#endif
                {
                    if(cur_motion_state == 1 && prev_motion_state == 0)
                    {
                        send_set_event_time_msg();
                    }

                    dump_string(_F_, _FU_, _L_, "\ngot a motion event_timeout(%d) systick(%d) p2p_viewing_cnt(%d)\n",
                                g_cloud_info.event_timeout, g_cloud_info.g_mmap_info_ptr->systick, g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt);
                    snprintf(debug_log, sizeof(debug_log), "motion_type=%d,motion_time=%d,event_timeout=%d,systick=%d,do_loging=%d",
                             g_cloud_info.g_mmap_info_ptr->motion_type, g_cloud_info.g_mmap_info_ptr->motion_time, g_cloud_info.event_timeout,
                             g_cloud_info.g_mmap_info_ptr->systick, g_cloud_info.do_loging);
                    cloud_debug_log(0, debug_log);

                    if(g_cloud_info.event_timeout == 0)
                    {
                        g_cloud_info.event_timeout = g_cloud_info.g_mmap_info_ptr->systick;
                    }
                    logw("motion_time = %d, event_timeout = %d\n", g_cloud_info.g_mmap_info_ptr->motion_time, g_cloud_info.event_timeout);
                    #ifdef PRODUCT_H31BG
                    if((g_cloud_info.g_mmap_info_ptr->motion_time >= g_cloud_info.event_timeout))
					#else
                    if(g_cloud_info.g_mmap_info_ptr->systick >= g_cloud_info.event_timeout)
					#endif
                    {
                       
                        #ifndef PRODUCT_H31BG
                        if(cloud_send_msg(g_cloud_info.mqfd_dispatch, RMM_SET_ALARM, NULL, 0) < 0) //上传报警视频前发送本地声光报警消息
                        {
                            dump_string(_F_, _FU_, _L_, "RMM_SET_ALARM send_msg to rmm fail!\n");
                        }
                        #endif
                       
                        //if(g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt < 2)
                        {
                            int trycnt = 200;
                            while(trycnt--)
                            {
                                if(0==g_cloud_info.do_loging)
                                {
                                    break;
                                }
                                ms_sleep(100);
                            }
                            yi_motion_push(0);
                        }
                    }
                }
                prev_motion_state = cur_motion_state;
            }
// 专用于人脸报警
#if defined(PRODUCT_H60GA)|| defined(PRODUCT_H31BG)
            if(g_cloud_info.g_mmap_info_ptr->motion_time > 0)
            {
                cur_face_state = cloud_get_face_state();

                logw("cur_face_state = %d, prev_face_state = %d\n", cur_face_state,prev_face_state);
                if(cur_face_state == 1 && prev_face_state == 0)
                {
                    dump_string(_F_, _FU_, _L_, "\ngot a motion event_timeout(%d) systick(%d) p2p_viewing_cnt(%d)\n",
                                g_cloud_info.event_timeout, g_cloud_info.g_mmap_info_ptr->systick, g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt);
                    snprintf(debug_log, sizeof(debug_log), "motion_type=%d,motion_time=%d,event_timeout=%d,systick=%d,do_loging=%d",
                             g_cloud_info.g_mmap_info_ptr->motion_type, g_cloud_info.g_mmap_info_ptr->motion_time, g_cloud_info.event_timeout,
                             g_cloud_info.g_mmap_info_ptr->systick, g_cloud_info.do_loging);
                    cloud_debug_log(0, debug_log);

                    if(g_cloud_info.event_timeout == 0)
                    {
                        g_cloud_info.event_timeout = g_cloud_info.g_mmap_info_ptr->systick;
                    }

                    if(g_cloud_info.g_mmap_info_ptr->motion_type == 7)
                    {
                        if(g_cloud_info.g_mmap_info_ptr->systick >= g_cloud_info.event_timeout)
                        {
                            //if(g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt < 2)
                            {
                                int trycnt = 200;
                                while(trycnt--)
                                {
                                    if(0==g_cloud_info.do_loging)
                                    {
                                        break;
                                    }
                                    ms_sleep(100);
                                }
                                yi_motion_push(0);
                            }
                        }
                    }
                }
                prev_face_state = cur_face_state;
            }
#endif


            if(report_baby_cry == 1)
            {
                cloud_debug_log(0, "babycry=1");
                report_baby_cry = 0;
                yi_motion_push(1);
            }

            if(report_abnormal_sound == 1)
            {
                cloud_debug_log(0, "sound=1");
                report_abnormal_sound = 0;
                yi_motion_push(2);
            }

            if(report_bigger_move == 1)
            {
                dump_string(_F_, _FU_, _L_, "cloud report bigger move\n");
                snprintf(debug_log, sizeof(debug_log), "bigger_move=1,motion_type=%d,motion_time=%d,event_timeout=%d,systick=%d",
                         g_cloud_info.g_mmap_info_ptr->motion_type, g_cloud_info.g_mmap_info_ptr->motion_time, g_cloud_info.event_timeout,
                         g_cloud_info.g_mmap_info_ptr->systick);
                cloud_debug_log(0, debug_log);
                report_bigger_move = 0;
                yi_motion_push(0);
            }

            if( g_cloud_info.g_mmap_info_ptr->vc_capure_stat == 1 )
            {
                yi_push_voicecmd_file(0);
                send_vc_capture_end_msg();
            }
            if( g_cloud_info.g_mmap_info_ptr->vc_record_stat == 1 )
            {
                yi_push_voicecmd_file(1);
                send_vc_record_end_msg();
            }

            if(report_rcd == 1)
            {
                if(0 == access(rcd_file_path, F_OK))
                {
                    dump_string(_F_, _FU_, _L_, "cloud report rcd, file path = %s\n", rcd_file_path);
                    yi_push_rcd_file();
                }
                else
                {
                    dump_string(_F_, _FU_, _L_, "rcd file not exist, path = %s\n", rcd_file_path);
                }
                report_rcd = 0;
            }
        }

        sleep(1);
    }
}

int yi_report_debug_info(debug_info_type_e info_type)
{
    int ret = -1;
    char url[3072] = {0};
    char cmd[4096] = {0};
    char ret_string[2048] = {0};
    char code[16] = {0};

    switch(info_type)
    {
    case DEBUG_INFO_TYPE_BIND_E:
        {
            snprintf(url, sizeof(url), "%s/info.gif?function=bindtrack&version=%s&did=%s&bind_key=%s&bind_errno=%d",
                     g_cloud_info.g_mmap_info_ptr->log_server,
                     g_cloud_info.g_mmap_info_ptr->version,
                     g_cloud_info.g_mmap_info_ptr->did,
                     g_cloud_info.g_mmap_info_ptr->bind_key,
                     g_bind_errno);

            snprintf(cmd, sizeof(cmd), "%s -c 412 -url \"%s\"", CLOUDAPI_PATH, url);
            break;
        }
    case DEBUG_INFO_TYPE_ALARM_E:
        {
            snprintf(url, sizeof(url), "%s/info.gif?function=alarmtrack&version=%s&did=%s",
                     g_cloud_info.g_mmap_info_ptr->log_server,
                     g_cloud_info.g_mmap_info_ptr->version,
                     g_cloud_info.g_mmap_info_ptr->did);

            snprintf(cmd, sizeof(cmd), "%s -c 415 -url \"%s\" -filename %s", CLOUDAPI_PATH, url, DEBUG_ALARM_PATH);
            break;
        }
    case DEBUG_INFO_TYPE_OSS_E:
        {
            snprintf(url, sizeof(url), "%s/info.gif?function=cloudservicetrack&version=%s&did=%s",
                     g_cloud_info.g_mmap_info_ptr->log_server,
                     g_cloud_info.g_mmap_info_ptr->version,
                     g_cloud_info.g_mmap_info_ptr->did);

            snprintf(cmd, sizeof(cmd), "%s -c 415 -url \"%s\" -filename %s", CLOUDAPI_PATH, url, DEBUG_OSS_PATH);
            break;
        }
    case DEBUG_INFO_TYPE_P2P_E:
        {
            snprintf(url, sizeof(url), "%s/info.gif?function=p2ptrack&version=%s&did=%s",
                     g_cloud_info.g_mmap_info_ptr->log_server,
                     g_cloud_info.g_mmap_info_ptr->version,
                     g_cloud_info.g_mmap_info_ptr->did);

            snprintf(cmd, sizeof(cmd), "%s -c 415 -url \"%s\" -filename %s", CLOUDAPI_PATH, url, DEBUG_P2P_PATH);
            break;
        }
    case DEBUG_INFO_TYPE_RMM_E:
        {
            snprintf(url, sizeof(url), "%s/info.gif?function=rmmtrack&version=%s&did=%s&venc=timeout",
                     g_cloud_info.g_mmap_info_ptr->log_server,
                     g_cloud_info.g_mmap_info_ptr->version,
                     g_cloud_info.g_mmap_info_ptr->did);

            snprintf(cmd, sizeof(cmd), "%s -c 412 -url \"%s\"", CLOUDAPI_PATH, url);
            break;
        }
    default:
        {
            return -1;;
        }
    }

    dump_string(_F_, _FU_, _L_, "yi_report_debug_info cmd: %s\n", cmd);

    system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);

    dump_string(_F_, _FU_, _L_, "yi_report_debug_info ret_string: %s\n", ret_string);

    if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE)
    {
        g_debug_enable[info_type] = 0;
        ret = 0;
        dump_string(_F_, _FU_, _L_, "yi_report_debug_info success\n");
    }

    return ret;
}

int is_valid_yi_bindkey()
{
    if(strstr(g_cloud_info.g_mmap_info_ptr->bind_key, "_yunyi") != NULL)
    {
        g_bind_errno = BIND_ERRNO_KEY_MISMATCH;
        dump_string(_F_, _FU_, _L_, "err, bind key has '_yunyi'\n");
        return 0;
    }

    if(g_cloud_info.g_mmap_info_ptr->did[2] == 'C' && g_cloud_info.g_mmap_info_ptr->did[3] == 'N')
    {
        if(g_cloud_info.g_mmap_info_ptr->bind_key[0] == 'C' && g_cloud_info.g_mmap_info_ptr->bind_key[1] == 'N')		//CN must qrcode CN
        {
            return 1;
        }
        else
        {
            g_bind_errno = BIND_ERRNO_KEY_MISMATCH;
            dump_string(_F_, _FU_, _L_, "err, bind key mismatch 1\n");
            return 0;
        }
    }
    else
    {
        if(g_cloud_info.g_mmap_info_ptr->bind_key[0] == 'C' && g_cloud_info.g_mmap_info_ptr->bind_key[1] == 'N')		//not CN must not qrcode CN
        {
            g_bind_errno = BIND_ERRNO_KEY_MISMATCH;
            dump_string(_F_, _FU_, _L_, "err, bind key mismatch 2\n");
            return 0;
        }
        else
        {
            return 1;
        }
    }
}

void yi_proc()
{
    int tnp_online_trigger = 0;
    int login_trigger = 3600*2;
    int ret = 0;
    int trycnt = 10;
    int back_key = 5;
#if defined(SMART_LINK)||defined(ENABLE_4G)
	int len;
#endif
    int cur_pwd_change_cnt = g_cloud_info.g_mmap_info_ptr->pwd_change_cnt;
    int set_pwd_sync_server_fail = 0;

    dump_string(_F_, _FU_, _L_, "yi_proc ok\n");

    g_cloud_info.do_loging = 1;

/* smartlink mode need add*/
#ifdef SMART_LINK
	do
	{
		len = strlen(g_cloud_info.g_mmap_info_ptr->bind_key);
		printf("----g_cloud_info.g_mmap_info_ptr->bind_key=%s-----len=%d-------\n",g_cloud_info.g_mmap_info_ptr->bind_key,len);
		sleep(1);
	}while((len <=6) &&(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1));
#endif
	dump_string(_F_, _FU_, _L_,  "len_bindkey = %d, bind_key = %s\n", \
		strlen(g_cloud_info.g_mmap_info_ptr->bind_key), g_cloud_info.g_mmap_info_ptr->bind_key);
	dump_string(_F_, _FU_, _L_,  "did = %s, p2pid = %s, \n", \
		g_cloud_info.g_mmap_info_ptr->did, g_cloud_info.g_mmap_info_ptr->p2pid);

	/* #if defined(PRODUCT_R30GB) || defined(PRODUCT_R35GB) ||defined(PRODUCT_H50GA) || defined(PRODUCT_Y29GA)\ */
    /* || defined(PRODUCT_R31GB) || defined(PRODUCT_Y21GA)|| defined(PRODUCT_Y28GA)\ */
    /* || defined(PRODUCT_H51GA)|| defined(PRODUCT_H52GA) || defined(PRODUCT_H53GA) || defined(PRODUCT_LYR30) || defined(PRODUCT_R33GB)|| defined(PRODUCT_Y19GA) */

#ifdef NOT_PLT_API
    if(strlen(g_cloud_info.g_mmap_info_ptr->bind_key) != 0 && !is_valid_yi_bindkey())
    {
        yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
        cloud_set_bind_result(DISPATCH_SPEAK_WRONG_DEVICE);
        while(1)
        {
            sleep(60);
        }
    }
#endif

    if((REGION_CHINA==g_cloud_info.g_mmap_info_ptr->region_id)&&(webapi_do_check_did() == -1))
    {
        yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
        while(1)
        {
            if(trycnt>0)
            {
                if(0==g_cloud_info.g_mmap_info_ptr->ban_device)
                {
                    if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_BAN_DEVICE, NULL, 0) < 0)
                    {
                        dump_string(_F_, _FU_, _L_,  "cloud_set_ban_device send_msg DISPATCH_SET_BAN_DEVICE fail!\n");
                    }

                    dump_string(_F_, _FU_, _L_, "ban region!\n");
                }

                if(cloud_send_msg(g_cloud_info.mqfd_dispatch, RMM_SPEAK_BAN_DEVICE, NULL, 0) < 0)
                {
                    dump_string(_F_, _FU_, _L_,  "cloud_set_ban_device send_msg RMM_SPEAK_BAN_DEVICE fail!\n");
                }

                trycnt--;
            }

            sleep(20);
        }
    }

    if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
    {
        if(strlen(g_cloud_info.g_mmap_info_ptr->last_api_server) != 0)
        {
            ret = webapi_do_reset_by_specified_server(g_cloud_info.g_mmap_info_ptr->last_api_server, 1, 0);
            if(ret == 0)
            {
                dump_string(_F_, _FU_, _L_, "webapi_do_reset last_api_server fail");
            }
            else
            {
                cloud_clear_api_server();
            }
        }

        ret = webapi_do_reset();
        if(ret == 0)
        {
            dump_string(_F_, _FU_, _L_, "webapi_do_reset fail");
            yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
            cloud_set_bind_result(DISPATCH_SET_BIND_TIMEOUT);
            while(1)
            {
                sleep(10);
            }
            //exit(0);
        }
        dump_string(_F_, _FU_, _L_, "webapi_do_login");
        ret = webapi_do_login();
        if(ret == 0)
        {
            dump_string(_F_, _FU_, _L_, "webapi_do_login fail");
            yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
            cloud_set_bind_result(DISPATCH_SET_BIND_TIMEOUT);
            while(1)
            {
                sleep(10);
            }
            //exit(0);
        }
        #if defined(ENABLE_4G)&&defined(ENABLE_SCANE_4G_DEVICE)
            #if defined(PRODUCT_B091QP)
            if(g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0')
            {
            #endif
                g_cloud_info.do_loging = 1;
                g_cloud_info.do_4g_binding = 1;
                if(webapi_do_tnp_on_line() == 1)
                {
                    dump_string(_F_, _FU_, _L_, "tnp online ok!");
                }
                g_cloud_info.do_4g_binding = 0;
                check_bindkey:
                while( (!bind_4g_successed) && g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
                {
                    if(strlen(g_cloud_info.g_mmap_info_ptr->bind_key)!=0)
                    {
                        ret = webapi_do_bindkey();
                        if(ret == 0)
                        {
                            dump_string(_F_, _FU_, _L_, "webapi_do_bindkey fail");
                            yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
                            cloud_set_bind_result(DISPATCH_SET_BIND_TIMEOUT);
                            while(1)
                            {
                                sleep(10);
                            }
                            //exit(0);
                        }
                        else
                        {
                            bind_4g_successed = 1;
                            #ifdef YI_RTMP_CLIENT
                            cloud_send_msg(g_cloud_info.mqfd_dispatch, CLOUD_SET_BIND_ONLINE, NULL, 0);
                            #endif
                            cloud_save_api_server();
                        }
                    }
                    sleep(1);
                }  
                dump_string(_F_, _FU_, _L_, "webapi_do_login again");
                ret = webapi_do_login();
                if(ret == 0)
                {
                    dump_string(_F_, _FU_, _L_, "webapi_do_login again failed");
                    yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
                    cloud_set_bind_result(DISPATCH_SET_BIND_TIMEOUT);
                    while(1)
                    {
                        sleep(10);
                    }
                    //exit(0);
                }
            #if defined(PRODUCT_B091QP)
            }
            else
            {
                goto check_bindkey;
            }
            #endif
        #else
            ret = webapi_do_bindkey();
            if(ret == 0)
            {
                dump_string(_F_, _FU_, _L_, "webapi_do_bindkey fail");
                yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
                cloud_set_bind_result(DISPATCH_SET_BIND_TIMEOUT);
                while(1)
                {
                    sleep(10);
                }
                //exit(0);
            }
            else
            {
                #ifdef YI_RTMP_CLIENT
                cloud_send_msg(g_cloud_info.mqfd_dispatch, CLOUD_SET_BIND_ONLINE, NULL, 0);
                #endif
                cloud_save_api_server();
            }
        #endif
    }
    else
    {
        ret = webapi_do_login();
        if(ret == 0)
        {
            dump_string(_F_, _FU_, _L_, "webapi_do_login fail");
            sleep(10);
            exit(0);
        }
        if(1 == g_cloud_info.online_use_tmppwd)
        {
            if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_PWD_SYNC_SERVER, NULL, 0) < 0)
            {
                set_pwd_sync_server_fail = 1;
                dump_string(_F_, _FU_, _L_,  "DISPATCH_SET_PWD_SYNC_SERVER send_msg fail!\n");
            }
            else
            {
                set_pwd_sync_server_fail = 0;
                dump_string(_F_, _FU_, _L_,  "DISPATCH_SET_PWD_SYNC_SERVER send_msg ok!\n");
            }
        }
    }
#if defined(ENABLE_4G)
    #if defined(PRODUCT_B091QP)
    if(g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0')
    {
    #endif
        ret = webapi_do_report_iccid();
        if(ret == 0){
            dump_string(_F_, _FU_, _L_, "webapi_do_report_iccid fail");
        } 
    #if defined(PRODUCT_B091QP)
    }
    #endif
#endif

#ifdef YI_RTMP_CLIENT
    cloud_send_msg(g_cloud_info.mqfd_dispatch, CLOUD_SET_BIND_ONLINE, NULL, 0);
#endif

    webapi_get_dev_info();

    if(1==debug_mod)
    {
        login_trigger = 5*60;
    }

    write_log(g_cloud_info.g_mmap_info_ptr->is_sd_exist, "first_login");

    while(1)
    {
        #if !defined(NOT_PLT_API)
        if(g_cloud_info.g_mmap_info_ptr->is_wifi_changed == 1 && g_cloud_info.g_mmap_info_ptr->wifi_connected == 1)
        {
            tnp_online_trigger=0;
            send_re_online_success_msg();
        }
        #endif
        //if(g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt < 2)
        {
        #if defined(PRODUCT_B091QP)&&defined(ENABLE_4G)
        do{
            if((tnp_online_trigger > 0)&&
                !(g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0' && g_cloud_info.need_recon))
            {
                break;
            }
            g_cloud_info.do_loging = 1;
            if(webapi_do_tnp_on_line() == 1)
            {
                dump_string(_F_, _FU_, _L_, "tnp online ok!");
            }
            tnp_online_trigger = 3600*2;
        }while(0);
        #else
            if(tnp_online_trigger <= 0
#ifdef ENABLE_4G
               ||g_cloud_info.need_recon
#endif
              )
            {
                g_cloud_info.do_loging = 1;
                if(webapi_do_tnp_on_line() == 1)
                {
                    dump_string(_F_, _FU_, _L_, "tnp online ok!");
                }

                tnp_online_trigger = 3600*2;
            }
        #endif
            if(set_pwd_sync_server_fail == 1)//try twice
            {
                if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_PWD_SYNC_SERVER, NULL, 0) < 0)
                {
                    dump_string(_F_, _FU_, _L_,  "DISPATCH_SET_PWD_SYNC_SERVER send_msg fail!\n");
                }
                else
                {
                    dump_string(_F_, _FU_, _L_,  "DISPATCH_SET_PWD_SYNC_SERVER send_msg ok!\n");
                }
                set_pwd_sync_server_fail = 0;
            }

            if((login_trigger <= 0) || (g_power_mode != g_cloud_info.g_mmap_info_ptr->power_mode))
            {
                g_cloud_info.do_loging = 1;
                if(webapi_do_login() == 1)
                {
                    dump_string(_F_, _FU_, _L_, "login ok!");
                    login_trigger = 3600*2;
                    back_key = 5;
                    if(1 == g_cloud_info.online_use_tmppwd)
                    {
                        if(cloud_send_msg(g_cloud_info.mqfd_dispatch, DISPATCH_SET_PWD_SYNC_SERVER, NULL, 0) < 0)
                        {
                            set_pwd_sync_server_fail = 1;
                            dump_string(_F_, _FU_, _L_,  "DISPATCH_SET_PWD_SYNC_SERVER send_msg fail!\n");
                        }
                        else
                        {
                            set_pwd_sync_server_fail = 0;
                            dump_string(_F_, _FU_, _L_,  "DISPATCH_SET_PWD_SYNC_SERVER send_msg ok!\n");
                        }
                    }

                    write_log(g_cloud_info.g_mmap_info_ptr->is_sd_exist, "login");
                    if(1==debug_mod)
                    {
                        login_trigger = 5*60;
                    }
                }
                else
                {
                    login_trigger = back_key;
                    back_key = back_key*4+rand()%back_key;
                    if(back_key > 3600*2)
                    {
                        back_key = 3600*2;
                    }
                }
            }
        }

        if(g_cloud_info.g_mmap_info_ptr->sync_info_from_server == 1)
        {
            #if !defined(NOT_PLT_API)//触发online，同时获取重启参数
            login_trigger = 1;
            #endif
            g_cloud_info.do_loging = 1;
            if(webapi_get_dev_info() == 1)
            {
                dump_string(_F_, _FU_, _L_, "sync info from server ok!");

                cloud_finish_sync_info_from_server();
            }
        }

        tnp_online_trigger--;
        login_trigger--;

        if(cur_pwd_change_cnt!=g_cloud_info.g_mmap_info_ptr->pwd_change_cnt)
        {
            login_trigger = 0;
            cur_pwd_change_cnt = g_cloud_info.g_mmap_info_ptr->pwd_change_cnt;
        }

        g_cloud_info.do_loging = 0;
        #if defined(ENABLE_4G)
            #if defined(PRODUCT_B091QP)
            if(g_cloud_info.g_mmap_info_ptr->hw_ver.is_4g_device != '0')
            {
            #endif
                if(g_cloud_info.need_recon)g_cloud_info.need_recon = 0;
            #if defined(PRODUCT_B091QP)
            }
            #endif
        #endif
        if(access("/tmp/cloud_init_finish", F_OK) != 0)
        {
            char buf[256] = {0};
            system_cmd_withret_timeout("echo 1 > /tmp/cloud_init_finish", buf, sizeof(buf), 10);
        }
        #ifndef PRODUCT_H31BG
        cloud_check_ethernet();
        #endif
        sleep(1);
    }

    return;
}

void mi_proc()
{
    int prev_viewer = g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt;
    int cur_motion_state = 0, prev_motion_state = 0;
    int need_jpg = 1, need_mp4 = 1;
    int no_alert_check = 0;

    int cur_pwd_change_cnt = g_cloud_info.g_mmap_info_ptr->pwd_change_cnt;
    unsigned int otc_info_trycnt = 0;
    unsigned int next_otc_info_tick = 0;
    unsigned int next_keepalive_tick = 0;
    unsigned int next_sdinfo_tick = 0;
    int sub_type = 0;

    char cmd[1024] = {0};
    char cmdbuf[1024] = {0};

    if(0==first_otc_info())
    {
        cloud_set_server_connect_ok();
    }

    next_otc_info_tick = g_cloud_info.g_mmap_info_ptr->systick + OTC_INFO_INTERVAL;
    next_keepalive_tick = g_cloud_info.g_mmap_info_ptr->systick + KEEPALIVE_INTERVAL;
    next_sdinfo_tick = g_cloud_info.g_mmap_info_ptr->systick + SD_REPORT_INTERVAL;

    ms_sleep(1000*2);

    if(strlen(g_cloud_info.g_mmap_info_ptr->bind_key) > 0 && 1==g_cloud_info.g_mmap_info_ptr->start_with_reset)
    {
        (void)sync_props();
    }

    sync_alert_setting();

    cur_motion_state = prev_motion_state = cloud_get_motion_state();

    prop_sd_size();
    prop_sd_avail();

    while(1)
    {

        if(cur_pwd_change_cnt != g_cloud_info.g_mmap_info_ptr->pwd_change_cnt)
        {
            if(0==(otc_info_trycnt%6000))
            {
                otc_info(-1);
            }

            if(1==first_otc_info_over)
            {
                cur_pwd_change_cnt = g_cloud_info.g_mmap_info_ptr->pwd_change_cnt;
            }

            otc_info_trycnt ++;
        }
        else
        {
            first_otc_info_over = 0;
            otc_info_trycnt = 0;
            if(next_otc_info_tick < g_cloud_info.g_mmap_info_ptr->systick)
            {
                otc_info(-1);
                next_otc_info_tick = g_cloud_info.g_mmap_info_ptr->systick + OTC_INFO_INTERVAL;
            }
        }

        if(g_cloud_info.g_mmap_info_ptr->power_mode == POWER_MODE_ON_E)
        {
            cur_motion_state = cloud_get_motion_state();
            if(cur_motion_state == 1 && prev_motion_state == 0)
            {
                dump_string(_F_, _FU_, _L_, "\ngot a motion\n");
                if(g_cloud_info.event_timeout == 0)
                {
                    g_cloud_info.event_timeout = g_cloud_info.g_mmap_info_ptr->systick;
                }

                if(g_cloud_info.alarm_enable == 1 && g_cloud_info.g_mmap_info_ptr->systick >= g_cloud_info.event_timeout)
                {
                    gen_url++;
                    motion_apply_url = 1;
                    snprintf(cmd, sizeof(cmd), "rm -f %s* %s*", MOTION_PIC, MOTION_VIDEO);
                    system_cmd_withret_timeout(cmd, cmdbuf, sizeof(cmdbuf), 10);
                    cloud_cap_pic(MOTION_PIC);

                    if(g_cloud_info.g_mmap_info_ptr->motion_type == 0)
                    {
                        sub_type = 1;			//�ƶ�
                    }
                    else
                    {
                        sub_type = 4;			//�ƶ�����
                    }

                    if(sub_type == 4)
                    {
                        cloud_make_video(MOTION_VIDEO, 10, E_NORMAL_TYPE, time(NULL));
                    }
                    else
                    {
                        cloud_make_video(MOTION_VIDEO, 6, E_NORMAL_TYPE, time(NULL));
                    }
                    no_alert_check = 0;
                    get_upload_url(need_jpg, need_mp4, no_alert_check);
                }
            }
            prev_motion_state = cur_motion_state;
        }


        if(g_cloud_info.g_mmap_info_ptr->systick >= next_keepalive_tick)
        {
            next_keepalive_tick = g_cloud_info.g_mmap_info_ptr->systick	+ KEEPALIVE_INTERVAL;
            keep_alive();
        }

        if(prev_viewer != g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt)
        {
            prop_visitors();
            prev_viewer = g_cloud_info.g_mmap_info_ptr->p2p_viewing_cnt;
        }

        if(g_cloud_info.g_mmap_info_ptr->systick > next_sdinfo_tick)
        {
            next_sdinfo_tick = g_cloud_info.g_mmap_info_ptr->systick + SD_REPORT_INTERVAL;
            prop_sd_size();
            prop_sd_avail();
        }

        service_handle();
    }
}

void panorama_capture_push()
{
    char ret_string[2048] = {0};
    char clean_cmd[512] = {0};
    char write_buf[256] = {0};
    char write_file[128] = {0};
    char cmd[512] = {0};
    char code[16] = {0};
    char data[2048] = {0};
    char ok_str[16] = {0};
    char tar_part[1024] = {0};
    char tar_upload_url[512] = {0};
    char tar_pwd[64] = {0};
    char tar_name[64] = {0};
    char tar_crypt_name[64] = {0};
    char tar_upload_name[64] = {0};
    int trycnt = 3;
    int success = 0;
    int cnt_down = 0;
    int tar_ok = -1;
    int type = 3, sub_type = 1;
    int panorama_capture_time = time(NULL);
    int ret = 0;

    snprintf(tar_name, sizeof(tar_name), "%s.tar", PANORAMA_CAPTURE);
    snprintf(tar_crypt_name, sizeof(tar_crypt_name), "%s.tar.crypt", PANORAMA_CAPTURE);
    snprintf(clean_cmd, sizeof(clean_cmd), "rm -rf %s*", PANORAMA_CAPTURE);

    system_cmd_withret_timeout(clean_cmd, ret_string, sizeof(ret_string), 10);

    cloud_start_panorama_capture();

    //waiting for dispatch set capture begin
    cnt_down = 100;
    while(cnt_down > 0)
    {
        cnt_down--;

        if(g_cloud_info.g_mmap_info_ptr->panorama_capture_state != PANORAMA_CAPTURE_STATE_IDLE)
        {
            break;
        }

        usleep(100*1000);
    }

    snprintf(cmd, sizeof(cmd), "%s -c 306 -url %s/v5/alert/gen_presigned_url "
             "-uid %s -keySec %s -type %d -sub_type %d -suffix %s -time %d",
             CLOUDAPI_PATH,
             g_cloud_info.g_mmap_info_ptr->api_server,
             g_cloud_info.g_mmap_info_ptr->p2pid,
             g_cloud_info.g_mmap_info_ptr->key,
             type, sub_type, "tar",
             panorama_capture_time);

    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);

    while(trycnt > 0)
    {
        trycnt--;
        gen_url++;

        memset(ret_string, 0, sizeof(ret_string));
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        memset(data, 0, sizeof(data));
        memset(ok_str, 0, sizeof(ok_str));
        memset(tar_part, 0, sizeof(tar_part));
        memset(tar_upload_url, 0, sizeof(tar_upload_url));
        memset(tar_pwd, 0, sizeof(tar_pwd));

        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            if(trans_json_ex_s(data, sizeof(data), "\"data\"", ret_string) == TRUE)
            {
                dump_string(_F_, _FU_, _L_, "gen_presigned_url success \n");

                if(trans_json_ex_s(ok_str, sizeof(ok_str), "\"ok\"", data) == TRUE && strcmp(ok_str, "true") == 0)
                {
                    if(trans_json_ex_s(tar_part, sizeof(tar_part), "\"tar\"", data) == TRUE)
                    {
                        trans_json_ex_s(tar_upload_url, sizeof(tar_upload_url), "\"url\"", tar_part);
                        trans_json_ex_s(tar_pwd, sizeof(tar_pwd), "\"pwd\"", tar_part);
                    }
                    success = 1;
                }

                break;
            }
        }

        gen_url_fail++;

        sleep(5);
    }

    if(success)
    {
        if(strlen(tar_upload_url) != 0)
        {
            tar_ok = 0;
        }

        cnt_down = 300;
        while(cnt_down > 0 && g_cloud_info.g_mmap_info_ptr->panorama_capture_state != PANORAMA_CAPTURE_STATE_IDLE &&
              g_cloud_info.g_mmap_info_ptr->panorama_capture_state != PANORAMA_CAPTURE_STATE_FAIL && panorama_capture_push_abort != 1)
        {
            if(tar_ok == 0 && g_cloud_info.g_mmap_info_ptr->panorama_capture_state == 2)
            {
                snprintf(write_buf, sizeof(write_buf), "{\"angle\":%d, \"mirror_flip\":%d}", g_cloud_info.g_mmap_info_ptr->ptz_y_angle, g_cloud_info.g_mmap_info_ptr->mirror);
                snprintf(write_file, sizeof(write_file), "%s/param.txt", PANORAMA_CAPTURE);

                write_to_file(write_file, "w+", write_buf, strlen(write_buf));

                snprintf(cmd, sizeof(cmd), "cd /tmp && tar -cf %s %s", basename(tar_name), "panorama_capture");
                system(cmd);

                if(strlen(tar_pwd) != 0)
                {
                    cloud_crypt_file(tar_name, tar_crypt_name, tar_pwd);
                    strcpy(tar_upload_name, tar_crypt_name);
                }
                else
                {
                    strcpy(tar_upload_name, tar_name);
                }

                ret = webapi_do_event_upload(tar_upload_url, tar_upload_name);
                if(ret < 0)
                {
                    tar_ok = -1;
                }
                else
                {
                    webapi_do_event_update(tar_upload_url, "", tar_pwd, "", type, sub_type, panorama_capture_time, 0, 1, "");
                    tar_ok = 1;
                    video_upload_cnt++;
                }
            }

            if(tar_ok != 0)
            {
                break;
            }

            usleep(100*1000);
            cnt_down--;
        }

        if(panorama_capture_push_abort == 1)
        {
            cloud_abort_panorama_capture();
        }
    }
    else
    {
        int trycnt_l = 300;
        while(trycnt_l>=0)
        {
            usleep(100*1000);
            trycnt_l--;
            if(g_cloud_info.g_mmap_info_ptr->panorama_capture_state == PANORAMA_CAPTURE_STATE_FINISH)
            {
                break;
            }
        }
    }

    if(tar_ok != 1)
    {
        cloud_set_panorama_capture_state(PANORAMA_CAPTURE_STATE_FAIL);
    }
    else
    {
        cloud_set_panorama_capture_state(PANORAMA_CAPTURE_STATE_IDLE);
    }

    system_cmd_withret_timeout(clean_cmd, ret_string, sizeof(ret_string), 10);
}

void *panorama_capture_push_proc(void *arg)
{
    cloud_set_panorama_capture_state(PANORAMA_CAPTURE_STATE_IDLE);

    while(1)
    {
        sem_wait(&panorama_capture_sem);

        if(g_cloud_info.g_mmap_info_ptr->power_mode == POWER_MODE_ON_E)
        {
            panorama_capture_push();
        }
    }

    sem_destroy(&panorama_capture_sem);
    pthread_exit(0);
}

void *debug_log_report_proc(void *arg)
{
    pthread_detach(pthread_self());

    int ret = -1;
    int i = 0;
    time_t cur_time = 0;
    time_t pre_time[DEBUG_INFO_TYPE_MAX_E] = { 0 };
    int back_key[DEBUG_INFO_TYPE_MAX_E] = { 1 };

    for(i=0; i<DEBUG_INFO_TYPE_MAX_E; i++)
    {
        g_debug_enable[i] = 0;
        pre_time[i] = 0;
        back_key[i] = 1;
    }

    while(1)
    {
        if((g_cloud_info.g_mmap_info_ptr->power_mode != POWER_MODE_ON_E) || (0 == g_cloud_info.g_mmap_info_ptr->wifi_connected))
        {
            sleep(1);
            continue;
        }

        cur_time = time(NULL);

        for(i=0; i<DEBUG_INFO_TYPE_MAX_E; i++)
        {
            if(g_debug_enable[i])
            {
                if(cur_time > (pre_time[i] + 60 * back_key[i]))
                {
                    ret = yi_report_debug_info(i);
                    pre_time[i] = cur_time;
                    if(ret)
                    {
                        if(back_key[i] < 64)
                        {
                            back_key[i] *= 2;
                        }
                    }
                    else
                    {
                        back_key[i] = 1;
                    }
                }
            }
        }

        sleep(1);
    }

    pthread_exit(0);
}


int webapi_do_check_net()
{
#if 0
    char cmd[1024] = {0};
    char ret_string[2048] = {0};
    char allow[32] = {0};
    char code[16] = {0};
    int ret = 0;
    snprintf(cmd, sizeof(cmd), "%s -c 311 -url %s/v4/ipc/check_did -uid %s -keySec %s", CLOUDAPI_PATH, g_cloud_info.g_mmap_info_ptr->api_server, g_cloud_info.g_mmap_info_ptr->p2pid, g_cloud_info.g_mmap_info_ptr->key);
    dump_string(_F_, _FU_, _L_, "cmd=%s\n", cmd);
    system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string),2 );
    dump_string(_F_, _FU_, _L_, "%s\n", ret_string);
    memset(code, 0, sizeof(code));
    if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE)ret = 1;
#else
    char cmd[1024] = {0};
    char ret_string[2048] = {0};
    char code[16] = {0};
    char timestring[32] = {0};
    char http_api_server[64] = {0};
    time_t stamp = 0;
    int trycnt = 0;

    char curl_code_string[16] = {0};
    int curl_code = 0;
    static int pre_fail_flag = 0;

    if((1 == g_cloud_info.g_mmap_info_ptr->time_sync) && (0 == pre_fail_flag))
    {
        snprintf(http_api_server, sizeof(http_api_server), "%s", g_cloud_info.g_mmap_info_ptr->api_server);
    }
    else
    {
        if(strncmp(g_cloud_info.g_mmap_info_ptr->api_server, "https", 5) == 0)
            snprintf(http_api_server, sizeof(http_api_server), "http://%s", g_cloud_info.g_mmap_info_ptr->api_server + strlen("https://"));
        else
            snprintf(http_api_server, sizeof(http_api_server), "%s", g_cloud_info.g_mmap_info_ptr->api_server);
    }
    sprintf(cmd, "%s -c 136 -url %s/v2/ipc/sync_time ", CLOUDAPI_PATH, http_api_server);
    dump_string(_F_, _FU_, _L_,  "cmd = %s\n", cmd);

    trycnt = 3;
    while(trycnt>0)
    {
        trycnt-- ;
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            memset(timestring, 0, sizeof(timestring));
            if(trans_json_ex_s(timestring, sizeof(timestring), "\"time\"", ret_string) == TRUE)
            {
                stamp = atof(timestring)/1000;
                {
                    pre_fail_flag = 0;
                    dump_string(_F_, _FU_, _L_, "yi_sync_time ok!\n");
                    return 0;
                }
            }
        }

        if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
        {
            if(trans_json_ex_s(curl_code_string, sizeof(curl_code_string), "\"curl_code\"", ret_string) == TRUE)
            {
                curl_code = atoi(curl_code_string);
            }
        }

        ms_sleep(1000*3);
    }

    if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
    {
        if(curl_code > 0)
        {
            g_bind_errno = curl_code;
        }
        else
        {
            g_bind_errno = BIND_ERRNO_SYN_TIME_FAIL;
        }
    }
    pre_fail_flag = 1;
    dump_string(_F_, _FU_, _L_, "yi_sync_time fail!\n");
    return -1;
#endif
}

void *msg_proc(void * arg)
{
    fd_set fdset;
    char msg_buffer[MQ_MAX_MSG_SIZE] = {0};
    COM_MSGHD_t *msg_head = (COM_MSGHD_t *)msg_buffer;
#ifdef ENABLE_4G
    sim_card_t sim_card = {0};
#endif
    int rssi;

    while(1)
    {
        FD_ZERO(&fdset);
        FD_SET(g_cloud_info.mqfd_cloud, &fdset);

        if(select(1+g_cloud_info.mqfd_cloud, &fdset, 0, 0, NULL) >= 0)
        {
            if(FD_ISSET(g_cloud_info.mqfd_cloud, &fdset))
            {
                if(mq_receive(g_cloud_info.mqfd_cloud, msg_buffer, sizeof(msg_buffer), 0) > 0)
                {
                    switch(msg_head->mainOperation)
                    {
                    case CLOUD_START_PANORAMA_CAPTURE:
                        panorama_capture_push_abort = 0;
                        sem_post(&panorama_capture_sem);
                        break;

                    case CLOUD_ABORT_PANORAMA_CAPTURE:
                        panorama_capture_push_abort = 1;
                        break;

                    case CLOUD_REPORT_BAYCRY:
                        report_baby_cry = 1;
                        cloud_set_babycry_occur();
                        break;
                    case CLOUD_REPORT_ABNORMAL_SOUND:
                        report_abnormal_sound = 1;
                        cloud_set_abnormal_sound_occur();
                        break;
                    case CLOUD_REPORT_BIGGER_MOVE:
                        report_bigger_move = 1;
                        break;
                    case CLOUD_REPORT_RCD:
                        if(0 == report_rcd)
                        {
                            memset(rcd_file_path, 0x00, sizeof(rcd_file_path));
                            memcpy(rcd_file_path, msg_buffer + sizeof(COM_MSGHD_t), msg_head->msgLength);
                            report_rcd = 1;
                            dump_string(_F_, _FU_, _L_, "rcd_file_path is %s, len = %d\n", rcd_file_path, msg_head->msgLength);
                        }
                        break;

                    case CLOUD_DEBUG_ALARM:
                        g_debug_enable[DEBUG_INFO_TYPE_ALARM_E] = 1;
                        break;
                    case CLOUD_DEBUG_OSS:
                        g_debug_enable[DEBUG_INFO_TYPE_OSS_E] = 1;
                        break;
                    case CLOUD_DEBUG_P2P:
                        g_debug_enable[DEBUG_INFO_TYPE_P2P_E] = 1;
                        break;
                    case CLOUD_DEBUG_RMM:
                        g_debug_enable[DEBUG_INFO_TYPE_RMM_E] = 1;
                        break;
#ifdef ENABLE_4G
                    case MODULE_4G_CONNECT_FAIL:
                        dump_string(_F_, _FU_, _L_, "MODULE_4G_CONNECT_FAIL");
                        break;
                    case MODULE_4G_CONNECTED:
                        g_cloud_info.need_recon = 1;//// reconnect cloud
                        dump_string(_F_, _FU_, _L_, "MODULE_4G_CONNECTED");
                        break;
#endif
                    case P2P_CHECK_CLOUD_NET:
                        ///// here check net
                        dump_string(_F_, _FU_, _L_, "P2P_CHECK_CLOUD_NET");
                        if(webapi_do_check_net()!= 0){
                            cloud_send_msg(g_cloud_info.mqfd_dispatch, CLOUD_SET_DISCONNECTED, NULL, 0);
                        }
                        break;
                    default:
                        dump_string(_F_, _FU_, _L_, "unknown msg 0x%04x", msg_head->mainOperation);
                    }

                }
            }
        }
    }

    return NULL;
}

int mi_sync_time()
{
    int try_cnt = 0;
    int cnt_down = 0;

    try_cnt = 3;
    while(try_cnt)
    {
        try_cnt--;
        if(miio_send(NULL, 0, 1) == 0)
        {
            cnt_down = 400;
            while(cnt_down)
            {
                cnt_down--;
                service_handle();

                if(sync_time_over)
                {
                    dump_string(_F_, _FU_, _L_,  "mi_sync_time ok!\n");
                    return 0;
                }
            }
        }
        ms_sleep(1000*1);
    }

    dump_string(_F_, _FU_, _L_,  "mi_sync_time fail!\n");
    return -1;
}

int yi_sync_time()
{
    char cmd[1024] = {0};
    char ret_string[2048] = {0};
    char code[16] = {0};
    char timestring[32] = {0};
    char http_api_server[64] = {0};
    time_t stamp = 0;
    int trycnt = 0;
    int ret = 0;

    char curl_code_string[16] = {0};
    int curl_code = 0;
    static int pre_fail_flag = 0;

    if((1 == g_cloud_info.g_mmap_info_ptr->time_sync) && (0 == pre_fail_flag))
    {
        snprintf(http_api_server, sizeof(http_api_server), "%s", g_cloud_info.g_mmap_info_ptr->api_server);
    }
    else
    {
        if(strncmp(g_cloud_info.g_mmap_info_ptr->api_server, "https", 5) == 0)
            snprintf(http_api_server, sizeof(http_api_server), "http://%s", g_cloud_info.g_mmap_info_ptr->api_server + strlen("https://"));
        else
            snprintf(http_api_server, sizeof(http_api_server), "%s", g_cloud_info.g_mmap_info_ptr->api_server);
    }
    sprintf(cmd, "%s -c 136 -url %s/v2/ipc/sync_time ", CLOUDAPI_PATH, http_api_server);
    dump_string(_F_, _FU_, _L_,  "cmd = %s\n", cmd);

    trycnt = 3;
    while(trycnt>0)
    {
        trycnt-- ;
        system_cmd_withret_timeout(cmd, ret_string, sizeof(ret_string), 10);
        dump_string(_F_, _FU_, _L_, "%s\n", ret_string);

        memset(code, 0, sizeof(code));
        if(trans_json_ex_s(code, sizeof(code), "\"code\"", ret_string) == TRUE && atoi(code) == 20000)
        {
            memset(timestring, 0, sizeof(timestring));
            if(trans_json_ex_s(timestring, sizeof(timestring), "\"time\"", ret_string) == TRUE)
            {
                stamp = atof(timestring)/1000;
                ret = cloud_set_time(stamp);
                if(ret == 0)
                {
                    pre_fail_flag = 0;
                    dump_string(_F_, _FU_, _L_, "yi_sync_time ok!\n");
                    return ret;
                }
            }
        }

        if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
        {
            if(trans_json_ex_s(curl_code_string, sizeof(curl_code_string), "\"curl_code\"", ret_string) == TRUE)
            {
                curl_code = atoi(curl_code_string);
            }
        }

        ms_sleep(1000*3);
    }

    if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1)
    {
        if(curl_code > 0)
        {
            g_bind_errno = curl_code;
        }
        else
        {
            g_bind_errno = BIND_ERRNO_SYN_TIME_FAIL;
        }
    }

    pre_fail_flag = 1;
    dump_string(_F_, _FU_, _L_, "yi_sync_time fail!\n");
    return -1;
}

#if 0
void sync_time()
{
    int ret = 0;

    for(;;)
    {
        if(g_cloud_info.g_mmap_info_ptr->region_id != REGION_CHINA)
        {
            ret = yi_sync_time();
            if(ret == 0)
            {
                break;
            }
        }
        else
        {
            ret = mi_sync_time();
            if(ret == 0)
            {
                break;
            }
        }

        ms_sleep(3000);
    }

    dump_string(_F_, _FU_, _L_,  "sync_time ok!\n");

    return;
}
#endif

void sync_time()
{
    int ret = 0;
    int try_cnt = 0;

    for(;;)
    {
        if(g_cloud_info.g_mmap_info_ptr->start_with_reset == 1 && strlen(g_cloud_info.g_mmap_info_ptr->bind_key) != 0 && try_cnt == 3)
        {
            yi_report_debug_info(DEBUG_INFO_TYPE_BIND_E);
            cloud_set_bind_result(DISPATCH_SET_BIND_TIMEOUT);
            while(1)
            {
                sleep(10);
            }
        }

        ret = yi_sync_time();
        if(ret == 0)
        {
            break;
        }
        else
        {
#ifndef AP_MODE
            if(try_cnt == 1 && 1 != g_cloud_info.g_mmap_info_ptr->start_with_reset ){
                //// report net wifi disconnect to dispatch
                // g_dispatch.mmap->wifi_connected = 0;
                // CLOUD_SET_DISCONNECTED
                cloud_send_msg(g_cloud_info.mqfd_dispatch, CLOUD_SET_DISCONNECTED, NULL, 0);
                try_cnt = 0;
            }
#endif
            cloud_check_ethernet();
        }
        ms_sleep(3000);

        try_cnt++;
    }

    dump_string(_F_, _FU_, _L_,  "sync_time ok!\n");

    return;
}


int sys_init()
{
    sem_init(&motion_sem, 0, 0);
    sem_init(&snap_sem, 0, 0);

    memset_s(&miio_handle_table, sizeof(miio_handle)*MAX_HANDLE_NUM, 0, sizeof(miio_handle)*MAX_HANDLE_NUM);

    memset_s(&g_cloud_info, sizeof(api_s), 0, sizeof(api_s));

    dump_string(_F_, _FU_, _L_, "open share mem ok\n");

    if(init_mqueue(&(g_cloud_info.mqfd_dispatch), MQ_NAME_DISPATCH) != 0)
    {
        dump_string(_F_, _FU_, _L_,  "init_mqueue dispatch fail!\n");
        return -1;
    }

    if(init_mqueue(&(g_cloud_info.mqfd_cloud), MQ_NAME_CLOUD) != 0)
    {
        dump_string(_F_, _FU_, _L_,  "init_mqueue cloud fail!\n");
        return -1;
    }

    while((g_cloud_info.g_mmap_info_ptr = (mmap_info_s*)get_sharemem(MMAP_FILE_NAME, sizeof(mmap_info_s))) == NULL)
    {
        ms_sleep(1000);
    }


    for(;;)
    {
        if(1==g_cloud_info.g_mmap_info_ptr->init_finish)
        {
            break;
        }
        ms_sleep(100);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    pthread_t yi_motion_push_proc_thread = 0;
    pthread_t msg_proc_thread = 0;
    pthread_t panorama_capture_push_proc_thread = 0;
    pthread_t debug_log_proc_thread = 0;

    if(sys_init() < 0)
    {
        return -1;
    }

    if(pthread_create(&msg_proc_thread, NULL, msg_proc, NULL) < 0)
    {
        dump_string(_F_, _FU_, _L_, "msg_proc failed");
        perror("msg_proc failed");
        //return -1;
    }

    for(;;)
    {
#ifdef ETH_NET_BIND
        if(0 == g_cloud_info.g_mmap_info_ptr->wifi_connected && 0 == g_cloud_info.g_mmap_info_ptr->start_with_reset)
            cloud_check_ethernet();
#endif
        if(0!=g_cloud_info.g_mmap_info_ptr->wifi_connected)
        {
            break;
        }
        ms_sleep(1000);
    }

    sync_time();

    if(pthread_create(&panorama_capture_push_proc_thread, NULL, panorama_capture_push_proc, NULL) < 0)
    {
        dump_string(_F_, _FU_, _L_, "panorama_capture_push_proc failed");
        perror("panorama_capture_push_proc failed");
        //return -1;
    }

    if(pthread_create(&yi_motion_push_proc_thread, NULL, yi_motion_push_proc, NULL) < 0)
    {
        dump_string(_F_, _FU_, _L_, "yi_motion_push_proc failed");
        perror("yi_motion_push_proc failed");
        //return -1;
    }

    #if 0
    if(pthread_create(&debug_log_proc_thread, NULL, debug_log_report_proc, NULL) < 0)
    {
        dump_string(_F_, _FU_, _L_, "debug_log_report_proc failed");
        perror("debug_log_report_proc failed");
        //return -1;
    }
    #endif

    yi_proc();

    return 0;
}

