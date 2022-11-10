/*************************************************************************
	> File Name: reactor_single.c
	> Author: JianWei
	> Mail: wj_clear@163.com 
	> Created Time: 2022年11月07日 星期一 16时06分42秒
 ************************************************************************/
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#include "../include/my_misc.h"
#include "../include/zlog.h"
#include "reactor.h"
static char *	g_init_host = "127.1";
static uint16_t g_init_port = 8888;
static uint8_t  g_muti_mode;
static int		ret;
static zlog_category_t* g_zlog_handle;
static reactor_t*		g_reactor;
static int g_sockfds[1024];

#define JUDGE_FUNC_RET(__ret, __proc_mode)\
	do{\
		if(__ret != 0 )\
		{\
			fprintf(stderr ,"function runs return [%d] at line [%d] in file [%s]\n" ,__ret , __LINE__, __FILE__);\
			if(__proc_mode == MISC_EXIT_MODE)\
				exit(0);\
		}\
	}while(0);

static int misc_item_send_cb(int fd, int events, void *arg);

static void show_usage()
{
    puts("usage: exe -h host_ip  -p port (--muti)\n");
    return; 
}

static int deal_with_cmdline(int argc ,char** argv)
{
	if(argc < 3)
    {
        printf("example usage : %s -p port_num\n" , argv[0]);
        exit  (-1);
    }

    int opcode;  ///< 返回的操作码
    int digit_optind = 0;

    while (1) {
        int this_option_optind = optind ? optind : 1;
        int option_index = 0;
        static struct option long_options[] = {
            {"port",	required_argument,  NULL,  'p' },
            {"host",    required_argument,  NULL,  'h' },
            {"muti",    no_argument,        NULL,  'm' },
            {0,         0,                 0,  0 }
        };

        opcode = getopt_long(argc, argv, "h:p:",
                long_options, &option_index);
        if (opcode == -1)
            break;

        switch (opcode) {
        case 0:
            printf("option %s", long_options[option_index].name);
            if (optarg)
                printf(" with arg %s", optarg);
            printf("\n");
            break;

        case 'p':
            g_init_port = atoi(optarg);
            break;
        case 'h':
			g_init_host = optarg;
			break;

		case 'm':
			g_muti_mode = 1;
			break;

        case '?':                                                            
            show_usage();
            break;
        default:
            printf("?? getopt returned character code 0%o ??\n", opcode);
        }
    }
    int tmp;
    if (optind < argc) {
        tmp = optind ;
        printf("non-option ARGV-elements: ");
        while (optind < argc)
            printf("%s ", argv[optind++]);
        printf("\n");
    }

    return argc - tmp;
}

static int misc_zlog_init()
{
	char *zlog_conf_path = "../sys_conf/zlog.conf";
	if(access(zlog_conf_path , F_OK))
	{
		printf("where is my %s\n" ,zlog_conf_path);
		return -1;
	}

	int ret = zlog_init(zlog_conf_path);
	if(ret)
	{
		printf("zlog init failed and return %d\n",ret);
		return -1;
	}
	
	g_zlog_handle = zlog_get_category("reactor_log");
	if(!g_zlog_handle)
	{
		printf("zlog get category failed\n");
		zlog_fini();
		return -1;
	}
	/*zlog init over*/
	return 0;
}

static int misc_reactor_init()
{
	reactor_t* reactor = g_reactor;
	memset(reactor, 0, sizeof(reactor_t));
	reactor->epoll_fd = epoll_create(1);	///< 从Linux 2.6.8开始，该函数入参将被忽略，但必须大于零。
	if (reactor->epoll_fd <= 0) 
	{
        zlog_error(g_zlog_handle ,"create epoll fd in %s err %s\n", __func__, strerror(errno));
        return -1;
    }

	reactor_item_t* tmp_node = (reactor_item_t *)malloc(MAX_EPOLL_EVENTS * sizeof(reactor_item_t));	///< 初始化出第一个响应堆块
	if(tmp_node == NULL)
	{
		zlog_error(g_zlog_handle ,"first event bucket malloc failed");
		goto exit;
	}
	memset(tmp_node , 0 , MAX_EPOLL_EVENTS * sizeof(reactor_item_t));

	reactor->list_header = malloc(sizeof(event_list_node_t));
	if(!reactor->list_header)
	{
		zlog_error(g_zlog_handle ," malloc list_header failed");
		goto exit;
	}

	reactor->list_header->item_bucket = tmp_node;
	reactor->list_header->next = NULL;
	reactor->block_count = 1;

	return 0;

exit:
	free(g_reactor);
	if(tmp_node)
		free(tmp_node);
	close(reactor->epoll_fd);
	return -1;
}

static int misc_init_sock(uint16_t port)
{
	int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(sock_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in server_addr = {0}; 
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);	///< 支持任意接入
    server_addr.sin_port = htons(port);

    bind(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (listen(sock_fd, 20) < 0) 
	{
        zlog_error(g_zlog_handle , "listen failed : %s\n", strerror(errno));
        return -1;
    }

    zlog_info(g_zlog_handle , "listen server port : %d\n", port);
	
	int reuse = 1;
	setsockopt(sock_fd ,SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));	///< 端口快速重用

	return sock_fd;
}

static int misc_reactor_alloc_block(reactor_t* reactor)
{
	event_list_node_t *tmp_node = reactor->list_header;
	while(tmp_node->next != NULL)	///< 找到fd块管理链表最后一个节点,使用尾插法
		tmp_node = tmp_node->next;

	event_list_node_t *final_node = malloc(sizeof(event_list_node_t));	///< 新添加一个事务节点
	if(unlikely(final_node == NULL))
	{
		zlog_error(g_zlog_handle ,"block manage node malloc  failed");
		return -1;
	}
	memset(final_node , 0 , sizeof(event_list_node_t));

	final_node->item_bucket = malloc((MAX_EPOLL_EVENTS) * sizeof(reactor_item_t));	///< 指向item 的 block
	if(unlikely(final_node->item_bucket == NULL))
	{
		zlog_error(g_zlog_handle ,"block malloc failed");
		return -1;
	}
	memset(final_node->item_bucket , 0 , (MAX_EPOLL_EVENTS) * sizeof(reactor_item_t));

	tmp_node->next = final_node;
	reactor->block_count ++;
	
	return 0;
}

static force_inline reactor_item_t* find_itme_in_reactor(reactor_t* reactor , int sockfd)
{
	int buctet_index = sockfd / MAX_EPOLL_EVENTS;	///< 计算所在的fd块下标
	while(unlikely(buctet_index >= reactor->block_count))
		misc_reactor_alloc_block(reactor);
	
	int i = 0 ;

	event_list_node_t *tmp_node = reactor->list_header;
	while(i++ != buctet_index && tmp_node != NULL)
		tmp_node = tmp_node->next;

	return &(tmp_node->item_bucket[sockfd % MAX_EPOLL_EVENTS]);	///< 从对应的块中找到目标item并返回
}

static force_inline void misc_item_init(reactor_item_t *item , int sockfd , misc_event_callback* event_proc, reactor_t* reactor)
{
	item->fd = sockfd;
	item->call_back_func = event_proc;
	item->events = 0;
	item->arg	= reactor;
	
	return ;
}


static void misc_item_add(int epoll_fd , int events , reactor_item_t *item)
{
	struct epoll_event ep_ev = {0, {0}};
	ep_ev.data.ptr = item;
	ep_ev.events = item->events = events;
	int op;

	if(unlikely(item->status == 1))
		op = EPOLL_CTL_MOD;
	else 
	{
		op = EPOLL_CTL_ADD;
		item->status = 1;
	}

	if (epoll_ctl(epoll_fd, op, item->fd, &ep_ev) < 0)
		zlog_error(g_zlog_handle ,"event add failed [fd=%d], events[%d]\n", item->fd, events);
	
	return ;
}

static int misc_reactor_add_listener(reactor_t* reactor , int sockfd , misc_event_callback* event_proc)
{
	if(unlikely(reactor == NULL))
	{
		zlog_error(g_zlog_handle , "reactor is invalid");
		return -1;
	}
	
	if(unlikely(reactor->list_header == NULL))
	{
		zlog_error(g_zlog_handle , "reactor's bucket node  is invalid");
		return -1;
	}

	reactor_item_t *item_worker = find_itme_in_reactor(reactor , sockfd);
	if(unlikely(item_worker == NULL))
	{
		zlog_error(g_zlog_handle , "fisst itme  node find failed");
		return -1;
	}

	misc_item_init(item_worker , sockfd, event_proc , reactor);	///< item 初始化
	misc_item_add(reactor->epoll_fd , EPOLLIN , item_worker);	///< item 添加到epoll

	return 0;
}

static int epoll_item_del(int epfd, reactor_item_t *ev)
{
	struct epoll_event ep_ev = {0, {0}};
    if (ev->status != 1) 
        return -1;  

    ep_ev.data.ptr = ev;
    ev->status = 0;
    epoll_ctl(epfd, EPOLL_CTL_DEL, ev->fd, &ep_ev);
	
	return 0;
}

static int misc_base64_encode(char *in_str, int in_len, char *out_str) 
{
    BIO *b64, *bio;    
    BUF_MEM *bptr = NULL;    
    size_t size = 0;    

    if (in_str == NULL || out_str == NULL)        
        return -1;    

    b64 = BIO_new(BIO_f_base64());    
    bio = BIO_new(BIO_s_mem());    
    bio = BIO_push(b64, bio);
    
    BIO_write(bio, in_str, in_len);    
    BIO_flush(bio);    

    BIO_get_mem_ptr(bio, &bptr);    
    memcpy(out_str, bptr->data, bptr->length);    
    out_str[bptr->length-1] = '\0';    
    size = bptr->length;    

    BIO_free_all(bio);    
    return size;
}


static int readline_in_linebuf(char* allbuf ,int idx ,char* linebuf) 
{
    int len = strlen(allbuf);    
	int i ;
	for( ; i < len - 2 ; i++) 
	{
		if(memcmp(&allbuf[i] , "\r\n" , 2 ) == 0)
			return (i + 2);
		else 
			*(linebuf++) = allbuf[idx];
	}

    return -1;
}

static int ws_handshark(reactor_item_t *ev)
{
    int idx = 0;
    char sec_data[128] = {0};
    char sec_accept[128] = {0};
	char linebuf[1024];

    do {
		bzero(linebuf , sizeof(linebuf));
        idx = readline_in_linebuf(ev->rbuffer, idx, linebuf);	///< 将\r\n切分出的一行写入目标buffer

        if (strstr(linebuf, "Sec-WebSocket-Key")) 
		{
            strcat(linebuf, MISC_GUID);
            SHA1(linebuf+19, strlen(linebuf+19), sec_data);
            misc_base64_encode(sec_data, strlen(sec_data), sec_accept);      

            zlog_info(g_zlog_handle ,"idx: %d, line: %ld", idx, sizeof("Sec-WebSocket-Key: "));
            memcpy(ev->sec_accept, sec_accept, ACCEPT_KEY_LENGTH);
        }

    } while((ev->rbuffer[idx] != '\r' || ev->rbuffer[idx+1] != '\n') && idx != -1);
	
	return 0;
}

static  force_inline void umask(char *payload, int length, char *mask_key) 
{
    int i = 0;
    for (i = 0;i < length;i ++) 
        payload[i] ^= mask_key[i%4];

}


static int ws_tranmission(reactor_item_t *ev)
{
	struct ws_ophdr *hdr = (struct ws_ophdr *)ev->rbuffer;

    if (hdr->pl_len < 126) {

        unsigned char *payload = NULL;
        if (hdr->mask) 
		{
            payload = ev->rbuffer + 6;
        
            umask(payload, hdr->pl_len, ev->rbuffer + 2);
        } 
		else 
		{
            payload = ev->rbuffer + 2;
        }

        zlog_info(g_zlog_handle ,"payload: %s", payload);
    
    } 
	else if (hdr->pl_len == 126) 
	{
        
    } 
	else if (hdr->pl_len == 127)  
	{

    } 
	else 
	{
        //assert(0);
    }
	
}

/*处理http request内容*/
static force_inline int ws_request(reactor_item_t *ev)
{
	if (ev->wsstatus == WS_HANDSHARK)	///< 处理握手包
	{
        ws_handshark(ev);
        ev->wsstatus = WS_TRANMISSION;
    } 
	else if (ev->wsstatus == WS_TRANMISSION)	///< 处理data包
        ws_tranmission(ev);
	else 
		;
	return 0;
}

/*写入http repose内容*/
static int  ws_response(reactor_item_t *ev)
{
   ev->wlength = sprintf(ev->wbuffer, "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n\r\n", ev->sec_accept);
   zlog_info(g_zlog_handle ,"response: %s", ev->wbuffer);
   return ev->wlength;
   


	return 0;
}



static int misc_item_recv_cb(int fd, int events, void *arg) 
{
    reactor_t *reactor = (reactor_t*)arg;
    reactor_item_t *ev = find_itme_in_reactor(reactor, fd);                   
    if (unlikely(ev == NULL)) return -1;

    int len = recv(fd, ev->rbuffer, ITEM_BUFFER_LENGTH, 0);
    epoll_item_del(reactor->epoll_fd, ev);	///< 删除该事件

    if (len > 0) 
	{
        ev->rlength = len;
        ev->rbuffer[len] = '\0';

        ws_request(ev);		///< 处理请求内容


        misc_item_init(ev, fd, misc_item_send_cb, reactor);
        misc_item_add(reactor->epoll_fd, EPOLLOUT, ev);
    } 
	else if (len == 0) 
	{
        epoll_item_del(reactor->epoll_fd, ev);
        zlog_info(g_zlog_handle, "recv_cb --> disconnect\n");
        close(ev->fd);
    } 
	else 
	{
        if (errno == EAGAIN && errno == EWOULDBLOCK) 
		{ //
            
        } 
		else if (errno == ECONNRESET)
		{
            epoll_item_del(reactor->epoll_fd, ev);
            close(ev->fd);
        }
        zlog_info(g_zlog_handle ,"recv[fd=%d] error[%d]:%s\n", fd, errno, strerror(errno));
    }
    return len;
}

static int misc_item_send_cb(int fd, int events, void *arg) 
{

    reactor_t *reactor = (reactor_t*)arg;
    reactor_item_t *ev = find_itme_in_reactor(reactor, fd);

    if (ev == NULL) return -1;

    ws_response(ev);
    

    int len = send(fd, ev->wbuffer, ev->wlength, 0);
    if (len > 0) {
        zlog_info(g_zlog_handle ,"send[fd=%d], [%d]%s\n", fd, len, ev->wbuffer);

        epoll_item_del(reactor->epoll_fd, ev);
        misc_item_init(ev, fd, misc_item_recv_cb, reactor);
        misc_item_add(reactor->epoll_fd, EPOLLIN, ev);
        
    } else {

        epoll_item_del(reactor->epoll_fd, ev);
        close(ev->fd);

		zlog_info(g_zlog_handle ,"send[fd=%d] error %s\n", fd, strerror(errno));

    }

    return len;
}



static int accept_cb(int fd, int events, void *arg)
{
    reactor_t *reactor = (reactor_t*)arg;
    if (unlikely(reactor == NULL)) return -1;

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);

    int clientfd;

    if ((clientfd = accept(fd, (struct sockaddr*)&client_addr, &len)) == -1) 
	{
        if (errno != EAGAIN && errno != EINTR) 
		{
            
        }
        zlog_error(g_zlog_handle ,"accept: %s", strerror(errno));
        return -1;
    }

    
    int flag = 0;
    if ((flag = fcntl(clientfd, F_SETFL, O_NONBLOCK)) < 0) 
	{
        zlog_error(g_zlog_handle ,"%s: fcntl nonblocking failed, %d", __func__, MAX_EPOLL_EVENTS);
        return -1;
    }

    reactor_item_t  *work_item = find_itme_in_reactor(reactor, clientfd);

    if (work_item == NULL) return -1;
        
    misc_item_init(work_item, clientfd, misc_item_recv_cb, reactor);	
    misc_item_add(reactor->epoll_fd, EPOLLIN, work_item);

    zlog_info(g_zlog_handle ,"new connect [%s:%d], pos[%d]", 
        inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), clientfd);

	return 0;
}

static int misc_comm_init()
{
	if(misc_zlog_init())
		return -1;

	g_reactor =(typeof(g_reactor))malloc(sizeof(reactor_t));
	assert(g_reactor);

	if(misc_reactor_init())
		return -1;
	
	const uint8_t add_listrner_num = g_muti_mode == 1 ? 64 : 0;	///< 根据是否开启海量接入模式的判断要增加监听套接字的数量
	int i = 0 ,tmp_sock_fd;
	for( ; i <= add_listrner_num ; i++)
	{
		g_sockfds[i] = misc_init_sock(g_init_port + i);		///< 根据要服务的端口号初始化套接字
		if(likely(g_sockfds[i] > 0))
			misc_reactor_add_listener(g_reactor , g_sockfds[i] , accept_cb);	///< 初始化成功则加入响应池
	}

	return 0;
}

static void misc_reactor_destory(reactor_t *reactor)
{
    close(reactor->epoll_fd);
    
    event_list_node_t *blk = reactor->list_header;
    event_list_node_t *blk_next;
    while (blk != NULL) {
        blk_next = blk->next;

        free(blk->item_bucket);
        free(blk); 

        blk = blk_next;
    }

	const uint8_t add_listrner_num = g_muti_mode == 1 ? 64 : 0;
	int i;
	for (i = 0 ; i < add_listrner_num ; i++) 
		close(g_sockfds[i]);
	
	free(reactor);

	return ;
}

static int misc_reactor_run(reactor_t *reactor) 
{
    
    struct epoll_event events[MAX_EPOLL_EVENTS + 1];
    int checkpos = 0, i , nready;
	reactor_item_t *work_item;

	printf("http reactor is running at port %d\n",g_init_port);
    while (1) {

        nready = epoll_wait(reactor->epoll_fd, events, MAX_EPOLL_EVENTS, 1000);
        if (nready < 0) {
            zlog_error(g_zlog_handle ,"epoll_wait error");
            continue;
        }

        for (i = 0; i < nready; i ++) {

            work_item = (reactor_item_t*)events[i].data.ptr;

            if ((events[i].events & EPOLLIN) && (work_item->events & EPOLLIN)) {
                work_item->call_back_func(work_item->fd, events[i].events, work_item->arg);
            }
            if ((events[i].events & EPOLLOUT) && (work_item->events & EPOLLOUT)) {
                work_item->call_back_func(work_item->fd, events[i].events, work_item->arg);
            }
        }
    }
}



int main(int argc, char *argv[])
{
	deal_with_cmdline(argc , argv);   ///< 处理命令行完毕
	
	ret = misc_comm_init();	///< 资源初始化
	JUDGE_FUNC_RET(ret , MISC_EXIT_MODE);	
	
	misc_reactor_run(g_reactor);

	//misc_reactor_destory(g_reactor);
	
	return 0;
}

