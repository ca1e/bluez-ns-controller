#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>
#include "defs.h"
#include "session.h"
#include "console.h"
#include "input_report.h"
#include "output_report.h"

#define msleep(t) usleep(1000 * (t))
#define assert_console_session(s)        \
    do                                   \
    {                                    \
        assert(s && s->recv && s->send); \
        assert(s->host.role == CONSOLE); \
    } while (0)

/*extern*/ struct Session
{
    Recv *recv;
    Send *send;
    InputReport_t *input;
    OutputReport_t *output;
    Device_t host;
    int polling;
    pthread_t poll_thread;
    pthread_mutex_t poll_mutex;
};
/*
_STATIC_INLINE_ void _dump_hex(uint8_t *data, size_t len)
{
    if (!data)
        return;
    for (int i = 0; i < len; i++)
        fprintf(stdout, "%02x ", data[i]);
    fprintf(stdout, "\n");
}
*/
_STATIC_INLINE_ int _send(Session_t *s)
{
    _FUNC_;
    //_dump_hex((uint8_t *)s->output, sizeof(*s->output));
    return s->send((uint8_t *)s->output, sizeof(*s->output));
}

_STATIC_INLINE_ int _recv(Session_t *s)
{
    _FUNC_;
    int ret = s->recv((uint8_t *)s->input, sizeof(*s->input));
    if (ret < 0)
    {
        printf("recv error -> %d\n", ret);
        return ret;
    }
    //_dump_hex((uint8_t *)s->input, ret);
    return ret;
}

_STATIC_INLINE_ int _wait_until_timeout(Session_t *s, uint8_t subcmd, size_t timeout)
{
    if (s->polling)
    {
        return 0;
    }
    else
    {
        while (timeout--)
        {
            _recv(s);
            if (s->input->id == 0x21 && s->input->standard.reply.subcmd_id == subcmd)
                return 0;
        }
        printf("[%s] timeout(%ld)\n", __func__, timeout);
        return -ETIMEDOUT;
    }
}

int Console_establish(Session_t *session)
{
    _FUNC_;
    int ret = 0;
    SubCmd_t subcmd = {};
    createCmdOutputReport(session->output, 0x02, NULL, 0);
    ret = _send(session);
    _wait_until_timeout(session, 0x02, 10);

    createCmdOutputReport(session->output, 0x08, NULL, 0);
    ret = _send(session);
    _wait_until_timeout(session, 0x08, 10);

    bzero(&subcmd, sizeof(SubCmd_10_t));
    ((SubCmd_10_t *)&subcmd)->address = 0x00006000;
    ((SubCmd_10_t *)&subcmd)->length = 0x10;
    createCmdOutputReport(session->output, 0x10, &subcmd, sizeof(SubCmd_10_t));
    ret = _send(session);
    _wait_until_timeout(session, 0x10, 10);

    bzero(&subcmd, sizeof(SubCmd_10_t));
    ((SubCmd_10_t *)&subcmd)->address = 0x00006050;
    ((SubCmd_10_t *)&subcmd)->length = 0x0d;
    createCmdOutputReport(session->output, 0x10, &subcmd, sizeof(SubCmd_10_t));
    ret = _send(session);
    _wait_until_timeout(session, 0x10, 10);

    //memset(&subcmd, 0, sizeof(SubCmd_01_t));
    bzero(&subcmd, sizeof(SubCmd_01_t));
    ((SubCmd_01_t *)&subcmd)->subcmd = 0x04;
    ((SubCmd_01_t *)&subcmd)->address = mac_address_le(SwitchConsole.mac_address);
    ((SubCmd_01_t *)&subcmd)->fixed[1] = 0x04;
    ((SubCmd_01_t *)&subcmd)->fixed[2] = 0x3c;
    ((SubCmd_01_t *)&subcmd)->alias = alias(SwitchConsole.name);
    ((SubCmd_01_t *)&subcmd)->extra[0] = 0x68;
    ((SubCmd_01_t *)&subcmd)->extra[2] = 0xc0;
    ((SubCmd_01_t *)&subcmd)->extra[3] = 0x39;
    ((SubCmd_01_t *)&subcmd)->extra[4] = 0x0; // ?
    ((SubCmd_01_t *)&subcmd)->extra[5] = 0x0; // ?
    ((SubCmd_01_t *)&subcmd)->extra[6] = 0x0; // ?
    createCmdOutputReport(session->output, 0x01, &subcmd, sizeof(SubCmd_01_t));
    ret = _send(session);
    _wait_until_timeout(session, 0x01, 10);

    bzero(&subcmd, sizeof(SubCmd_03_t));
    ((SubCmd_03_t *)&subcmd)->poll_type = POLL_STANDARD;
    createCmdOutputReport(session->output, 0x03, &subcmd, sizeof(SubCmd_03_t));
    ret = _send(session);
    _wait_until_timeout(session, 0x03, 10);

    createCmdOutputReport(session->output, 0x04, NULL, 0);
    ret = _send(session);
    _wait_until_timeout(session, 0x04, 10);

    bzero(&subcmd, sizeof(SubCmd_30_t));
    ((SubCmd_30_t *)&subcmd)->player = PLAYER_1;
    ((SubCmd_30_t *)&subcmd)->flash = PLAYER_FLASH_4;
    createCmdOutputReport(session->output, 0x30, &subcmd, sizeof(SubCmd_30_t));
    ret = _send(session);
    _wait_until_timeout(session, 0x30, 10);

    return 0;
}

int Console_suspend(Session_t *session)
{
    _FUNC_;
    int ret = 0;
    SubCmd_t subcmd = {};
    createCmdOutputReport(session->output, 0x02, NULL, 0);
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x02, 10);
    return ret;
}

int Console_abolish(Session_t *session)
{
    _FUNC_;
    int ret = 0;
    SubCmd_06_t subcmd = {};
    subcmd.mode = REPAIR;
    createCmdOutputReport(session->output, 0x06, (SubCmd_t *)&subcmd, sizeof(SubCmd_06_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x06, 10);
    if (session->polling || session->poll_thread)
    {
        ret = pthread_cancel(session->poll_thread);
        if (ret < 0)
        {
            perror("pthread_cancel\n");
            return ret;
        }
        session->poll_thread = 0;
    }
    return ret;
}

int Console_getControllerInfo(Session_t *session, ControllerInfo_t *info)
{
    _FUNC_;
    if (!info)
        return -1;
    int ret = 0;
    createCmdOutputReport(session->output, 0x02, NULL, 0);
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x02, 10);
    *info = *(ControllerInfo_t *)session->input->standard.reply.data;
    printf("info = { fw: %04x , cate: %d , mac: %s }\n", firmware((*info)), info->category, mac_address_str_be(info->mac_address));
    return ret;
}

int Console_getControllerVoltage(Session_t *session, uint16_t *voltage)
{
    _FUNC_;
    int ret = 0;
    createCmdOutputReport(session->output, 0x50, NULL, 0);
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x50, 10);
    *voltage = _u16_le(session->input->standard.reply.data);
    return ret;
}

int Console_getControllerColor(Session_t *session, ControllerColor_t *color)
{
    _FUNC_;
    int ret = 0;
    SubCmd_10_t subcmd = {};
    subcmd.address = 0x00006050;
    subcmd.length = 0x0d;
    createCmdOutputReport(session->output, 0x10, (SubCmd_t *)&subcmd, sizeof(SubCmd_10_t));
    ret = _send(session);
    if (ret < 0)
        return ret;
    ret = _wait_until_timeout(session, 0x10, 10);
    if (ret < 0)
        return ret;
    *color = *((ControllerColor_t *)&session->input->standard.reply.data[5]);
    return ret;
}

int Console_setPlayerLight(Session_t *session, Player_t player, PlayerFlash_t flash)
{
    _FUNC_;
    int ret = 0;
    SubCmd_30_t subcmd = {
        .player = player,
        .flash = flash,
    };
    createCmdOutputReport(session->output, 0x30, (SubCmd_t *)&subcmd, sizeof(SubCmd_30_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x30, 10);
    return ret;
}

int Console_setHomeLight(Session_t *session, uint8_t intensity, uint8_t duration, uint8_t repeat, size_t len, HomeLightPattern_t patterns[])
{
    _FUNC_;
    assert((len > 0 && patterns) || (len == 0 && !patterns));
    int ret = 0;
    SubCmd_38_t subcmd = {
        .base_duration = duration,
        .pattern_count = len & 0xF,
        .repeat_count = repeat,
        .start_intensity = intensity,
        .patterns = home_light_pattern(patterns, len & 0xF),
    };
    createCmdOutputReport(session->output, 0x38, (SubCmd_t *)&subcmd, sizeof(SubCmd_38_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x38, 10);
    return ret;
}

int Console_enableImu(Session_t *session, uint8_t enable)
{
    _FUNC_;
    int ret = 0;
    SubCmd_40_t subcmd = {.enable_imu = (enable & 0x1)};
    createCmdOutputReport(session->output, 0x40, (SubCmd_t *)&subcmd, sizeof(SubCmd_40_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x40, 10);
    return ret;
}

int Console_configImu(Session_t *session, GyroSensitivity_t gs, AccSensitivity_t as, GyroPerformance_t gp, AccBandwidth_t ab)
{
    _FUNC_;
    int ret = 0;
    if (gs > GYRO_SEN_2000DPS)
        gs = GYRO_SEN_DEFAULT;
    if (as > ACC_SEN_16G)
        as = ACC_SEN_DEFAULT;
    if (gp > GYRO_PERF_208HZ)
        gp = GYRO_PERF_DEFAULT;
    if (ab > ACC_BW_100HZ)
        ab = ACC_BW_DEFAULT;
    SubCmd_41_t subcmd = {
        .gyro_sensitivity = gs,
        .acc_sensitivity = as,
        .gyro_performance = gp,
        .acc_bandwidth = ab,
    };
    createCmdOutputReport(session->output, 0x41, (SubCmd_t *)&subcmd, sizeof(SubCmd_41_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x41, 10);
    return ret;
}
//max = 0x20
int Console_readImuRegister(Session_t *session)
{
    _FUNC_;
    int ret = 0;
    SubCmd_43_t subcmd = {
        .address = 0,
        .count = 0,
    };
    createCmdOutputReport(session->output, 0x43, (SubCmd_t *)&subcmd, sizeof(SubCmd_43_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x43, 10);
    return ret;
}

int Console_writeImuRegister(Session_t *session)
{
    _FUNC_;
    int ret = 0;
    SubCmd_42_t subcmd = {
        .address = 0,
        .operation = 0x01,
        .value = 0xFF,
    };
    createCmdOutputReport(session->output, 0x42, (SubCmd_t *)&subcmd, sizeof(SubCmd_42_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x42, 10);
    return ret;
}

int Console_enableVibration(Session_t *session, uint8_t enable)
{
    _FUNC_;
    int ret = 0;
    SubCmd_48_t subcmd = {.enable_vibration = enable & 0x1};
    createCmdOutputReport(session->output, 0x48, (SubCmd_t *)&subcmd, sizeof(SubCmd_48_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x48, 10);
    return ret;
}

static void cleanup(void *arg)
{
    assert(arg);
    Session_t *session = (Session_t *)arg;
    printf("exit poll thread ...\n");
    session->polling = 0;
    session->poll_thread = 0;
}

static void *poll(void *arg)
{
    _FUNC_;
    assert(arg);
    int ret = 0;
    Session_t *session;
    ret = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    ret = pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    session = (Session_t *)arg;
    session->polling = 1;
    printf("enter poll thread ...\n");

    pthread_cleanup_push(cleanup, (void *)session);
    while (1)
    {
        msleep(1000);
        printf("poll\n");
    }
    pthread_cleanup_pop(0);

    cleanup((void *)session);
    pthread_exit(NULL);
    printf("pthread_exit\n");
    return NULL;
}

int Console_testPoll(Session_t *session)
{
    _FUNC_;
    int ret = 0;
    if (session->polling || session->poll_thread)
    {
        ret = pthread_cancel(session->poll_thread);
        if (ret < 0)
        {
            perror("pthread_cancel\n");
            return ret;
        }
        session->poll_thread = 0;
    }
    else
    {
        ret = pthread_create(&session->poll_thread, NULL, poll, (void *)session);
        if (ret < 0)
        {
            perror("pthread_cancel\n");
            return ret;
        }
    }
    return ret;
}

int Console_stopPoll(Session_t *session)
{
    int ret = 0;
    if (session->polling || session->poll_thread)
    {
        printf("polling, try to cancel it...\n");
        if (ret = pthread_cancel(session->poll_thread) < 0)
        {
            perror("pthread_cancel\n");
            return ret;
        }
        pthread_join(session->poll_thread, NULL);
        //pthread_mutex_destroy(&session->poll_mutex);
        session->poll_thread = 0;
        //free(session->output);
        //free(session->input);
        printf("stop poll done\n");
    }
    return ret;
}

int Console_poll(Session_t *session, PollType_t type)
{
    _FUNC_;
    int ret = 0;
    SubCmd_03_t subcmd = {.poll_type = type};
    createCmdOutputReport(session->output, 0x03, (SubCmd_t *)&subcmd, sizeof(SubCmd_03_t));
    ret = _send(session);
    ret = _wait_until_timeout(session, 0x03, 10);
    if (ret < 0)
        return ret;
    if (session->polling || session->poll_thread)
    {
        ret = pthread_cancel(session->poll_thread);
        if (ret < 0)
        {
            perror("pthread_cancel\n");
            return ret;
        }
        session->poll_thread = 0;
    }
    else
    {
        ret = pthread_create(&session->poll_thread, NULL, poll, (void *)session);
        if (ret < 0)
        {
            perror("pthread_cancel\n");
            return ret;
        }
    }
    return ret;
}
