#define _GNU_SOURCE
#define __STDC_FORMAT_MACROS
  
#include <stdio.h>
#include <errno.h>
#include <libaio.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <assert.h>

#define AIO_1K (1024)

typedef enum {
    AIO_READ,
    AIO_WRITE,
} aio_op_type_t;

typedef void (*ev_cb_t)(int, int, void*);

typedef struct {
    char   *filename;
    int     block_size;   // 读写块大小
    int     file_size;    // 文件总大小
    aio_op_type_t op;     // 请求类型
    int     thread_num;
} fio_param_t;

typedef struct {
    io_context_t  ctx;
    int           fd;
    int           req_num;  // ev loop 请求次数
    int           idx;
} event_base_t;

typedef struct {
    event_base_t* bases;
    pthread_t*    tids;
} env_t;
    

typedef struct {
    event_base_t *base;
    struct iocb   iocb;
    aio_op_type_t op;   // READ, WRITE
    int           seq;  // 请求编号
    char*         buf;  // 保存分配内存
    int           off;  // 请求偏移
    int           size; // 请求大小
    ev_cb_t       callback;
	void         *arg;
} aio_event_t;

pthread_mutex_t g_mutex;
pthread_cond_t  g_cond;

fio_param_t     g_param;
env_t           g_env;

void aio_callback(int res, int err, void* opaque) 
{
    aio_event_t* ev = (aio_event_t*)opaque;
    
    if(err != 0)
    {
        printf("AIO occured error\n");
        return;
    }
    
    /* 打印所属的事件堆循环, 事件序号 */
    printf("---->>> event_base(%d), ev_seq(%d) <<<---- \n", ev->base->idx, ev->seq);

    /* 请求信息 */
    printf("    [req]: off(%d), size(%d)\n", ev->off, ev->size);

    switch (ev->op) {
    case AIO_READ:
        printf("    op: Read \n");
        break;
    case AIO_WRITE:
        printf("    op: Write \n");
        break;
    default:
        assert(0);
    }

    /* 打印读写信息: note 打印结果少一个字符, 不要惊慌, 被我手动设为\0了 */
    ev->buf[res - 1] = '\0';
    printf("    [rsp]: size(%d), buf(%s) \n", res, ev->buf);

    printf("\n");

    /* 释放内存 */
    free(ev->buf);
}

void
client_usage(void)
{
    printf("-F   test file name                       \n");
    printf("-B   block size { > 0}                    \n");
    printf("-S   file size  { > 0}                    \n");
    printf("-O   operator   {r, w}                    \n");
    printf("-T   thread num { > 0}                    \n");
}

int 
param_default_init()
{    
    g_param.filename = "aio_test_file";
    g_param.block_size = 1024;
    g_param.file_size = 1024 * 10;
    g_param.op = AIO_READ;
    g_param.thread_num = 1;
}

int
parse_cmd(int argc, char *argv[]) 
{
    int ret = 0;
    
    while((ret = getopt(argc, argv, "F:B:S:O:T:")) != -1) {
        switch (ret) {
        case 'F':
            g_param.filename = optarg;
            printf("filename: %s \n", g_param.filename);
            break;
        case 'B':
            g_param.block_size = atoi(optarg);
            if (g_param.block_size <= 0) {
                printf("invalid argument: -B { > 0} \n");
                return -EINVAL;
            }
            if (g_param.block_size % AIO_1K != 0) {
                printf("invalid argument: -B { % 1024 == 0} \n");
                return -EINVAL;
            }
            break;
        case 'S':
            g_param.file_size = atoi(optarg);
            if (g_param.file_size <= 0) {
                printf("invalid argument: -S { > 0} \n");
                return -EINVAL;
            }
            /* 文件尺寸 */
            g_param.file_size = (g_param.file_size % g_param.block_size == 0) ? g_param.file_size 
                    : (g_param.file_size / g_param.block_size + 1) * g_param.block_size;
            break;
        case 'O':
            if (strcmp(optarg, "r") != 0 && strcmp(optarg, "w") != 0) {
                printf("invalid argument: -O is in {r, w} \n");
                return -EINVAL;
            }
            g_param.op = strcmp(optarg, "r") == 0 ? AIO_READ : AIO_WRITE;
            break;
        case 'T':
            g_param.thread_num = atoi(optarg);
            if (g_param.thread_num <= 0) {
                printf("invalid argument: -T { > 0} \n");
                return -EINVAL;
            }
            break;
        default:
			client_usage();
			return -EINVAL;
        }
    }
    return 0;
}

void 
event_base_init(event_base_t* base) 
{
    assert(base);

    /* aio_load */
    memset(&base->ctx, 0, sizeof(base->ctx));
    assert(io_setup(1024, &base->ctx) == 0);  // 成功, return 0

    /* io_open */
    base->fd = open(g_param.filename, O_RDWR | O_CREAT | O_DIRECT, 0644);
    assert(base->fd > 0);
    
    ftruncate(base->fd, g_param.file_size);
}

void event_base_fini(event_base_t* base)
{    
    io_destroy(base->ctx);
    close(base->fd);

    // printf("remove %s \n", base->filename);
    // remove(base->filename);
}

/* 
 * @brief 创建一个aio event请求事件
 * @Note 该接口应该改造下, buf应该由外部申请后, 传入进来
 */
void 
aio_event_assign(aio_event_t* ev, event_base_t* base, 
            int seq, int fd, int off, int size, 
            aio_op_type_t op, ev_cb_t cb, void* arg) 
{
    char *buf = NULL;

    assert(ev);
    
    ev->base = base;

    /* 注解: size一定是getpagesize()的整数倍 */
    posix_memalign((void**)&buf, g_param.block_size, size);
    assert(NULL != buf);
    
    switch (op) {
    case AIO_READ:
        io_prep_pread(&ev->iocb, fd, buf, size, off);
        break;
    case AIO_WRITE:
        memset(buf, '0' + seq % 9, size);
        io_prep_pwrite(&ev->iocb, fd, buf, size, off);
        break;
    default:
        assert(0);
    }
    ev->op = op;
    ev->seq = seq;
    
    ev->buf = buf;
    ev->off = off;
    ev->size = size;
        
    ev->callback = cb;
    ev->arg = arg;

    /* important */
    ev->iocb.data = ev; // io_getevents返回后, io_event->data
}

/* 
 * @brief 发送请求
 */
void 
aio_event_add(aio_event_t* ev) 
{
    int           res  = -1;

    assert(ev);
    
    struct iocb* iocbs = &ev->iocb;
    
    do {
        res = io_submit(ev->base->ctx, 1, &iocbs);
    } while (res == -EAGAIN);
    assert(res == 1);
}

void* thread_func(void *arg) 
{
    struct timespec  tms    = {0, 0};
    struct io_event* events = NULL;  
    int num_event;
    int finished, res;
    int j;
    
    int idx = (*(int*)arg);
    num_event = g_env.bases[idx].req_num;
    g_env.bases[idx].idx = idx;

    printf("thread (%d) start\n", idx);

    pthread_mutex_lock(&g_mutex);
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    
    /* 等待AIO请求的到来 */
    events = (struct io_event*)calloc(num_event, sizeof(struct io_event));
    assert(events);
    
    finished = 0;
    while (finished < num_event) {
        res = io_getevents(g_env.bases[idx].ctx, 1, num_event, events, &tms);
        for (j = 0; j < res; ++j)
        {
            aio_event_t* ev = (aio_event_t*)events[j].data;
            ev->callback(events[j].res, events[j].res2, ev->arg);
        }
        finished += res;
    }

    free(events);
}

void 
env_start()
{
    int i;
    pthread_t* tidp = NULL;

    tidp = g_env.tids;

    for (i = 0; i < g_param.thread_num; i++) {
        pthread_mutex_lock(&g_mutex);
        {
            pthread_create(&tidp[i], NULL, thread_func, &i);
            pthread_cond_wait(&g_cond, &g_mutex);
        }
        pthread_mutex_unlock(&g_mutex);
    }
}

void 
env_fini()
{
    int i;
    
    for (i = 0; i < g_param.thread_num; i++) {
        pthread_join(g_env.tids[i], NULL);
    }

    for (i = 0; i < g_param.thread_num; i++) {
        event_base_fini(&g_env.bases[i]);
    }
    free(g_env.bases);

    free(g_env.tids);
    
    pthread_mutex_destroy(&g_mutex);
    pthread_cond_destroy(&g_cond);
}

void 
env_init()
{
    int i;

    g_env.bases = (event_base_t*)calloc(g_param.thread_num, sizeof(event_base_t));
    assert(g_env.bases);

    g_env.tids = (pthread_t*)calloc(g_param.thread_num, sizeof(pthread_t));
    assert(g_env.tids);

    for (i = 0; i < g_param.thread_num; i++) {
        event_base_init(&g_env.bases[i]);
    }
    
    pthread_mutex_init(&g_mutex, NULL);
    pthread_cond_init(&g_cond, NULL);
}


int 
main(int argc, char *argv[])
{
    int ret = 0;
    int i, j;
    int num_event;
    
    param_default_init(); /* 默认参数 */
    
    ret = parse_cmd(argc, argv);
    if (ret < 0) {
        return ret;
    }

    env_init();

    num_event = g_param.file_size / g_param.block_size;  // AIO请求次数

    /* 构造多个AIO请求 & 按照i, 分发给多个事件循环 event_base */
    aio_event_t* aio_events = (aio_event_t*)malloc(sizeof(aio_event_t) * num_event);
    assert(aio_events);
    
    for (i = 0; i < num_event; ++i) {
        int idx = i % g_param.thread_num;
        int off = i * g_param.block_size;
        /* 构造请求 */
        aio_event_assign(&aio_events[i], &g_env.bases[idx], 
                i, g_env.bases[idx].fd,
                off, g_param.block_size, g_param.op,
                aio_callback, &aio_events[i]);
        /* 发起请求 */
        aio_event_add(&aio_events[i]);
        
        g_env.bases[idx].req_num++;
    }

    env_start();

    env_fini();

    free(aio_events);
    
    return 0;
}



