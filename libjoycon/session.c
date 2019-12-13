#include <assert.h>
#include "defs.h"
#include "task.h"
#include "session.h"
#include "input_report.h"
#include "output_report.h"

#define assert_session(session) assert(session && session->recv && session->send)

struct Session
{
    Recv *recv;
    Send *send;
    InputReport_t *input;
    OutputReport_t *output;
    Device_t host;
    int polling;
    pthread_t poll_thread;
    pthread_rwlock_t input_lock;
    pthread_rwlock_t output_lock;
    TaskHead_t tasks;
};

Session_t *Session_create(Device_t *host, Recv *recv, Send *send)
{
    _FUNC_;
    Session_t *session;
    assert(host && recv && send);
    session = (Session_t *)malloc(sizeof(Session_t));
    if (!session)
        goto done;
    bzero(session, sizeof(Session_t));
    session->recv = recv;
    session->send = send;
    memmove(&session->host, host, sizeof(Device_t));
    session->output = createOutputReport(NULL);
    if (!session->output)
        goto free;
    session->input = createInputReport(NULL);
    if (!session->input)
        goto free;
    if (0 != pthread_rwlock_init(&session->input_lock, NULL))
        goto free;
    if (0 != pthread_rwlock_init(&session->output_lock, NULL))
        goto free;
    task_head_init(&session->tasks);
    return session;

free:
    Session_release(session);
done:
    perror("error when create session\n");
    return NULL;
}

void Session_release(Session_t *session)
{
    _FUNC_;
    if (session)
    {
        if (session->polling || session->poll_thread)
        {
            printf("polling, try to cancel it...\n");
            if (pthread_cancel(session->poll_thread) < 0)
            {
                perror("pthread_cancel\n");
                exit(-1);
            }
            pthread_join(session->poll_thread, NULL);
            pthread_rwlock_destroy(&session->input_lock);
            pthread_rwlock_destroy(&session->output_lock);
            Task_t *task = (Task_t *)session->tasks.head.next;
            while (task)
            {
                Task_t *next = (Task_t *)task->head.next;
                task_del(&session->tasks, task);
                task_free(task);
                free(task);
                task = next;
            }
            task_head_free(&session->tasks);
            session->poll_thread = 0;
            free(session->output);
            free(session->input);
            printf("release done\n");
        }
    }
    free(session);
}
