## Aio-demo
### 异步IO小程序:smile:

1. 支持多线程，每个线程开启一个AIO事件循环
2. 将一个文件的读写，根据请求下标i，采用i%thread_num的方式分配到某个线程的AIO事件循环上监听

### 编译 & 运行

1. 编译  gcc aio_mul_thread.c -o aio -lpthread -laio
2. 运行 valgrind --leak-check=yes ./aio -h
