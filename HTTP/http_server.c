#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

typedef struct sockaddr sockaddr;
typedef struct sockaddr_in sockaddr_in;

#define SIZE (1024 * 10)  // 因为宏只是进行简单的文本替换，所以加上括号，避免出现问题。

// 将 http request封装成一个结构体，利于后续操作
typedef struct HttpRequest
{
  char first_line[SIZE]; // C89不能这样写，但C99可以。 保存首行的缓冲区。
  char *method; 
  char *url;
  char *url_path;
  char *query_string;
  int content_length;
}HttpRequest;

// 每次读一行
int ReadLine(int sock, char buf[], ssize_t max_size) // ssize_t 为有符号的长整型，size_t 为无符号的长整型
{
  // 按行从 socket 中读数据
  // 实际上浏览器发送的请求中换行符可能不一样。
  // 换行符可能有：\n, \r, \r\n
  // 1. 循环从 socket 中读取字符，一次读一个。
  char c='\0';
  ssize_t i=0;  // 描述当前读到的字符应该放到缓冲区的那个下标上。
  while(i<max_size-1)
  {
    ssize_t read_size=recv(sock, &c, 1, 0);
    if(read_size <= 0)
    {
      // 此时认为读取数据失败，即使是recv返回0，也认为失败。
      // 由于此时我们预期是至少能读到换行标记的。此处很可能
      // 是因为收到的报文就是非法的。
      return -1;
    }
    // 2. 如果当前的字符是 \r
    if(c == '\r')
    { 
      // a) 尝试从缓冲区读取下一个字符，判定下一个字符是\n，
      //    就把这种情况处理成 \n
      recv(sock, &c, 1, MSG_PEEK);
      // 选项 MSG_PEEK 表示：原本从内核的缓冲区中读数据时，
      // 规则和生产者-消费者模型类似，即读一个字符就将该字符
      // 从缓冲区中删除掉.但加上该选项后，读到的字符在该缓冲区
      // 中还留着，也就是说下一次调用 recv，读到的还是刚才的字符。
      // 这样做的原因是：我不知道后面的字符是啥，如果下一个读到的字符是'\n'，
      // 删了就删了，但如果后面的是一个非换行符的普通字符，就比较麻烦。
      // 所以加上 MSG_PEEK ，起到探路的功效。
      if(c == '\n')
      {
        // 当前的行分隔符是 \r\n
        // 接下来就把下一个 \n 字符从缓冲区中删掉就可以了。
        recv(sock, &c, 1, 0); 
        // 第四个参数不加选项，默认为0时，表示读数据的同时，
        // 就从缓冲区中将该字符删掉了。
      }
      else 
      { 
        // b) 如果下一个字符是其他字符，就把 \r 修改成 \n(把
        //    \r 和 \n 的情况统一在一起) 
        // 此时行分隔符是 \r,为了方便处理，就把 \r 和 \n 统一
        // 在一起，也就是把 c 中的 \r 改成 \n
        c = '\n';
      }
      
    }
    // 4. 如果当前的字符是其它字符，就把这个字符放到 buf 中
    buf[i++]=c;
    // 3. 如果当前的字符是 \n, 就退出循环，函数结束
    if(c=='\n')
    {
      break;
    }
  } 
  buf[i]='\0'; 
  return 0;
}

// 实现 Split 字符串的切分
// 致命问题：主要是由于 strtok 韩寒诉线程不安全，
// 需要用 strtok_r 函数进行替代。
ssize_t Split(char first_line[], const char* split_char, char* output[])
{
  int output_index=0;
  //char *p=strtok(first_line, split_char);
  // strtok 的说明：
  // 1. 每次调用只能完成一次切分；
  // 2. 第二次调用时从NULL开始，说明该函数内部有
  // 一个静态变量来保存上一次切分后的位置。
  // strtok 存在致命问题：
  // 此时我们写的是一个多线程的程序，所以可能会出现
  // 多个线程同时调用split,就存在线程不安全的情况，所以使用 strtok_r
  
  // 此处的tmp 必须是一个栈上的变量。
  char *tmp=NULL; // 定义一个栈上的变量，用来保存每一次切分后的位置，
                  // 而每一个线程都有自己独立的栈空间，从而解决了线程不安全的问题。
  char *p=strtok_r(first_line, split_char, &tmp);
  while(p != NULL)
  {
    output[output_index++]=p;
    // 后续循环调用额时候，第一个参数要填NULL，
    // 此时函数就会根据上次切分的结果，继续往下切分。
    p=strtok_r(NULL, split_char, &tmp);
  }
  return output_index;
}

// 解析首行
// GET / HTTP/1.1
int ParseFirstLine(char first_line[], char** method_ptr, char** url_ptr)
{                                     // 此处写二级指针的原因是：method 和 url 
                                     //  都是字符串，所以需要得到他们的地址。
  char* tokens[100]={NULL};
  // Split切分完毕后，就会破坏掉原有的字符串，
  // 把其中的分隔符替换成 \0
  ssize_t n = Split(first_line, " ", tokens);
  if(n!=3)
  {
    printf("first_line Split error! n=%d\n", n);
    return -1;
  }
  
  // 返回结果
  *method_ptr = tokens[0];
  *url_ptr = tokens[1];
  return 0;
}

// 解析 query_string 
// 再url中找？，若有？，则？后面的就是query_string；
// 否则，query_string 就不存在
int ParseQueryString(char url[], char **url_path_ptr, char **query_string_ptr)
{
  // 此处 url 没有考虑带域名的情况。
  url_path_ptr = &url; 
  char *p = url;
  for(; *p != '\0'; ++p)
  {
    if(*p == '?')
    {
      // 说明此时 url 中带有 query_string,
      // 先把 ？这个字符替换成 \0
      *p='\0';
      // p 指向？位置，p+1 才是 query_string 的起始位置
      *query_string_ptr = p+1;
      return 0;
    }
  }
  // 如果循环结束，也没找到 ?,此时就认为 url 中不存在
  // query_string, 就让 query_string 指向 NULL 
  *query_string_ptr=NULL;
  return 0;
}

// 处理 header，解析出content_length 
int HandlerHeader(int new_sock, int *content_length_ptr)
{
  char buf[SIZE]={0};
  while(1)
  {
    if(ReadLine(new_sock, buf, sizeof(buf)) < 0 )
    {
      printf("ReadLine failed!\n");
      return -1;
    }
    if(strcmp(buf,"\n")==0)
    {
      // 读到了空行，此时 header 部分就结束了。
      return 0;
    }
    // Content-Length:10
    const char* content_length_str = "Content-Length:";
    if(strncmp(buf, content_length_str, strlen(content_length_str))==0)
    {
      *content_length_ptr=atoi(buf+strlen(content_length_str));

      // 此处代码不应该直接return ，本函数其实有两重含义：
      // 1. 找到 content_length 的值。
      // 2. 把接收缓冲区中收到的数据都读出来，也就是从缓冲区中删除掉，
      //    避免粘包问题。
      // return 0;
    }
  } // end while(1)
}

int Handler404(int new_sock)
{
  // 构造一个错误处理的页面, 实际上是进行字符串的拼接。
  // 严格遵守 HTTP 响应格式
  // 1. 首行
  const char* first_line="HTTP/1.1 404 Not Found\n";
  // 3. 空行
  const char* blank_line="\n";
  // 4. body 
  // body 部分的内容就是 HTML
  const char* body="<head><meta http-equiv=\"content-type\" "
      "content=\"text/html;charset=utf-8\"></head>"
      "<h1>您的页面被喵星人吃掉了！！！</h1>";
  // 2. header 
  // 构造 content_length 部分
  char content_length[SIZE]={0};
  sprintf(content_length, "Content-Length: %lu\n",strlen(body));

  send(new_sock, first_line, strlen(first_line), 0);
  send(new_sock, content_length, strlen(content_length), 0);
  send(new_sock, blank_line, strlen(blank_line), 0);
  send(new_sock, body, strlen(body), 0);
  return 0;
}

// 判断是否是目录
int IsDir(const char* file_path)
{
    struct stat st;
    int ret=stat(file_path, &st);
    if(ret < 0)
    {
        // 此处不是目录
        return 0;
    }
    if(S_ISDIR(st.st_mode))
    {
        // 此处是目录
        return 1;
    }
    return 0;
}

// 
void HandlerFilePath(const char* url_path, char file_path[])
{
  // url_path 是以 / 开头的，所以不需要 wwwroot 之后显式指明 /
  sprintf(file_path, "./wwwroot%s", url_path);
  // 如果 url_path 指向的是目录，就在目录后面拼接上 index.html 
  // 作为默认访问的文件
  // 如何识别 url_path 指向的文件到底是普通文件还是目录呢？
  if(file_path[strlen(file_path)-1] == '/')
  {
      // a) url_path 以 / 结尾，例如：/image/ ，就一定是目录
      strcat(file_path, "index.html");
  }
  else
  {
      // b) url_path 没有以 / 结尾，此时需要根据文件属性来判定是否是目录
      if(IsDir(file_path))
      {
          strcat(file_path, "/index.html");
      }
  }
}

ssize_t GetFileSize(const char* file_path)
{
    struct stat st;
    int ret = stat(file_path, &st);
    if(ret < 0)
    {
        return 0;
    }
    return st.st_size;
}

// 
int WriteStaticFile(int new_sock, const char* file_path)
{
  // 1. 打开文件。如果打开失败，就返回 404。
  int fd=open(file_path, O_RDONLY);
  if(fd < 0)
  {
      perror("open");
      return 404;
  }
  // 2. 构造 http 响应报文。
  const char* first_line = "HTTP/1.1 200 OK\n";
  send(new_sock, first_line, strlen(first_line), 0);
  // 此处如果从一个更严谨的角度考虑，最好还要加上一些 header
  // 此处我们没有写 Content-Length 是因为后面立即关闭了 socket ,
  // 浏览器就能识别出数据应该读到哪里结束。
  const char* blank_line = "\n";
  send(new_sock, blank_line, strlen(blank_line), 0);
  // 3. 读文件内容并且写到 socket 之中。
  // 此处我们采用更高效的 sendfile 来完成文件的传输操作。
  //char c='\0';
  //while(read(new_sock, &c, 1) > 0)
  //{
  //    send(new_sock, &c, 1, 0);
  //}
  ssize_t file_size=GetFileSize(file_path);
  sendfile(new_sock, fd, NULL, file_size);
  // 4. 关闭文件
  close(fd);
  return 200;
}

// 处理静态文件
int HandlerStaticFile(int new_sock, const HttpRequest* req)
{
  // 1. 根据上面解析出的 url_path, 获取到对应的真实文件路径
  // 例如，此时 HTTP 服务器的根目录叫做 ./wwwroot
  // 此时有一个文件 ./wwwroot/image/cat.jpg
  // 在 url 中写 path 就叫做 /image/cat.jpg
  char file_path[SIZE]={0};
  // 根据下面的函数把 /image/101.jpg 转换成了磁盘上的 ./wwwroot/image/cat.jpg
  HandlerFilePath(req->url, file_path);
  // 2. 打开文件，把文件中的内容读取出来，并写入 socket 中。
  int err_code=WriteStaticFile(new_sock, file_path);
  return err_code;
}

int HandlerCGIFather(int new_sock, int father_read, int father_write, const HttpRequest* req)
{ 
  //  a) 如果是 POST 请求，把 body 部分的数据读出来写到管道中,
  //     剩下的动态生成页面的过程都交给子进程来完成.
  if(strcasecmp(req->method, "POST") == 0)
  {
    // 根据 body 的长度来读取多少个字节
    // 注意1：此处不能使用 sendfile ,sendfile 要求目标文件描述符必须是一个 socket .
    // 注意2：此处也不能使用下面的写法，因为有可能导致 body 没有被完全读完，
    //        即使缓冲区足够长，但是 read 很可能被信号打断。
    //char buf[10*1024]={0};
    //read(new_sock, buf, sizeof(buf)-1);
    //write(father_write, buf, req->content_length);
    // 综上所述，比较靠谱的方法还是下面的写法：循环进行 read, 然后校验 read 返回的结果的和
    // 是否达到了 content_length 
    char c='\0';
    int i=0;
    for(; i<req->content_length;++i)
    {
      read(new_sock, &c, 1);
      write(father_write, &c, 1);
    }
  }
  //  b) 构造 HTTP 响应中的首行， header ,空行
  const char* first_line = "HTTP/1.1 200 OK\n";
  send(new_sock, first_line, strlen(first_line), 0);
  // 此处为了简单,暂时先不管 header 
  const char* blank_line = "\n";
  send(new_sock, blank_line, strlen(blank_line), 0);
  //  c) 从管道中读取数据（子进程动态生成的页面），把这个数据写到
  //     socket 之中。
  //     此处也不太方便使用 sendfile ，主要是数据的长度不容易确定。
  char c = '\0';
  while(read(father_read, &c, 1) > 0)
  {
    write(new_sock, &c, 1);
  }
  //  d) 进程等待，回收子进程的资源。
  //  此处如果要进行进程等待，那么最好使用 waitpid, 保证当前线程回收的
  //  子进程就是自己当年创建的那个子进程
  //  更简洁的做法，就是直接使用忽略 SIGCHLD 信号。
  return 200;
}

int HandlerCGIChild(int child_read, int child_write, const HttpRequest* req)
{
  //  a) 设置环境变量（REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH）
  //     如果把上面这几个信息通过管道来告知替换之后的程序，
  //     也是完全可行的。但是此处我们是要遵守 CGI 标准，所以
  //     必须使用环境变量传递以上信息。
  //     注意！：设置环境变量的步骤，不能有父进程来进行。
  //     虽然子进程能够继承父进程的环境变量。由于同一时刻，
  //     会有多个请求，每个请求都在尝试修改父进程的环境变量。
  //     就会产生类似于线程安全的问题。导致子进程不能正确的获取
  //     到这些信息。
  char method_env[SIZE]={0};
  // REQUEST_METHOD=GET
  sprintf(method_env, "REQUEST_METHOD=%s\n", req->method);
  putenv(method_env);
  if(strcasecmp(req->method, "GET") == 0)
  {
    // 设置 QUERY_STRING
    char query_string_env[SIZE]={0};
    sprintf(query_string_env, "QUERY_STRING=%s\n", req->query_string);
    putenv(query_string_env);
  }
  else 
  {
    // 设置 CONTENT_LENGTH
    char content_length_env[SIZE]={0};
    sprintf(content_length_env, "CONTENT_LENGTH=%s\n", req->content_length);
    putenv(content_length_env);
  }

  //  b) 把标准输入和标准输出重定向到管道上。此时 CGI 程序读写
  //     标准输入输出就相当于读写管道。
  dup2(child_read, 0);
  dup2(child_write, 1);
  //  c) 子进程进行程序替换，（应注意程序的替换，只是将代码和数据
  //     进行了替换，而进程还是原来的那个，所以对于前面的重定向操作
  //     是没有任何影响的）。（需要先找到是哪个CGI可执行程序，然后
  //     再使用 exec 函数进行替换）
  //     替换成功之后，动态页面完全交给 CGI 程序进行计算生成。
  //     假设 url_path 值为 /cgi-bin/test 
  //     说明对应的 CGI 程序的路径就是 ./wwwroot/cgi-bin/test 
  char file_path[SIZE]={0};
  HandlerFilePath(req->url_path, file_path); // 文件路径的拼接
  // exec 函数大体分为两类：l 和 v
  // l le lp 
  // v ve vp
  // 第一个参数为可执行程序的路径
  // 第二个参数，argv[0]
  // 第三个参数为NULL，表示命令行参数结束了。
  execl(file_path, file_path, NULL);  
  //exec 成功，无返回值；失败，才会走到下面的过程 
  //  d) 替换失败的错误处理。子进程就是为了替换而生的。
  //  如果替换失败，子进程也就没有存在的必要了。反而如果子进程
  //  继续存在，继续执行父进程原有的代码，就有可能会对父进程
  //  原有的逻辑造成干扰。所以，直接让失败的子进程退出。
  exit(0);
  
}

// 处理动态页面
int HandlerCGI(int new_sock, const HttpRequest* req)
{
  // 1. 创建一对匿名管道
  int fd1[2],fd2[2];
  pipe(fd1);
  pipe(fd2);
  int father_read = fd1[0];
  int child_write = fd1[1];
  int father_write = fd2[0];
  int child_read = fd2[1];
  // 2. 创建子进程 fork 
  pid_t ret = fork();
  if(ret > 0)
  {
    // 3. 父进程核心流程
    // 此处先把不必要的文件描述符关闭掉
    // 为了保证后面的父进程从管道中读数据的时候，
    // read 能够正确返回不阻塞。后面的代码中会循环
    // 从管道中读数据，读到 EOF 就认为读完了，循环退出。
    // 而对于管道而言，必须所有的写端关闭，再进行读，才是读到 EOF。
    // 而这里所有的写端包含父进程的写端和子进程的写端。
    // 子进程的写端会随着子进程的终止而自动关闭。
    // 父进程的写端，就可以在此处，直接关闭。（反正父进程自己也不需要
    // 使用这个写端）
    close(child_read);
    close(child_write);
    HandlerCGIFather(new_sock, father_read, father_write, req);
  }
  else if(ret == 0)
  {
    // 4. 子进程核心流程：
    close(father_read);
    close(father_write);
    
    HandlerCGIChild(child_read, child_write, req);
  }
  else 
  {
    perror("fork");
    goto END;
  }
  
END:
  // 收尾工作
  close(father_read);
  close(father_write);
  close(child_read);
  close(child_write);

  return 404;
}


// 这个函数才是真正的完成一次请求的完整过程
void HandlerRequest(int new_sock)
{
  // 1.读取请求并解析
  //   a) 从 socket 中读出HTTP请求的首行。
  int err_code = 200;
  HttpRequest req;
  memset(&req, 0, sizeof(req));
  if(ReadLine(new_sock, req.first_line, sizeof(req.first_line)-1) < 0)
  {
    printf("ReadLine first_line failed!\n");
    //对于错误的处理情况，统一返回404
    err_code = 404;
    goto END;
    // 构造 404 响应的代码
  }
  //   b) 解析首行，获取到方法，url, 版本号(不用)。
  if(ParseFirstLine(req.first_line, &req.method, &req.url) < 0)
  {                                 // req.method 和 req.url 都是输出型参数
    printf("ParseFirstLine failed! first_line=%s\n", req.first_line);
    err_code = 404;
    goto END;
    // 构造 404 响应的代码
  }
  //   c) 对 url 再进行解析，解析出其中的 url_path, query_string
  if(ParseQueryString(req.url, &req.url_path, &req.query_string) < 0)
  {
    printf("ParseQueryString failed! url=%s\n", req.url);
    err_code = 404;
    goto END;
    // 构造 404 响应的代码
  }
  //   d) 读取并解析 header 部分(此处为了简单，只保留 content_length，
  //      其它的 header 内容就直接丢弃了)。
  if(HandlerHeader(new_sock, &req.content_length) < 0)
  {
    printf("HandlerHeader failed!\n");
    err_code = 404;
    goto END; 
    // 构造 404 响应的代码
  }
  // 2.根据请求的详细情况执行静态页面逻辑还是动态页面逻辑
  //   a) 如果是 GET 请求，并且没有 query_string，就认为是静态页面。
  // Get, geT, gET 
  if(strcasecmp(req.method, "GET")==0 && req.query_string == NULL)
  {
    // 处理静态页面
    err_code=HandlerStaticFile(new_sock, &req);
  }
  //   b) 如果是 GET 请求，并且有 query_string, 就可以根据 query_string
  //      参数的内容来动态计算生成页面了。
  else if(strcasecmp(req.method, "GET")==0 && req.query_string != NULL)
  {
    // 处理动态页面
    err_code=HandlerCGI(new_sock, &req);
  }
  //   c) 如果是 POST 请求，(一般没有query_string),都认为是动态页面。
  else if(strcasecmp(req.method, "POST")==0)
  {
    // 处理动态页面
    err_code=HandlerCGI(new_sock, &req);
  }
  //   d) 既不是GET也不是POST
  else 
  {
    printf("method not support! method=%s\n", req.method);
    err_code = 404;
    goto END;
    // 构造 404 响应的代码
  }
END:
  // 这次请求处理结束的收尾工作
  if(err_code != 200)
  {
    Handler404(new_sock);
  }

  // 此处我们只考虑短链接。短连接的意思是每次客户端(浏览器)
  // 给服务器发送请求之前，都是新建立一个 socket 进行连接。
  // 对于短连接来说，如果响应写完了，就可以关闭 new_sock.
  // 此处由于是服务器主动断开连接，也就进入 TIME_WAIT 状态。
  // 由于服务器可能短时间内就处理了大量的连接，导致服务器上
  // 出现了大量的 TIME_WAIT. 导致服务器没法处理新的连接。
  // 所以需要设置 setsockopt REUSEADDR 来重用 TIME_WAIT 状态
  // 的连接。
  close(new_sock);
  // 文件描述符关闭的前提是：该文件描述符不用了
}

void *ThreadEntry(void *arg)
{
  // 线程入口函数，负责这一次请求的完整过程。
  int new_sock = (int64_t)arg; 
  HandlerRequest(new_sock);
  return NULL;
}

void HttpServerStart(const char* ip, short port)
{
  // 1. 创建 tcp socket 
  int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
  if(listen_sock < 0)
  {
    perror("socket");
    return;
  }

  // 设置 REUSERADDR,处理后面短连接主动关闭 socket 的问题。
  int opt = 1; // 启动 setsockopt 这个功能
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // 2. 绑定端口号
  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(ip);
  addr.sin_port = htons(port);
  int ret = bind(listen_sock, (sockaddr*)&addr, sizeof(addr));
  if(ret < 0)
  {
    perror("bind");
    return;
  }
  // 3. 监听 socket
  ret = listen(listen_sock, 5);
  if(ret < 0)
  {
    perror("listen");
    return;
  }
  printf("HttpServerStart OK\n");
  // 4. 进入循环，处理客户端的连接
  while(1)
  {
    sockaddr_in peer;
    socklen_t len = sizeof(peer);
    
    //static int new_sock = accept(listen_sock, (sockaddr*)&peer, &len);
    // 加上static 虽然会让new_sock 从栈上的局部变量变成静态的全局变量，
    // 但是，要注意，此时我们写的是一个多线程的程序，多个线程操作一个变量
    // 会出现相互覆盖的情况，线程不安全。
    
    int64_t new_sock = accept(listen_sock, (sockaddr*)&peer, &len);
    if(new_sock < 0)
    {
      perror("accept");
      continue;
    }
    // 使用多线程的方式来完成多个连接的并行处理
    pthread_t tid;
    pthread_create(&tid, NULL, ThreadEntry, (void*)new_sock);
    // pthread_create()中的第三个参数是线程入口函数，
    // 第四个参数是线程入口函数的参数。
    // 注意：第四个参数一定不能写成 &new_sock ,
    // 因为 new_sock 是栈上的变量，循环一次，它就会被释放，成为野指针。
    // 若再将其传给arg，并进行 *arg 的话，很可能操作的是一段非法内存，
    // 使得程序崩溃。
    // 
    // 正确的写法是：
    // 1. 传一个堆上的变量。
    // 2. 写成(void*)new_sock, 但是如果是 int new_sock，这里
    // 会报警告（类型长度不匹配:4字节转8字节）（但是要注意，8字节转4字节时，
    // 会发生错误，因为可能会丢失一些信息），为了安全起见，改写成 int64_t new_sock 
    pthread_detach(tid);
  }
}

int main(int argc, char* argv[])
{
  if(argc != 3)
  {
    printf("Usage ./http_server [ip] [port]\n");
    return 1;
  }

  signal(SIGCHLD, SIG_IGN); // 线程共享信号处理函数

  HttpServerStart(argv[1], atoi(argv[2]));
  return 0;
}

