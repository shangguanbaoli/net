#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

int HandlerStaticFile()
{
  return 404;
}

int HandlerCGI()
{
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
  if(strcmp(req.method, "GET")==0 && req.query_string == NULL)
  {
    err_code=HandlerStaticFile();

  }
  //   b) 如果是 GET 请求，并且有 query_string, 就可以根据 query_string
  //      参数的内容来动态计算生成页面了。
  else if(strcmp(req.method, "GET")==0 && req.query_string != NULL)
  {
    err_code=HandlerCGI();
  }
  //   c) 如果是 POST 请求，(一般没有query_string),都认为是动态页面。
  else if(strcmp(req.method, "POST")==0)
  {
    err_code=HandlerCGI();
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
  HttpServerStart(argv[1], atoi(argv[2]));
  return 0;
}

